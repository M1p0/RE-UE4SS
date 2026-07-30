#define NOMINMAX

#include <Windows.h>

#ifdef TEXT
#undef TEXT
#endif

#include <MCP/MCPServer.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <exception>
#include <format>
#include <future>
#include <memory>
#include <sstream>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Helpers/String.hpp>
#include <LuaMadeSimple/LuaMadeSimple.hpp>
#include <Mod/LuaMod.hpp>
#include <UE4SSProgram.hpp>
#include <Unreal/FOutputDevice.hpp>
#include <Unreal/Hooks.hpp>
#include <Unreal/TypeChecker.hpp>
#include <Unreal/UAssetRegistry.hpp>
#include <Unreal/UAssetRegistryHelpers.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealVersion.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <glaze/glaze.hpp>

namespace RC::MCP
{
    namespace
    {
        constexpr DWORD BUFFER_SIZE = 64 * 1024;
        constexpr auto REQUEST_TIMEOUT = std::chrono::seconds{10};

        auto json_escape(std::string_view input) -> std::string
        {
            std::string output{};
            output.reserve(input.size() + 2);

            for (const unsigned char c : input)
            {
                switch (c)
                {
                case '"':
                    output += "\\\"";
                    break;
                case '\\':
                    output += "\\\\";
                    break;
                case '\b':
                    output += "\\b";
                    break;
                case '\f':
                    output += "\\f";
                    break;
                case '\n':
                    output += "\\n";
                    break;
                case '\r':
                    output += "\\r";
                    break;
                case '\t':
                    output += "\\t";
                    break;
                default:
                    if (c < 0x20)
                    {
                        output += std::format("\\u{:04x}", static_cast<int>(c));
                    }
                    else
                    {
                        output += static_cast<char>(c);
                    }
                    break;
                }
            }

            return output;
        }

        auto json_string(std::string_view input) -> std::string
        {
            return std::format("\"{}\"", json_escape(input));
        }

        auto bool_json(bool value) -> const char*
        {
            return value ? "true" : "false";
        }

        auto to_lower_ascii(std::string value) -> std::string
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        }

        auto parse_object(std::string_view json) -> glz::generic::object_t
        {
            glz::generic root{};
            const auto parse_result = glz::read_json(root, json);
            if (parse_result.ec != glz::error_code::none)
            {
                throw std::runtime_error{"invalid_json"};
            }
            return root.get<glz::generic::object_t>();
        }

        auto write_generic_json(const glz::generic& value) -> std::string
        {
            const auto written = glz::write<glz::opts{}>(value);
            if (!written.has_value())
            {
                throw std::runtime_error{"unable_to_write_json"};
            }
            return written.value();
        }

        auto get_string(const glz::generic::object_t& object, std::string_view key, std::string default_value = {}) -> std::string
        {
            const auto it = object.find(std::string{key});
            if (it == object.end())
            {
                return default_value;
            }
            try
            {
                return it->second.get<std::string>();
            }
            catch (...)
            {
                return default_value;
            }
        }

        auto get_bool(const glz::generic::object_t& object, std::string_view key, bool default_value = false) -> bool
        {
            const auto it = object.find(std::string{key});
            if (it == object.end())
            {
                return default_value;
            }
            try
            {
                return it->second.get<bool>();
            }
            catch (...)
            {
                return default_value;
            }
        }

        auto get_size(const glz::generic::object_t& object, std::string_view key, size_t default_value) -> size_t
        {
            const auto it = object.find(std::string{key});
            if (it == object.end())
            {
                return default_value;
            }
            try
            {
                const auto value = it->second.get<double>();
                return value > 0.0 ? static_cast<size_t>(value) : 0;
            }
            catch (...)
            {
                return default_value;
            }
        }

        auto get_array_strings(const glz::generic::object_t& object, std::string_view key) -> std::vector<std::string>
        {
            std::vector<std::string> strings{};
            const auto it = object.find(std::string{key});
            if (it == object.end())
            {
                return strings;
            }

            try
            {
                for (const auto& item : it->second.get<glz::generic::array_t>())
                {
                    strings.emplace_back(item.get<std::string>());
                }
            }
            catch (...)
            {
            }
            return strings;
        }

        auto object_is_valid(Unreal::UObject* object) -> bool
        {
            return object && !object->IsUnreachable();
        }

        auto property_to_text(Unreal::UObject* object, Unreal::FProperty* property) -> std::string
        {
            if (!object || !property)
            {
                return {};
            }

            Unreal::FString property_text{};
            const auto value_container = property->ContainerPtrToValuePtr<void>(object);
            property->ExportTextItem(property_text, value_container, value_container, object, NULL);
            return to_string(*property_text);
        }

        auto find_property(Unreal::UObject* object, std::string_view property_name) -> Unreal::FProperty*
        {
            if (!object)
            {
                return nullptr;
            }

            auto* obj_as_struct = Unreal::Cast<Unreal::UStruct>(object);
            if (!obj_as_struct)
            {
                obj_as_struct = object->GetClassPrivate();
            }

            return obj_as_struct ? obj_as_struct->FindProperty(Unreal::FName(ensure_str(property_name), Unreal::FNAME_Find)) : nullptr;
        }

        auto find_function_by_path(std::string_view function_path) -> Unreal::UFunction*
        {
            if (function_path.empty())
            {
                return nullptr;
            }

            if (auto* exact_object = Unreal::UObjectGlobals::StaticFindObject_InternalSlow(
                        nullptr,
                        nullptr,
                        ensure_str(function_path).c_str());
                object_is_valid(exact_object))
            {
                if (auto* exact_function = Unreal::Cast<Unreal::UFunction>(exact_object);
                    exact_function && to_string(exact_function->GetPathName()) == function_path)
                {
                    return exact_function;
                }
            }

            auto function_name = std::string{function_path};
            if (const auto separator = function_name.find_last_of(".:"); separator != std::string::npos)
            {
                function_name.erase(0, separator + 1);
            }
            const Unreal::FName resolved_name{ensure_str(function_name), Unreal::FNAME_Find};
            if (resolved_name.IsNone())
            {
                return nullptr;
            }

            const auto object_count = static_cast<int64_t>(Unreal::UObjectArray::GetNumElements());
            for (int64_t index = object_count - 1; index >= 0; --index)
            {
                auto* item = Unreal::FUObjectArray::IndexToObject(static_cast<int32_t>(index));
                auto* candidate = item ? item->GetUObject() : nullptr;
                if (!object_is_valid(candidate) ||
                    !candidate->GetNamePrivate().Equals(resolved_name))
                {
                    continue;
                }
                if (auto* function = Unreal::Cast<Unreal::UFunction>(candidate);
                    function && to_string(function->GetPathName()) == function_path)
                {
                    return function;
                }
            }
            return nullptr;
        }

        auto find_function_without_interfaces(Unreal::UObject* object, const Unreal::FName& function_name) -> Unreal::UFunction*
        {
            if (!object || !object->GetClassPrivate())
            {
                return nullptr;
            }

            // Walk the class and super chain first. Interface traversal is not
            // required for ordinary reflected member functions and can be
            // expensive or unavailable in customized engine builds.
            for (auto* function : Unreal::TFieldRange<Unreal::UFunction>(
                         object->GetClassPrivate(),
                         Unreal::EFieldIterationFlags::IncludeSuper))
            {
                if (function && function->GetNamePrivate().Equals(function_name))
                {
                    return function;
                }
            }

            // Some customized builds expose incomplete UField chains. Try each
            // exact owner path before falling back to an object-array scan.
            const auto function_name_text = to_string(function_name.ToString());
            for (Unreal::UStruct* owner = object->GetClassPrivate();
                 owner;
                 owner = owner->GetSuperStruct())
            {
                auto function_path = to_string(owner->GetPathName());
                function_path += ':';
                function_path += function_name_text;
                auto* candidate = Unreal::UObjectGlobals::StaticFindObject_InternalSlow(
                        nullptr,
                        nullptr,
                        ensure_str(function_path).c_str());
                if (auto* function = Unreal::Cast<Unreal::UFunction>(candidate))
                {
                    Output::send(STR("[MCP] Resolved function by owner path: {}\n"),
                                 function->GetFullName());
                    return function;
                }
            }

            // StaticFindObject may miss functions in customized reflection
            // layouts. The final fallback scans by exact name and owner.
            const auto object_count = static_cast<int64_t>(Unreal::UObjectArray::GetNumElements());
            const auto matches_owner = [&](Unreal::UFunction* function) {
                for (Unreal::UStruct* owner = object->GetClassPrivate();
                     owner;
                     owner = owner->GetSuperStruct())
                {
                    if (function->GetOuterPrivate() == owner)
                    {
                        return true;
                    }
                }
                return false;
            };
            for (int64_t index = object_count - 1; index >= 0; --index)
            {
                auto* item = Unreal::FUObjectArray::IndexToObject(static_cast<int32_t>(index));
                auto* candidate = item ? item->GetUObject() : nullptr;
                if (!object_is_valid(candidate) ||
                    !candidate->GetNamePrivate().Equals(function_name))
                {
                    continue;
                }
                if (auto* function = Unreal::Cast<Unreal::UFunction>(candidate);
                    function && matches_owner(function))
                {
                    Output::send(STR("[MCP] Resolved function in object-array fallback: {}\n"),
                                 function->GetFullName());
                    return function;
                }
            }
            return nullptr;
        }

        auto find_player_controller() -> Unreal::UObject*
        {
            const Unreal::FName player_controller_name{STR("PlayerController"), Unreal::FNAME_Find};
            if (player_controller_name.IsNone())
            {
                Output::send<LogLevel::Warning>(STR("[MCP] PlayerController FName is unavailable.\n"));
                return nullptr;
            }

            const auto object_count = Unreal::UObjectArray::GetNumElements();
            for (int64_t object_index = static_cast<int64_t>(object_count) - 1; object_index >= 0; --object_index)
            {
                auto* object_item = Unreal::FUObjectArray::IndexToObject(static_cast<int32_t>(object_index));
                auto* candidate = object_item ? object_item->GetUObject() : nullptr;
                if (!object_is_valid(candidate) ||
                    candidate->HasAnyFlags(Unreal::EObjectFlags::RF_ClassDefaultObject))
                {
                    continue;
                }

                bool is_player_controller{};
                for (Unreal::UStruct* candidate_class = candidate->GetClassPrivate();
                     candidate_class;
                     candidate_class = candidate_class->GetSuperStruct())
                {
                    const auto& candidate_class_name = candidate_class->GetNamePrivate();
                    if (candidate_class_name.Equals(player_controller_name))
                    {
                        is_player_controller = true;
                        break;
                    }
                }
                if (!is_player_controller)
                {
                    continue;
                }

                const auto full_name = to_string(candidate->GetFullName());
                if (full_name.contains("Default__") ||
                    full_name.contains("SKEL_") ||
                    full_name.contains("REINST_"))
                {
                    continue;
                }

                Output::send(STR("[MCP] Selected console context: {}\n"), candidate->GetFullName());
                return candidate;
            }
            Output::send<LogLevel::Warning>(STR("[MCP] No live non-CDO PlayerController found in {} objects.\n"),
                                            object_count);
            return nullptr;
        }

        auto find_lua_mod(std::string_view mod_name) -> LuaMod*
        {
            if (!mod_name.empty())
            {
                return UE4SSProgram::find_lua_mod_by_name(mod_name, UE4SSProgram::IsInstalled::Yes, UE4SSProgram::IsStarted::Yes);
            }

            for (const auto& mod : UE4SSProgram::get_program().m_mods)
            {
                auto* lua_mod = dynamic_cast<LuaMod*>(mod.get());
                if (lua_mod && lua_mod->is_installed() && lua_mod->is_started())
                {
                    return lua_mod;
                }
            }

            return nullptr;
        }

        auto lua_value_to_json(lua_State* L, int index) -> std::string
        {
            const int type = lua_type(L, index);
            switch (type)
            {
            case LUA_TNIL:
                return "null";
            case LUA_TBOOLEAN:
                return bool_json(lua_toboolean(L, index) != 0);
            case LUA_TNUMBER:
                if (lua_isinteger(L, index))
                {
                    return std::format("{}", lua_tointeger(L, index));
                }
                return std::format("{}", lua_tonumber(L, index));
            case LUA_TSTRING:
                return json_string(lua_tostring(L, index));
            default:
                luaL_tolstring(L, index, nullptr);
                std::string as_string = lua_tostring(L, -1);
                lua_pop(L, 1);
                return std::format("{{\"type\":{},\"value\":{}}}", json_string(lua_typename(L, type)), json_string(as_string));
            }
        }
    } // namespace

    Server::~Server()
    {
        stop();
    }

    auto Server::Get() -> Server&
    {
        static Server server{};
        return server;
    }

    auto Server::start(const SettingsManager::SectionMCP& settings) -> void
    {
        if (!settings.Enabled || m_running.load(std::memory_order_acquire))
        {
            return;
        }

        m_config.enabled = settings.Enabled;
        m_config.pipe_name = settings.PipeName.empty() ? std::format("UE4SS-MCP-{}", GetCurrentProcessId()) : to_string(settings.PipeName);
        m_config.auth_token = to_string(settings.AuthToken);
        m_config.allow_lua_eval = settings.AllowLuaEval;
        m_config.max_result_count = static_cast<size_t>(std::max<int64_t>(1, settings.MaxResultCount));
        m_config.max_serialize_depth = static_cast<size_t>(std::max<int64_t>(1, settings.MaxSerializeDepth));
        m_config.audit_log_enabled = settings.AuditLogEnabled;

        if (m_config.auth_token.empty())
        {
            Output::send<LogLevel::Error>(STR("[MCP] Refusing to start because MCP.AuthToken is empty.\n"));
            return;
        }

        if (!install_game_thread_pump())
        {
            Output::send<LogLevel::Error>(STR("[MCP] Refusing to start because the EngineTick game-thread pump could not be installed.\n"));
            return;
        }

        m_pipe_path = std::format(R"(\\.\pipe\{})", m_config.pipe_name);
        m_running.store(true, std::memory_order_release);
        m_thread = std::jthread{[this](std::stop_token token) { server_loop(token); }};

        Output::send(STR("[MCP] Bridge started on pipe '{}'.\n"), ensure_str(m_pipe_path));
    }

    auto Server::stop() -> void
    {
        if (!m_running.exchange(false, std::memory_order_acq_rel))
        {
            return;
        }

        uninstall_game_thread_pump();

        if (m_thread.joinable())
        {
            m_thread.request_stop();
            if (!m_pipe_path.empty())
            {
                const auto pipe = CreateFileA(m_pipe_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                if (pipe != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(pipe);
                }
            }
            m_thread.join();
        }

        Output::send(STR("[MCP] Bridge stopped.\n"));
    }

    auto Server::is_running() const -> bool
    {
        return m_running.load(std::memory_order_acquire);
    }

    auto Server::pipe_name() const -> std::string
    {
        return m_config.pipe_name;
    }

    auto Server::server_loop(std::stop_token token) -> void
    {
        while (!token.stop_requested() && m_running.load(std::memory_order_acquire))
        {
            const auto pipe = CreateNamedPipeA(m_pipe_path.c_str(),
                                              PIPE_ACCESS_DUPLEX,
                                              PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                              1,
                                              BUFFER_SIZE,
                                              BUFFER_SIZE,
                                              0,
                                              nullptr);

            if (pipe == INVALID_HANDLE_VALUE)
            {
                audit("server", "error", "CreateNamedPipe failed");
                std::this_thread::sleep_for(std::chrono::seconds{1});
                continue;
            }

            const BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
            if (connected && m_running.load(std::memory_order_acquire))
            {
                serve_client(pipe);
            }

            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
        }
    }

    auto Server::serve_client(void* pipe_handle) -> void
    {
        const auto pipe = static_cast<HANDLE>(pipe_handle);
        std::string pending{};
        std::array<char, BUFFER_SIZE> buffer{};

        for (;;)
        {
            DWORD bytes_read{};
            const BOOL ok = ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, nullptr);
            if (!ok || bytes_read == 0 || !m_running.load(std::memory_order_acquire))
            {
                break;
            }

            pending.append(buffer.data(), bytes_read);

            for (;;)
            {
                const auto newline = pending.find('\n');
                if (newline == std::string::npos)
                {
                    break;
                }

                auto line = pending.substr(0, newline);
                pending.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                if (line.empty())
                {
                    continue;
                }

                auto response = process_line(line);
                response.push_back('\n');

                DWORD bytes_written{};
                if (!WriteFile(pipe, response.data(), static_cast<DWORD>(response.size()), &bytes_written, nullptr))
                {
                    return;
                }
            }
        }
    }

    auto Server::process_line(std::string_view line) -> std::string
    {
        std::string id_json{"null"};

        try
        {
            const auto root = parse_object(line);

            if (const auto id_it = root.find("id"); id_it != root.end())
            {
                id_json = write_generic_json(id_it->second);
            }

            const auto token = get_string(root, "token");
            if (token != m_config.auth_token)
            {
                audit("auth", "rejected");
                return make_error(id_json, "unauthorized", "Invalid MCP bridge token.");
            }

            const auto method = get_string(root, "method");
            if (method.empty())
            {
                return make_error(id_json, "invalid_request", "Missing method.");
            }

            std::string params_json{"{}"};
            if (const auto params_it = root.find("params"); params_it != root.end())
            {
                params_json = write_generic_json(params_it->second);
            }

            const auto result_json = dispatch_on_game_thread(method, params_json);
            audit(method, "ok");
            return make_response(id_json, true, "result", result_json);
        }
        catch (const std::exception& e)
        {
            audit("request", "error", e.what());
            return make_error(id_json, "bridge_error", e.what());
        }
    }

    auto Server::dispatch_on_game_thread(std::string method, std::string params_json) -> std::string
    {
        struct State
        {
            std::mutex mutex{};
            std::condition_variable cv{};
            bool done{false};
            std::string result{};
            std::exception_ptr exception{};
        };

        auto state = std::make_shared<State>();

        {
            std::lock_guard guard{m_game_thread_task_mutex};
            m_game_thread_tasks.emplace_back([this, method = std::move(method), params_json = std::move(params_json), state]() mutable {
                try
                {
                    state->result = handle_request(method, params_json);
                }
                catch (...)
                {
                    state->exception = std::current_exception();
                }

                {
                    std::lock_guard state_guard{state->mutex};
                    state->done = true;
                }
                state->cv.notify_one();
            });
        }

        std::unique_lock lock{state->mutex};
        if (!state->cv.wait_for(lock, REQUEST_TIMEOUT, [&] { return state->done; }))
        {
            throw std::runtime_error{"request_timeout"};
        }

        if (state->exception)
        {
            std::rethrow_exception(state->exception);
        }

        return state->result;
    }

    auto Server::install_game_thread_pump() -> bool
    {
        if (m_engine_tick_callback_id != Unreal::Hook::ERROR_ID)
        {
            return true;
        }

        m_engine_tick_callback_id = Unreal::Hook::RegisterEngineTickPostCallback(
                [this](auto&, auto&&...) {
                    drain_game_thread_tasks();
                },
                {false, true, STR("UE4SS"), STR("MCPGameThreadPump")});

        return m_engine_tick_callback_id != Unreal::Hook::ERROR_ID;
    }

    auto Server::uninstall_game_thread_pump() -> void
    {
        if (m_engine_tick_callback_id == Unreal::Hook::ERROR_ID)
        {
            return;
        }

        Unreal::Hook::UnregisterCallback(m_engine_tick_callback_id);
        m_engine_tick_callback_id = Unreal::Hook::ERROR_ID;
    }

    auto Server::drain_game_thread_tasks() -> void
    {
        std::vector<std::function<void()>> tasks{};
        {
            std::lock_guard guard{m_game_thread_task_mutex};
            tasks.swap(m_game_thread_tasks);
        }

        for (auto& task : tasks)
        {
            try
            {
                task();
            }
            catch (const std::exception& e)
            {
                audit("game_thread_task", "error", e.what());
            }
        }
    }

    auto Server::make_response(std::string_view id, bool ok, std::string_view payload_key, std::string_view payload_json) -> std::string
    {
        return std::format("{{\"id\":{},\"ok\":{},\"{}\":{}}}", id, bool_json(ok), payload_key, payload_json);
    }

    auto Server::make_error(std::string_view id, std::string_view code, std::string_view message) -> std::string
    {
        return std::format("{{\"id\":{},\"ok\":false,\"error\":{{\"code\":{},\"message\":{}}}}}", id, json_string(code), json_string(message));
    }

    auto Server::audit(std::string_view method, std::string_view status, std::string_view details) -> void
    {
        if (!m_config.audit_log_enabled)
        {
            return;
        }

        const auto line = details.empty() ? std::format("{} {}", method, status) : std::format("{} {} {}", method, status, details);
        {
            std::lock_guard guard{m_audit_mutex};
            m_audit_entries.emplace_back(line);
            while (m_audit_entries.size() > 200)
            {
                m_audit_entries.pop_front();
            }
        }

        Output::send(STR("[MCP] {}\n"), ensure_str(line));
    }

    auto Server::recent_logs_json() -> std::string
    {
        std::lock_guard guard{m_audit_mutex};
        std::string out{"{\"entries\":["};
        for (size_t i = 0; i < m_audit_entries.size(); ++i)
        {
            if (i > 0)
            {
                out += ',';
            }
            out += json_string(m_audit_entries[i]);
        }
        out += "]}";
        return out;
    }

    auto Server::handle_request(std::string_view method, std::string_view params_json) -> std::string
    {
        if (method == "ping") return handle_ping();
        if (method == "session.info") return handle_session_info();
        if (method == "objects.search") return handle_search_objects(params_json);
        if (method == "object.inspect") return handle_inspect_object(params_json);
        if (method == "property.get") return handle_get_property(params_json);
        if (method == "property.set") return handle_set_property(params_json);
        if (method == "delegate.invoke") return handle_invoke_delegate(params_json);
        if (method == "asset.load") return handle_load_asset(params_json);
        if (method == "console.exec") return handle_exec_console(params_json);
        if (method == "function.call") return handle_call_function(params_json);
        if (method == "function.inspect") return handle_inspect_function(params_json);
        if (method == "mod.reload") return handle_reload_mod(params_json);
        if (method == "function.watch") return handle_watch_function(params_json);
        if (method == "watch.remove") return handle_unwatch(params_json);
        if (method == "lua.run") return handle_run_lua(params_json);
        if (method == "events.recent") return handle_events_recent();
        if (method == "logs.recent") return recent_logs_json();

        throw std::runtime_error{std::format("unknown_method: {}", method)};
    }

    auto Server::handle_ping() -> std::string
    {
        return std::format("{{\"pong\":true,\"pipeName\":{},\"allowLuaEval\":{}}}",
                           json_string(m_config.pipe_name),
                           bool_json(m_config.allow_lua_eval));
    }

    auto Server::handle_session_info() -> std::string
    {
        return std::format(
                "{{\"ue4ssVersion\":{},\"gitSha\":{},\"pid\":{},\"pipeName\":{},\"workingDirectory\":{},\"allowLuaEval\":{},\"maxResultCount\":{},\"maxSerializeDepth\":{}}}",
                json_string(std::format("{}.{}.{}.{}.{}",
                                        UE4SS_LIB_VERSION_MAJOR,
                                        UE4SS_LIB_VERSION_MINOR,
                                        UE4SS_LIB_VERSION_HOTFIX,
                                        UE4SS_LIB_VERSION_PRERELEASE,
                                        UE4SS_LIB_VERSION_BETA)),
                json_string(UE4SS_LIB_BUILD_GITSHA),
                GetCurrentProcessId(),
                json_string(m_config.pipe_name),
                json_string(to_string(UE4SSProgram::get_program().get_working_directory())),
                bool_json(m_config.allow_lua_eval),
                m_config.max_result_count,
                m_config.max_serialize_depth);
    }

    auto Server::handle_search_objects(std::string_view params_json) -> std::string
    {
        const auto params = parse_object(params_json);
        const auto query = to_lower_ascii(get_string(params, "query"));
        const auto class_name = to_lower_ascii(get_string(params, "className"));
        const auto limit = std::min(get_size(params, "limit", m_config.max_result_count), m_config.max_result_count);
        constexpr size_t scan_budget = 512;
        const auto total_objects = Unreal::UObjectArray::GetNumElements();
        const auto default_cursor = total_objects > 0 ? static_cast<size_t>(total_objects - 1) : 0;
        const auto requested_cursor = get_size(params, "cursor", default_cursor);
        int64_t object_index = total_objects > 0
                                       ? static_cast<int64_t>(std::min(requested_cursor, default_cursor))
                                       : -1;

        std::string out{"{\"objects\":["};
        size_t count{};
        size_t scanned{};
        bool truncated{};

        for (; object_index >= 0 && scanned < scan_budget; --object_index)
        {
            ++scanned;
            auto* object_item = Unreal::FUObjectArray::IndexToObject(static_cast<int32_t>(object_index));
            auto* object = object_item ? object_item->GetUObject() : nullptr;
            if (!object_is_valid(object))
            {
                continue;
            }

            const auto object_class_name = object->GetClassPrivate() ? to_string(object->GetClassPrivate()->GetName()) : "";
            const auto object_class_lower = to_lower_ascii(object_class_name);
            if (!class_name.empty() && !object_class_lower.contains(class_name))
            {
                continue;
            }

            const auto full_name = to_string(object->GetFullName());
            const auto full_name_lower = to_lower_ascii(full_name);
            if (!query.empty() && !full_name_lower.contains(query))
            {
                continue;
            }
            if (count >= limit)
            {
                truncated = true;
                --object_index;
                break;
            }

            if (count > 0)
            {
                out += ',';
            }

            const auto handle = remember_object(full_name);
            out += std::format("{{\"handle\":{},\"fullName\":{},\"path\":{},\"className\":{},\"address\":{}}}",
                               json_string(handle),
                               json_string(full_name),
                               json_string(to_string(object->GetPathName())),
                               json_string(object_class_name),
                               json_string(std::format("{:016X}", std::bit_cast<uintptr_t>(object))));
            ++count;
        }

        if (object_index >= 0)
        {
            truncated = true;
        }
        out += std::format(
                "],\"count\":{},\"scanned\":{},\"truncated\":{},\"nextCursor\":{}}}",
                count,
                scanned,
                bool_json(truncated),
                object_index >= 0 ? std::to_string(object_index) : "null");
        return out;
    }

    auto Server::handle_inspect_object(std::string_view params_json) -> std::string
    {
        const auto params = parse_object(params_json);
        auto* object = resolve_object(get_string(params, "handle"));
        if (!object)
        {
            object = resolve_object(get_string(params, "fullName"));
        }
        if (!object_is_valid(object))
        {
            throw std::runtime_error{"object_invalid"};
        }

        const auto full_name = to_string(object->GetFullName());
        std::string out = std::format("{{\"handle\":{},\"fullName\":{},\"path\":{},\"className\":{},\"address\":{},\"properties\":[",
                                      json_string(remember_object(full_name)),
                                      json_string(full_name),
                                      json_string(to_string(object->GetPathName())),
                                      json_string(object->GetClassPrivate() ? to_string(object->GetClassPrivate()->GetName()) : ""),
                                      json_string(std::format("{:016X}", std::bit_cast<uintptr_t>(object))));

        const auto include_values = get_bool(params, "includeValues", true);
        const auto property_query = to_lower_ascii(get_string(params, "propertyQuery"));
        const auto property_limit = std::min(get_size(params, "limit", m_config.max_result_count), m_config.max_result_count);
        size_t count{};
        for (auto* property : Unreal::TFieldRange<Unreal::FProperty>(object->GetClassPrivate(), Unreal::EFieldIterationFlags::IncludeDeprecated))
        {
            if (!property || count >= property_limit)
            {
                break;
            }
            const auto property_name = to_string(property->GetName());
            const auto property_full_name = to_string(property->GetFullName());
            if (!property_query.empty() &&
                !to_lower_ascii(property_name).contains(property_query) &&
                !to_lower_ascii(property_full_name).contains(property_query))
            {
                continue;
            }
            if (count > 0)
            {
                out += ',';
            }
            const auto cpp_type = property->GetCPPType();
            out += std::format(
                    "{{\"name\":{},\"fullName\":{},\"cppType\":{},\"offset\":{},\"size\":{},\"propertyFlags\":{}",
                    json_string(property_name),
                    json_string(property_full_name),
                    json_string(to_string(*cpp_type)),
                    property->GetOffset_Internal(),
                    property->GetSize(),
                    static_cast<uint64_t>(property->GetPropertyFlags()));
            if (include_values)
            {
                out += std::format(",\"value\":{}", json_string(property_to_text(object, property)));
            }
            out += '}';
            ++count;
        }

        out += std::format("],\"propertyCount\":{},\"includeValues\":{}}}", count, bool_json(include_values));
        return out;
    }

    auto Server::handle_get_property(std::string_view params_json) -> std::string
    {
        const auto params = parse_object(params_json);
        auto* object = resolve_object(get_string(params, "handle"));
        if (!object_is_valid(object))
        {
            throw std::runtime_error{"object_invalid"};
        }

        auto* property = find_property(object, get_string(params, "propertyName"));
        if (!property)
        {
            throw std::runtime_error{"property_not_found"};
        }

        return std::format("{{\"name\":{},\"value\":{},\"fullName\":{}}}",
                           json_string(to_string(property->GetName())),
                           json_string(property_to_text(object, property)),
                           json_string(to_string(property->GetFullName())));
    }

    auto Server::handle_set_property(std::string_view params_json) -> std::string
    {
        const auto params = parse_object(params_json);
        auto* object = resolve_object(get_string(params, "handle"));
        if (!object_is_valid(object))
        {
            throw std::runtime_error{"object_invalid"};
        }

        auto* property = find_property(object, get_string(params, "propertyName"));
        if (!property)
        {
            throw std::runtime_error{"property_not_found"};
        }

        const auto value = ensure_str(get_string(params, "value"));
        Unreal::FOutputDevice output_device{};
        const auto imported = property->ImportText(FromCharTypePtr<TCHAR>(value.c_str()), property->ContainerPtrToValuePtr<void>(object), NULL, object, &output_device);
        if (!imported)
        {
            throw std::runtime_error{"property_import_failed"};
        }

        return std::format("{{\"set\":true,\"name\":{},\"value\":{}}}", json_string(to_string(property->GetName())), json_string(property_to_text(object, property)));
    }

    auto Server::handle_invoke_delegate(std::string_view params_json) -> std::string
    {
        const auto params = parse_object(params_json);
        auto* object = resolve_object(get_string(params, "handle"));
        if (!object_is_valid(object))
        {
            throw std::runtime_error{"object_invalid"};
        }

        const auto property_name = get_string(params, "propertyName");
        auto* property = find_property(object, property_name);
        if (!property)
        {
            throw std::runtime_error{"property_not_found"};
        }

        size_t invoked_count{};
        const auto invoke_one = [&](const Unreal::FScriptDelegate& delegate) {
            if (!delegate.IsBound())
            {
                return;
            }
            auto* target = delegate.GetUObject();
            if (!object_is_valid(target))
            {
                return;
            }
            auto* function = find_function_without_interfaces(target, delegate.GetFunctionName());
            if (!function)
            {
                return;
            }
            const size_t parameter_size = function->GetParmsSize();
            if (parameter_size > 1024 * 1024)
            {
                throw std::runtime_error{"invalid_parameter_size"};
            }
            std::vector<std::byte> parameter_data(std::max<size_t>(parameter_size, 1));
            target->ProcessEvent(function, parameter_data.data());
            ++invoked_count;
        };

        if (auto* delegate_property = Unreal::CastField<Unreal::FDelegateProperty>(property))
        {
            auto* delegate = delegate_property->ContainerPtrToValuePtr<Unreal::FScriptDelegate>(object);
            if (delegate)
            {
                invoke_one(*delegate);
            }
        }
        else if (auto* multicast_property = Unreal::CastField<Unreal::FMulticastDelegateProperty>(property))
        {
            auto* property_value = multicast_property->ContainerPtrToValuePtr<void>(object);
            const auto* delegate = multicast_property->GetMulticastDelegate(property_value);
            if (delegate)
            {
                for (const auto& invocation : delegate->InvocationList)
                {
                    invoke_one(invocation);
                }
            }
        }
        else
        {
            throw std::runtime_error{"property_not_delegate"};
        }

        return std::format(
                "{{\"invoked\":{},\"invokedCount\":{},\"context\":{},\"propertyName\":{}}}",
                bool_json(invoked_count > 0),
                invoked_count,
                json_string(to_string(object->GetFullName())),
                json_string(property_name));
    }

    auto Server::handle_load_asset(std::string_view params_json) -> std::string
    {
        const auto params = parse_object(params_json);
        const auto asset_path = get_string(params, "assetPath");
        if (asset_path.empty())
        {
            throw std::runtime_error{"missing_assetPath"};
        }

        auto* asset_registry = static_cast<Unreal::UAssetRegistry*>(
                Unreal::UAssetRegistryHelpers::GetAssetRegistry().ObjectPointer);
        if (!asset_registry)
        {
            throw std::runtime_error{"asset_registry_unavailable"};
        }

        auto asset_name = Unreal::FName(ensure_str(asset_path), Unreal::FNAME_Add);
        auto asset_data = asset_registry->GetAssetByObjectPath(asset_name);
        const bool was_found =
                (Unreal::Version::IsAtMost(5, 0) && asset_data.ObjectPath().GetComparisonIndex()) ||
                asset_data.PackageName().GetComparisonIndex();

        Unreal::UObject* loaded_asset{};
        if (was_found)
        {
            loaded_asset = Unreal::UAssetRegistryHelpers::GetAsset(asset_data);
        }

        if (!object_is_valid(loaded_asset))
        {
            return std::format("{{\"found\":{},\"loaded\":false,\"assetPath\":{}}}",
                               bool_json(was_found),
                               json_string(asset_path));
        }

        const auto full_name = to_string(loaded_asset->GetFullName());
        return std::format(
                "{{\"found\":true,\"loaded\":true,\"assetPath\":{},\"handle\":{},\"fullName\":{},\"path\":{}}}",
                json_string(asset_path),
                json_string(remember_object(full_name)),
                json_string(full_name),
                json_string(to_string(loaded_asset->GetPathName())));
    }

    auto Server::handle_exec_console(std::string_view params_json) -> std::string
    {
        const auto params = parse_object(params_json);
        auto* object = resolve_object(get_string(params, "handle"));
        if (!object)
        {
            object = find_player_controller();
        }
        if (!object_is_valid(object))
        {
            throw std::runtime_error{"console_context_not_found"};
        }

        const auto command = ensure_str(get_string(params, "command"));
        Unreal::FOutputDevice output_device{};
        auto succeeded = object->ProcessConsoleExec(FromCharTypePtr<TCHAR>(command.c_str()), output_device, object);
        bool used_console_command_wrapper{};
        if (!succeeded)
        {
            const Unreal::FName console_command_name{STR("ConsoleCommand"), Unreal::FNAME_Find};
            if (!console_command_name.IsNone())
            {
                if (auto* console_command_function =
                            find_function_without_interfaces(object, console_command_name))
                {
                    auto wrapped_command = ensure_str(std::string{"ConsoleCommand \""});
                    wrapped_command.append(command);
                    wrapped_command.append(STR("\""));
                    auto& function_flags = console_command_function->GetFunctionFlags();
                    const auto original_flags = function_flags;
                    function_flags |= Unreal::FUNC_Exec;
                    succeeded = object->ProcessConsoleExec(
                            FromCharTypePtr<TCHAR>(wrapped_command.c_str()),
                            output_device,
                            object);
                    function_flags = original_flags;
                    used_console_command_wrapper = succeeded;
                }
            }
        }
        return std::format("{{\"succeeded\":{},\"context\":{},\"command\":{},\"dispatch\":{}}}",
                           bool_json(succeeded),
                           json_string(to_string(object->GetFullName())),
                           json_string(to_string(command)),
                           json_string(used_console_command_wrapper ? "PlayerController.ConsoleCommand" : "ProcessConsoleExec"));
    }

    auto Server::handle_call_function(std::string_view params_json) -> std::string
    {
        const auto params = parse_object(params_json);
        auto* object = resolve_object(get_string(params, "handle"));
        if (!object)
        {
            object = find_player_controller();
        }
        if (!object_is_valid(object))
        {
            throw std::runtime_error{"object_invalid"};
        }

        const auto function_name = get_string(params, "functionName");
        if (function_name.empty())
        {
            throw std::runtime_error{"missing_functionName"};
        }

        const Unreal::FName resolved_function_name{ensure_str(function_name), Unreal::FNAME_Find};
        if (resolved_function_name.IsNone())
        {
            throw std::runtime_error{"function_name_not_found"};
        }
        const auto function_handle = get_string(params, "functionHandle");
        const auto function_path = get_string(params, "functionPath");
        Unreal::UFunction* function{};
        if (!function_handle.empty())
        {
            function = Unreal::Cast<Unreal::UFunction>(resolve_object(function_handle));
            if (function && !function->GetNamePrivate().Equals(resolved_function_name))
            {
                throw std::runtime_error{"function_handle_name_mismatch"};
            }
            if (function && find_function_without_interfaces(object, resolved_function_name) != function)
            {
                throw std::runtime_error{"function_handle_context_mismatch"};
            }
        }
        else
        {
            function = function_path.empty()
                    ? find_function_without_interfaces(object, resolved_function_name)
                    : find_function_by_path(function_path);
        }
        if (!function)
        {
            throw std::runtime_error{"function_not_found"};
        }

        const auto arguments = get_array_strings(params, "args");
        if (arguments.empty())
        {
            const size_t parameter_size = function->GetParmsSize();
            if (parameter_size > 1024 * 1024)
            {
                throw std::runtime_error{"invalid_parameter_size"};
            }

            std::vector<std::byte> parameter_data(std::max<size_t>(parameter_size, 1));
            const auto parameter_hex = get_string(params, "paramHex");
            if (!parameter_hex.empty())
            {
                if ((parameter_hex.size() % 2) != 0 || parameter_hex.size() / 2 > parameter_size)
                {
                    throw std::runtime_error{"invalid_param_hex_size"};
                }
                const auto hex_value = [](char c) -> uint8_t {
                    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
                    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
                    if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
                    throw std::runtime_error{"invalid_param_hex_character"};
                };
                for (size_t index = 0; index < parameter_hex.size(); index += 2)
                {
                    parameter_data[index / 2] = static_cast<std::byte>(
                            (hex_value(parameter_hex[index]) << 4) |
                            hex_value(parameter_hex[index + 1]));
                }
            }
            const auto object_argument_handle = get_string(params, "objectArgHandle");
            const auto object_argument_offset = get_size(params, "objectArgOffset", 0);
            Unreal::UObject* object_argument{};
            if (!object_argument_handle.empty())
            {
                object_argument = resolve_object(object_argument_handle);
                if (!object_is_valid(object_argument))
                {
                    throw std::runtime_error{"object_argument_invalid"};
                }
                if (object_argument_offset > parameter_size ||
                    parameter_size - object_argument_offset < sizeof(object_argument))
                {
                    throw std::runtime_error{"object_argument_out_of_bounds"};
                }
                std::memcpy(
                        parameter_data.data() + object_argument_offset,
                        &object_argument,
                        sizeof(object_argument));
            }
            Output::send(STR("[MCP] Direct ProcessEvent call: {} on {} (0x{:X} parameter bytes).\n"),
                         ensure_str(function_name),
                         object->GetFullName(),
                         parameter_size);
            object->ProcessEvent(function, parameter_data.data());
            constexpr char HEX_DIGITS[]{"0123456789ABCDEF"};
            std::string result_hex{};
            result_hex.reserve(parameter_size * 2);
            for (size_t index = 0; index < parameter_size; ++index)
            {
                const auto value = std::to_integer<uint8_t>(parameter_data[index]);
                result_hex.push_back(HEX_DIGITS[value >> 4]);
                result_hex.push_back(HEX_DIGITS[value & 0x0F]);
            }
            return std::format(
                    "{{\"succeeded\":true,\"dispatch\":\"ProcessEvent\",\"context\":{},\"functionName\":{},"
                    "\"parameterSize\":{},\"paramHex\":{},\"resultHex\":{},\"objectArg\":{},\"objectArgOffset\":{}}}",
                    json_string(to_string(object->GetFullName())),
                    json_string(function_name),
                    parameter_size,
                    parameter_hex.empty() ? "null" : json_string(parameter_hex),
                    json_string(result_hex),
                    object_argument ? json_string(to_string(object_argument->GetFullName())) : "null",
                    object_argument_offset);
        }

        auto command = ensure_str(function_name);
        for (const auto& arg : arguments)
        {
            command.append(STR(" "));
            command.append(ensure_str(arg));
        }

        auto& function_flags = function->GetFunctionFlags();
        const auto original_flags = function_flags;
        function_flags |= Unreal::FUNC_Exec;

        Unreal::FOutputDevice output_device{};
        const auto succeeded = object->ProcessConsoleExec(FromCharTypePtr<TCHAR>(command.c_str()), output_device, object);
        function_flags = original_flags;

        return std::format("{{\"succeeded\":{},\"context\":{},\"functionName\":{},\"command\":{}}}",
                           bool_json(succeeded),
                           json_string(to_string(object->GetFullName())),
                           json_string(function_name),
                           json_string(to_string(command)));
    }

    auto Server::handle_inspect_function(std::string_view params_json) -> std::string
    {
        const auto params = parse_object(params_json);
        const auto function_name = get_string(params, "functionName");
        if (function_name.empty())
        {
            throw std::runtime_error{"missing_functionName"};
        }

        const Unreal::FName resolved_function_name{ensure_str(function_name), Unreal::FNAME_Find};
        if (resolved_function_name.IsNone())
        {
            throw std::runtime_error{"function_name_not_found"};
        }

        auto* object = resolve_object(get_string(params, "handle"));
        const auto function_handle = get_string(params, "functionHandle");
        auto* function = Unreal::Cast<Unreal::UFunction>(resolve_object(function_handle));
        if (!function_handle.empty() && !function)
        {
            throw std::runtime_error{"function_handle_invalid"};
        }
        if (function && !function->GetNamePrivate().Equals(resolved_function_name))
        {
            throw std::runtime_error{"function_handle_name_mismatch"};
        }
        if (!function)
        {
            if (!object)
            {
                object = find_player_controller();
            }
            if (!object_is_valid(object))
            {
                throw std::runtime_error{"object_invalid"};
            }
            function = find_function_without_interfaces(object, resolved_function_name);
            if (!function)
            {
                throw std::runtime_error{"function_not_found"};
            }
        }

        std::string out{"{\"functions\":["};
        out += std::format("{{\"fullName\":{},\"path\":{},\"parameterSize\":{},\"functionFlags\":{},\"parameters\":[",
                           json_string(to_string(function->GetFullName())),
                           json_string(to_string(function->GetPathName())),
                           function->GetParmsSize(),
                           static_cast<uint64_t>(function->GetFunctionFlags()));

        size_t parameter_count{};
        for (auto* property : Unreal::TFieldRange<Unreal::FProperty>(
                     function,
                     Unreal::EFieldIterationFlags::IncludeDeprecated))
        {
            if (!property || !property->HasAnyPropertyFlags(Unreal::CPF_Parm))
            {
                continue;
            }
            if (parameter_count > 0)
            {
                out += ',';
            }
            const auto cpp_type = property->GetCPPType();
            out += std::format(
                    "{{\"name\":{},\"fullName\":{},\"cppType\":{},\"offset\":{},\"size\":{},\"propertyFlags\":{}}}",
                    json_string(to_string(property->GetName())),
                    json_string(to_string(property->GetFullName())),
                    json_string(to_string(*cpp_type)),
                    property->GetOffset_Internal(),
                    property->GetSize(),
                    static_cast<uint64_t>(property->GetPropertyFlags()));
            ++parameter_count;
        }
        out += std::format("],\"parameterCount\":{}}}],\"count\":1,\"context\":{},\"resolution\":{}}}",
                           parameter_count,
                           object ? json_string(to_string(object->GetFullName())) : "null",
                           json_string(object ? "contextObject" : "functionHandle"));
        return out;
    }

    auto Server::handle_reload_mod(std::string_view params_json) -> std::string
    {
        const auto params = parse_object(params_json);
        const auto mod_name = get_string(params, "modName");
        if (mod_name.empty())
        {
            throw std::runtime_error{"missing_modName"};
        }

        auto* mod = UE4SSProgram::find_lua_mod_by_name(mod_name);
        if (!mod)
        {
            throw std::runtime_error{"mod_not_found"};
        }

        const auto was_started = mod->is_started();
        UE4SSProgram::get_program().queue_reinstall_mod_by_name(mod_name);
        return std::format("{{\"queued\":true,\"modName\":{},\"wasStarted\":{}}}", json_string(mod_name), bool_json(was_started));
    }

    auto Server::handle_run_lua(std::string_view params_json) -> std::string
    {
        if (!m_config.allow_lua_eval)
        {
            throw std::runtime_error{"lua_eval_disabled"};
        }

        const auto params = parse_object(params_json);
        const auto script = get_string(params, "script");
        if (script.empty())
        {
            throw std::runtime_error{"missing_script"};
        }

        auto* mod = find_lua_mod(get_string(params, "modName"));
        if (!mod)
        {
            throw std::runtime_error{"lua_mod_not_found"};
        }

        const auto game_thread = get_bool(params, "gameThread", false);
        std::string script_to_run = script;
        if (game_thread)
        {
            script_to_run = std::format(
                    "ExecuteInGameThread(function()\n"
                    "  local ok, err = pcall(function()\n{}\n  end)\n"
                    "  if not ok then print('[MCP LuaEval] ' .. tostring(err)) end\n"
                    "end)",
                    script);
        }

        std::lock_guard guard{LuaMod::m_thread_actions_mutex};
        lua_State* L = mod->lua().get_lua_state();
        const int base = lua_gettop(L);

        if (int status = luaL_loadstring(L, script_to_run.c_str()); status != LUA_OK)
        {
            const auto error = lua_tostring(L, -1);
            lua_settop(L, base);
            throw std::runtime_error{std::format("lua_load_failed: {}", error ? error : "unknown")};
        }

        if (int status = lua_pcall(L, 0, LUA_MULTRET, 0); status != LUA_OK)
        {
            const auto error = lua_tostring(L, -1);
            lua_settop(L, base);
            throw std::runtime_error{std::format("lua_pcall_failed: {}", error ? error : "unknown")};
        }

        const int top = lua_gettop(L);
        std::string values{"["};
        for (int i = base + 1; i <= top; ++i)
        {
            if (i > base + 1)
            {
                values += ',';
            }
            values += lua_value_to_json(L, i);
        }
        values += ']';
        lua_settop(L, base);

        return std::format("{{\"modName\":{},\"gameThread\":{},\"queued\":{},\"returns\":{}}}",
                           json_string(to_string(mod->get_name())),
                           bool_json(game_thread),
                           bool_json(game_thread),
                           values);
    }

    auto Server::handle_watch_function(std::string_view params_json) -> std::string
    {
        const auto params = parse_object(params_json);
        const auto function_name = get_string(params, "functionName");
        return std::format("{{\"requested\":{},\"active\":false,\"note\":{}}}",
                           json_string(function_name),
                           json_string("Function watch streaming is reserved for the next MCP bridge iteration."));
    }

    auto Server::handle_unwatch(std::string_view params_json) -> std::string
    {
        const auto params = parse_object(params_json);
        const auto watch_id = get_string(params, "watchId");
        return std::format("{{\"requested\":{},\"removed\":false,\"note\":{}}}",
                           watch_id.empty() ? "null" : json_string(watch_id),
                           json_string("Function watch streaming is reserved for the next MCP bridge iteration."));
    }

    auto Server::handle_events_recent() -> std::string
    {
        return "{\"events\":[],\"note\":\"Function watch streaming is reserved for the next MCP bridge iteration.\"}";
    }

    auto Server::remember_object(std::string full_name) -> std::string
    {
        std::lock_guard guard{m_handle_mutex};

        for (const auto& [handle, existing_full_name] : m_handle_to_full_name)
        {
            if (existing_full_name == full_name)
            {
                return handle;
            }
        }

        const auto handle = std::format("uobject-{}", m_next_handle_id++);
        m_handle_to_full_name.emplace(handle, std::move(full_name));
        return handle;
    }

    auto Server::resolve_object(std::string_view handle) -> RC::Unreal::UObject*
    {
        if (handle.empty())
        {
            return nullptr;
        }

        std::string full_name{};
        {
            std::lock_guard guard{m_handle_mutex};
            if (const auto it = m_handle_to_full_name.find(std::string{handle}); it != m_handle_to_full_name.end())
            {
                full_name = it->second;
            }
            else
            {
                full_name = std::string{handle};
            }
        }

        // Handles store GetFullName() ("ClassName /Path/Object"). Unreal's
        // exact lookup accepts the path portion and avoids a global scan.
        auto object_path = full_name;
        if (const auto separator = object_path.find(' '); separator != std::string::npos)
        {
            object_path.erase(0, separator + 1);
        }
        auto* object = Unreal::UObjectGlobals::StaticFindObject_InternalSlow(
                nullptr,
                nullptr,
                ensure_str(object_path).c_str());
        if (object_is_valid(object) &&
            to_string(object->GetPathName()) == object_path)
        {
            return object;
        }

        // Deep actor-component paths are not accepted by every engine build's
        // slow object parser. Narrow candidates by exact short name, then
        // compare their full paths.
        auto short_name = object_path;
        if (const auto separator = short_name.find_last_of(".:"); separator != std::string::npos)
        {
            short_name.erase(0, separator + 1);
        }
        const Unreal::FName resolved_short_name{ensure_str(short_name), Unreal::FNAME_Find};
        if (resolved_short_name.IsNone())
        {
            return nullptr;
        }

        const auto object_count = static_cast<int64_t>(Unreal::UObjectArray::GetNumElements());
        for (int64_t index = object_count - 1; index >= 0; --index)
        {
            auto* item = Unreal::FUObjectArray::IndexToObject(static_cast<int32_t>(index));
            auto* candidate = item ? item->GetUObject() : nullptr;
            if (!object_is_valid(candidate) ||
                !candidate->GetNamePrivate().Equals(resolved_short_name))
            {
                continue;
            }
            if (to_string(candidate->GetPathName()) == object_path)
            {
                return candidate;
            }
        }
        return nullptr;
    }
} // namespace RC::MCP

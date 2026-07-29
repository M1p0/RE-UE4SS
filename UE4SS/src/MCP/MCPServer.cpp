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
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
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

        auto find_player_controller() -> Unreal::UObject*
        {
            std::vector<Unreal::UObject*> player_controllers{};
            Unreal::UObjectGlobals::FindAllOf(STR("PlayerController"), player_controllers);
            if (player_controllers.empty())
            {
                return nullptr;
            }
            return player_controllers.back();
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
        if (method == "console.exec") return handle_exec_console(params_json);
        if (method == "function.call") return handle_call_function(params_json);
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

        std::string out{"{\"objects\":["};
        size_t count{};
        bool truncated{};

        Unreal::UObjectGlobals::ForEachUObject([&](Unreal::UObject* object, ...) -> LoopAction {
            if (!object_is_valid(object))
            {
                return LoopAction::Continue;
            }

            const auto full_name = to_string(object->GetFullName());
            const auto full_name_lower = to_lower_ascii(full_name);
            const auto object_class_name = object->GetClassPrivate() ? to_string(object->GetClassPrivate()->GetName()) : "";
            const auto object_class_lower = to_lower_ascii(object_class_name);

            if (!query.empty() && !full_name_lower.contains(query))
            {
                return LoopAction::Continue;
            }
            if (!class_name.empty() && !object_class_lower.contains(class_name))
            {
                return LoopAction::Continue;
            }
            if (count >= limit)
            {
                truncated = true;
                return LoopAction::Break;
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

            return LoopAction::Continue;
        });

        out += std::format("],\"count\":{},\"truncated\":{}}}", count, bool_json(truncated));
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

        size_t count{};
        for (auto* property : Unreal::TFieldRange<Unreal::FProperty>(object->GetClassPrivate(), Unreal::EFieldIterationFlags::IncludeDeprecated))
        {
            if (!property || count >= m_config.max_result_count)
            {
                break;
            }
            if (count > 0)
            {
                out += ',';
            }
            out += std::format("{{\"name\":{},\"fullName\":{},\"value\":{}}}",
                               json_string(to_string(property->GetName())),
                               json_string(to_string(property->GetFullName())),
                               json_string(property_to_text(object, property)));
            ++count;
        }

        out += std::format("],\"propertyCount\":{}}}", count);
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
        const auto succeeded = object->ProcessConsoleExec(FromCharTypePtr<TCHAR>(command.c_str()), output_device, object);
        return std::format("{{\"succeeded\":{},\"context\":{},\"command\":{}}}",
                           bool_json(succeeded),
                           json_string(to_string(object->GetFullName())),
                           json_string(to_string(command)));
    }

    auto Server::handle_call_function(std::string_view params_json) -> std::string
    {
        const auto params = parse_object(params_json);
        auto* object = resolve_object(get_string(params, "handle"));
        if (!object_is_valid(object))
        {
            throw std::runtime_error{"object_invalid"};
        }

        const auto function_name = get_string(params, "functionName");
        if (function_name.empty())
        {
            throw std::runtime_error{"missing_functionName"};
        }

        Unreal::UFunction* function{};
        for (auto* candidate : Unreal::TFieldRange<Unreal::UFunction>(object->GetClassPrivate(), Unreal::EFieldIterationFlags::IncludeAll))
        {
            if (candidate && to_string(candidate->GetName()) == function_name)
            {
                function = candidate;
                break;
            }
        }
        if (!function)
        {
            throw std::runtime_error{"function_not_found"};
        }

        auto command = ensure_str(function_name);
        for (const auto& arg : get_array_strings(params, "args"))
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

        Unreal::UObject* found{};
        Unreal::UObjectGlobals::ForEachUObject([&](Unreal::UObject* object, ...) -> LoopAction {
            if (object_is_valid(object) && to_string(object->GetFullName()) == full_name)
            {
                found = object;
                return LoopAction::Break;
            }
            return LoopAction::Continue;
        });

        return found;
    }
} // namespace RC::MCP

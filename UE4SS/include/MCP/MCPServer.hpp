#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <Common.hpp>
#include <SettingsManager.hpp>

namespace RC::Unreal
{
    class UObject;
}

namespace RC::MCP
{
    class RC_UE4SS_API Server
    {
      public:
        static auto Get() -> Server&;

        auto start(const SettingsManager::SectionMCP& settings) -> void;
        auto stop() -> void;
        [[nodiscard]] auto is_running() const -> bool;
        [[nodiscard]] auto pipe_name() const -> std::string;

      private:
        Server() = default;
        ~Server();
        Server(const Server&) = delete;
        auto operator=(const Server&) -> Server& = delete;

        struct Config
        {
            bool enabled{false};
            std::string pipe_name{};
            bool allow_lua_eval{true};
            size_t max_result_count{200};
            size_t max_serialize_depth{3};
            bool audit_log_enabled{true};
        };

        auto server_loop(std::stop_token token) -> void;
        auto serve_client(void* pipe) -> void;
        auto process_line(std::string_view line) -> std::string;
        auto dispatch_on_game_thread(std::string method, std::string params_json) -> std::string;
        auto install_game_thread_pump() -> bool;
        auto uninstall_game_thread_pump() -> void;
        auto drain_game_thread_tasks() -> void;

        auto make_response(std::string_view id, bool ok, std::string_view payload_key, std::string_view payload_json) -> std::string;
        auto make_error(std::string_view id, std::string_view code, std::string_view message) -> std::string;
        auto audit(std::string_view method, std::string_view status, std::string_view details = {}) -> void;
        auto recent_logs_json() -> std::string;

        auto handle_request(std::string_view method, std::string_view params_json) -> std::string;
        auto handle_ping() -> std::string;
        auto handle_session_info() -> std::string;
        auto handle_search_objects(std::string_view params_json) -> std::string;
        auto handle_inspect_object(std::string_view params_json) -> std::string;
        auto handle_get_property(std::string_view params_json) -> std::string;
        auto handle_set_property(std::string_view params_json) -> std::string;
        auto handle_invoke_delegate(std::string_view params_json) -> std::string;
        auto handle_load_asset(std::string_view params_json) -> std::string;
        auto handle_exec_console(std::string_view params_json) -> std::string;
        auto handle_call_function(std::string_view params_json) -> std::string;
        auto handle_inspect_function(std::string_view params_json) -> std::string;
        auto handle_reload_mod(std::string_view params_json) -> std::string;
        auto handle_watch_function(std::string_view params_json) -> std::string;
        auto handle_unwatch(std::string_view params_json) -> std::string;
        auto handle_run_lua(std::string_view params_json) -> std::string;
        auto handle_events_recent() -> std::string;

        auto remember_object(std::string full_name) -> std::string;
        auto resolve_object(std::string_view handle) -> RC::Unreal::UObject*;

      private:
        Config m_config{};
        std::jthread m_thread{};
        std::atomic_bool m_running{false};
        std::string m_pipe_path{};

        std::mutex m_audit_mutex{};
        std::deque<std::string> m_audit_entries{};

        std::mutex m_handle_mutex{};
        uint64_t m_next_handle_id{1};
        std::unordered_map<std::string, std::string> m_handle_to_full_name{};

        std::mutex m_game_thread_task_mutex{};
        std::vector<std::function<void()>> m_game_thread_tasks{};
        uint64_t m_engine_tick_callback_id{};
    };
} // namespace RC::MCP

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <TlHelp32.h>
#include <winternl.h>
#include <Unknwn.h>
#include <UIAutomationClient.h>
#include <UIAutomationCoreApi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <ctime>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <oleauto.h>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;

namespace {

std::atomic_bool g_stop_requested{false};

std::string trim(std::string value) {
    const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string strip_bom(std::string value) {
    constexpr unsigned char utf8_bom[] = {0xEF, 0xBB, 0xBF};
    if (value.size() >= 3 && static_cast<unsigned char>(value[0]) == utf8_bom[0] &&
        static_cast<unsigned char>(value[1]) == utf8_bom[1] &&
        static_cast<unsigned char>(value[2]) == utf8_bom[2]) {
        value.erase(0, 3);
    }
    return value;
}

std::string to_lower_ascii(std::string_view input) {
    std::string result;
    result.reserve(input.size());
    for (unsigned char ch : input) {
        result.push_back(static_cast<char>(std::tolower(ch)));
    }
    return result;
}

void replace_all(std::string& text, const std::string& needle, const std::string& replacement) {
    if (needle.empty()) {
        return;
    }

    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) {
        text.replace(position, needle.size(), replacement);
        position += replacement.size();
    }
}

std::vector<std::string> split_patterns(const std::string& value) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto pos = value.find("||", start);
        const auto raw = pos == std::string::npos ? value.substr(start) : value.substr(start, pos - start);
        const auto trimmed = trim(raw);
        if (!trimmed.empty()) {
            parts.push_back(trimmed);
        }
        if (pos == std::string::npos) {
            break;
        }
        start = pos + 2;
    }
    return parts;
}

std::string join_patterns(const std::vector<std::string>& values, std::string_view separator = "||") {
    if (values.empty()) {
        return "(none)";
    }

    std::ostringstream builder;
    bool first = true;
    for (const auto& value : values) {
        if (!first) {
            builder << separator;
        }
        builder << value;
        first = false;
    }
    return builder.str();
}

std::string match_any_substring(const std::string& haystack_lower, const std::vector<std::string>& patterns) {
    for (const auto& raw_pattern : patterns) {
        const auto normalized = to_lower_ascii(raw_pattern);
        if (!normalized.empty() && haystack_lower.find(normalized) != std::string::npos) {
            return raw_pattern;
        }
    }
    return {};
}

bool process_name_matches(std::string_view exe_name, const std::vector<std::string>& patterns) {
    const auto normalized_exe_name = to_lower_ascii(exe_name);
    for (const auto& raw_pattern : patterns) {
        auto normalized_pattern = to_lower_ascii(trim(raw_pattern));
        if (normalized_pattern.empty()) {
            continue;
        }
        if (normalized_exe_name == normalized_pattern) {
            return true;
        }
        if (normalized_pattern.size() < 4 || normalized_pattern.substr(normalized_pattern.size() - 4) != ".exe") {
            normalized_pattern += ".exe";
            if (normalized_exe_name == normalized_pattern) {
                return true;
            }
        }
    }
    return false;
}

std::string normalize_line(std::string text) {
    if (!text.empty() && text.back() == '\r') {
        text.pop_back();
    }
    return text;
}

std::string decode_escaped_text(std::string text) {
    replace_all(text, "\\r\\n", "\n");
    replace_all(text, "\\n", "\n");
    replace_all(text, "\\t", "\t");
    return text;
}

std::wstring utf8_to_wide(const std::string& input) {
    if (input.empty()) {
        return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), nullptr, 0);
    if (size <= 0) {
        throw std::runtime_error("MultiByteToWideChar failed.");
    }

    std::wstring output(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), output.data(), size);
    return output;
}

std::string wide_to_utf8(const std::wstring& input) {
    if (input.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        throw std::runtime_error("WideCharToMultiByte failed.");
    }

    std::string output(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), output.data(), size, nullptr, nullptr);
    return output;
}

std::string bstr_to_utf8(BSTR value) {
    if (value == nullptr) {
        return {};
    }
    return wide_to_utf8(std::wstring(value, SysStringLen(value)));
}

std::string path_to_utf8(const fs::path& path) {
    return wide_to_utf8(path.wstring());
}

fs::path path_from_utf8(const std::string& text) {
    return fs::path(utf8_to_wide(text));
}

std::string now_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t current = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
    localtime_s(&local_tm, &current);

    char buffer[64];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local_tm);
    return buffer;
}

std::string compact_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t current = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
    localtime_s(&local_tm, &current);

    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &local_tm);
    return std::string(buffer) + "-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
}

std::string shorten_for_log(std::string_view input, std::size_t max_size = 160) {
    std::string result(input);
    replace_all(result, "\r", " ");
    replace_all(result, "\n", " ");
    if (result.size() > max_size) {
        result.resize(max_size - 3);
        result += "...";
    }
    return result;
}

std::string hex_uintptr(std::uintptr_t value) {
    std::ostringstream builder;
    builder << "0x" << std::hex << std::uppercase << value;
    return builder.str();
}

std::string sanitize_name(std::string value, std::string fallback = "default") {
    for (char& ch : value) {
        const bool ok = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        ch == '.' || ch == '_' || ch == '-';
        if (!ok) {
            ch = '-';
        }
    }

    value = trim(value);
    while (!value.empty() && value.front() == '-') {
        value.erase(value.begin());
    }
    while (!value.empty() && value.back() == '-') {
        value.pop_back();
    }

    return value.empty() ? fallback : value;
}

std::string strip_exe_suffix(std::string value) {
    const auto normalized = to_lower_ascii(value);
    if (normalized.size() > 4 && normalized.substr(normalized.size() - 4) == ".exe") {
        value.resize(value.size() - 4);
    }
    return value;
}

std::string sanitize_single_line(std::string value) {
    replace_all(value, "\r", " ");
    replace_all(value, "\n", " ");
    replace_all(value, "\t", " ");
    return trim(value);
}

std::wstring quote_windows_argument(const std::wstring& value) {
    if (value.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return value;
    }

    std::wstring quoted;
    quoted.push_back(L'"');
    std::size_t backslashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
            continue;
        }
        if (backslashes > 0) {
            quoted.append(backslashes, L'\\');
            backslashes = 0;
        }
        quoted.push_back(ch);
    }
    if (backslashes > 0) {
        quoted.append(backslashes * 2, L'\\');
    }
    quoted.push_back(L'"');
    return quoted;
}

bool looks_like_ini_path(const std::string& value) {
    return to_lower_ascii(fs::path(value).extension().string()) == ".ini";
}

std::string build_command_line(const std::vector<std::string>& arguments) {
    std::wstring command_line;
    bool first = true;
    for (const auto& argument : arguments) {
        if (!first) {
            command_line.push_back(L' ');
        }
        command_line += quote_windows_argument(utf8_to_wide(argument));
        first = false;
    }
    return wide_to_utf8(command_line);
}

std::optional<std::string> read_environment_variable(const char* name) {
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return std::nullopt;
    }

    std::string result(value);
    free(value);
    if (result.empty()) {
        return std::nullopt;
    }
    return result;
}

void append_command_argument(std::string& command_line, const std::string& argument) {
    if (!command_line.empty()) {
        command_line.push_back(' ');
    }
    command_line += wide_to_utf8(quote_windows_argument(utf8_to_wide(argument)));
}

std::string build_codex_wrapper_command_line(const std::vector<std::string>& passthrough_args) {
    std::string command_line = "codex";
    if (const auto override_command = read_environment_variable("LOOPCODE_CODEX_COMMAND")) {
        command_line = trim(*override_command);
    }
    if (command_line.empty()) {
        command_line = "codex";
    }
    for (const auto& argument : passthrough_args) {
        append_command_argument(command_line, argument);
    }
    return command_line;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (char ch : text) {
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            lines.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    return lines;
}

std::string tail_lines(const std::string& text, std::size_t max_lines) {
    const auto lines = split_lines(text);
    if (lines.size() <= max_lines) {
        return text;
    }

    std::ostringstream builder;
    const auto start = lines.size() - max_lines;
    for (std::size_t i = start; i < lines.size(); ++i) {
        if (i > start) {
            builder << '\n';
        }
        builder << lines[i];
    }
    return builder.str();
}

void close_handle(HANDLE& handle) {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        handle = nullptr;
    }
}

bool sleep_with_stop(int seconds) {
    for (int i = 0; i < seconds * 10; ++i) {
        if (g_stop_requested.load()) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return !g_stop_requested.load();
}

struct IniFile {
    std::unordered_map<std::string, std::string> values;

    static IniFile load(const fs::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("Unable to open config file: " + path.string());
        }

        IniFile ini;
        std::string current_section;
        std::string line;
        bool first_line = true;

        while (std::getline(input, line)) {
            if (first_line) {
                line = strip_bom(std::move(line));
                first_line = false;
            }

            auto cleaned = trim(line);
            if (cleaned.empty() || cleaned[0] == ';' || cleaned[0] == '#') {
                continue;
            }

            if (cleaned.front() == '[' && cleaned.back() == ']') {
                cleaned.erase(cleaned.begin());
                cleaned.pop_back();
                current_section = to_lower_ascii(trim(cleaned));
                continue;
            }

            const auto delimiter = cleaned.find('=');
            if (delimiter == std::string::npos) {
                continue;
            }

            auto key = to_lower_ascii(trim(cleaned.substr(0, delimiter)));
            auto value = trim(cleaned.substr(delimiter + 1));
            ini.values[current_section + "." + key] = value;
        }

        return ini;
    }

    std::string get(const std::string& section, const std::string& key, const std::string& fallback) const {
        const auto full_key = to_lower_ascii(section) + "." + to_lower_ascii(key);
        const auto it = values.find(full_key);
        return it == values.end() ? fallback : it->second;
    }
};

bool parse_bool(const std::string& value, bool fallback) {
    const auto normalized = to_lower_ascii(trim(value));
    if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") {
        return true;
    }
    if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") {
        return false;
    }
    return fallback;
}

int parse_int(const std::string& value, int fallback) {
    try {
        return value.empty() ? fallback : std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

struct Config {
    std::string mode = "spawn";
    std::string command_line;
    std::string workdir = ".";
    bool restart_on_exit = true;
    int max_restarts = -1;
    int restart_delay_seconds = 10;
    std::string attach_strategy = "auto";
    std::string attach_window_scope = "all";
    std::vector<std::string> target_process_names;
    std::vector<std::string> terminal_process_names;
    std::string window_title_contains;
    std::string window_class_contains;
    std::vector<std::string> attach_visible_text_patterns;
    int idle_seconds = 240;
    int attach_poll_millis = 1500;
    int action_cooldown_seconds = 45;
    int initial_resume_delay_seconds = 3;
    int transcript_keep_lines = 120;
    int decision_timeout_seconds = 20;
    bool echo_output = true;
    std::string log_dir = "logs";
    std::string continue_message = "continue";
    std::string resume_template =
        "Continue the interrupted task.\n"
        "Reason: {reason}\n"
        "Recent transcript:\n"
        "{transcript}\n";
    std::string new_session_message = "/new";
    std::string new_session_resume_template =
        "Treat this as a fresh conversation and continue the interrupted task.\n"
        "Reason: {reason}\n"
        "Recent transcript:\n"
        "{transcript}\n";
    std::vector<std::string> wait_patterns;
    std::vector<std::string> recoverable_error_patterns;
    int recoverable_error_new_session_threshold = 2;
    std::string decision_mode = "fixed";
    std::string external_command;
    bool session_enabled = true;
    std::string session_name;
    std::string session_storage_dir = "sessions";
};

Config load_config(const fs::path& path) {
    const auto ini = IniFile::load(path);

    Config config;
    config.mode = to_lower_ascii(ini.get("agent", "mode", "spawn"));
    config.command_line = ini.get("agent", "command_line", "");
    config.workdir = ini.get("agent", "workdir", ".");
    config.restart_on_exit = parse_bool(ini.get("agent", "restart_on_exit", "true"), true);
    config.max_restarts = parse_int(ini.get("agent", "max_restarts", "-1"), -1);
    config.restart_delay_seconds = parse_int(ini.get("agent", "restart_delay_seconds", "10"), 10);
    config.attach_strategy = to_lower_ascii(ini.get("agent", "attach_strategy", "auto"));
    config.attach_window_scope = to_lower_ascii(ini.get("agent", "attach_window_scope", "all"));
    config.target_process_names =
        split_patterns(ini.get("agent", "target_process_names", "codex.exe||claude.exe"));
    config.terminal_process_names =
        split_patterns(ini.get("agent", "terminal_process_names", "WindowsTerminal.exe||OpenConsole.exe||conhost.exe"));
    config.window_title_contains = ini.get("agent", "window_title_contains", "");
    config.window_class_contains = ini.get("agent", "window_class_contains", "");
    config.attach_visible_text_patterns = split_patterns(
        ini.get("agent",
                "attach_visible_text_contains",
                "codex||claude||openai||anthropic||continue||继续||approval||confirm||network||timeout||rate limit||"
                "service unavailable||thinking||working"));

    config.idle_seconds = parse_int(ini.get("watchdog", "idle_seconds", "240"), 240);
    config.attach_poll_millis = parse_int(ini.get("watchdog", "attach_poll_millis", "1500"), 1500);
    config.action_cooldown_seconds = parse_int(ini.get("watchdog", "action_cooldown_seconds", "45"), 45);
    config.initial_resume_delay_seconds = parse_int(ini.get("watchdog", "initial_resume_delay_seconds", "3"), 3);
    config.transcript_keep_lines = parse_int(ini.get("watchdog", "transcript_keep_lines", "120"), 120);
    config.decision_timeout_seconds = parse_int(ini.get("watchdog", "decision_timeout_seconds", "20"), 20);
    config.echo_output = parse_bool(ini.get("watchdog", "echo_output", "true"), true);
    config.log_dir = ini.get("watchdog", "log_dir", "logs");

    config.continue_message = decode_escaped_text(ini.get("actions", "continue_message", "continue"));
    config.resume_template = decode_escaped_text(ini.get(
        "actions",
        "resume_template",
        "Continue the interrupted task.\nReason: {reason}\nRecent transcript:\n{transcript}\n"));
    config.new_session_message = decode_escaped_text(ini.get("actions", "new_session_message", "/new"));
    config.new_session_resume_template = decode_escaped_text(ini.get(
        "actions",
        "new_session_resume_template",
        "Treat this as a fresh conversation and continue the interrupted task.\nReason: {reason}\nRecent transcript:\n{transcript}\n"));

    config.wait_patterns = split_patterns(
        ini.get("patterns", "wait", "continue||继续||press enter||confirm||waiting for your input||need your approval"));
    config.recoverable_error_patterns = split_patterns(
        ini.get("patterns", "recoverable_error",
                "timed out||timeout||network||connection reset||service unavailable||rate limit||temporary failure"));
    config.recoverable_error_new_session_threshold =
        parse_int(ini.get("recovery", "recoverable_error_new_session_threshold", "2"), 2);

    config.decision_mode = to_lower_ascii(ini.get("decision", "mode", "fixed"));
    config.external_command = ini.get("decision", "external_command", "");
    config.session_enabled = parse_bool(ini.get("session", "enabled", "true"), true);
    config.session_name = ini.get("session", "name", "");
    config.session_storage_dir = ini.get("session", "storage_dir", "sessions");

    if (config.mode != "spawn" && config.mode != "attach") {
        throw std::runtime_error("Config key [agent] mode must be either spawn or attach.");
    }

    if (config.mode == "spawn" && config.command_line.empty()) {
        throw std::runtime_error("Config key [agent] command_line is required.");
    }

    if (config.mode == "attach") {
        const bool has_title_filters = !config.window_title_contains.empty() || !config.window_class_contains.empty();
        const bool has_process_tree_filters = !config.target_process_names.empty() && !config.terminal_process_names.empty();

        if (config.attach_strategy != "auto" && config.attach_strategy != "process_tree" &&
            config.attach_strategy != "title_match") {
            throw std::runtime_error(
                "Config key [agent] attach_strategy must be auto, process_tree, or title_match.");
        }
        if (config.attach_window_scope != "single" && config.attach_window_scope != "all") {
            throw std::runtime_error("Config key [agent] attach_window_scope must be single or all.");
        }

        if (config.attach_strategy == "title_match" && !has_title_filters) {
            throw std::runtime_error(
                "Attach mode with attach_strategy=title_match requires [agent] window_title_contains or "
                "window_class_contains.");
        }

        if (config.attach_strategy == "process_tree" && !has_process_tree_filters) {
            throw std::runtime_error(
                "Attach mode with attach_strategy=process_tree requires [agent] target_process_names and "
                "terminal_process_names.");
        }

        if (config.attach_strategy == "auto" && !has_process_tree_filters && !has_title_filters) {
            throw std::runtime_error(
                "Attach mode requires either process-tree settings or title/class filters.");
        }
    }

    return config;
}

class Logger {
public:
    explicit Logger(const fs::path& file_path) {
        fs::create_directories(file_path.parent_path());
        file_.open(file_path, std::ios::app | std::ios::binary);
        if (!file_) {
            throw std::runtime_error("Unable to open log file: " + file_path.string());
        }
    }

    void info(const std::string& category, const std::string& message, bool to_stderr = false) {
        const auto line = "[" + now_timestamp() + "] [" + category + "] " + message;
        std::lock_guard<std::mutex> lock(mutex_);
        if (to_stderr) {
            std::cerr << line << std::endl;
        } else {
            std::cout << line << std::endl;
        }
        file_ << line << '\n';
        file_.flush();
    }

    void child_line(const std::string& source, const std::string& line, bool echo_to_console) {
        const auto rendered = "[" + now_timestamp() + "] [" + source + "] " + line;
        std::lock_guard<std::mutex> lock(mutex_);
        if (echo_to_console) {
            std::cout << rendered << std::endl;
        }
        file_ << rendered << '\n';
        file_.flush();
    }

private:
    std::mutex mutex_;
    std::ofstream file_;
};

enum class TriggerKind {
    Continue,
    NewSessionResume,
    RestartResume,
    Idle,
};

struct PendingAction {
    TriggerKind kind = TriggerKind::Continue;
    std::string reason;
    int priority = 0;
    std::string transcript_override;
};

struct AttachInventoryEntry {
    std::string agent_name;
    std::string workdir;
};

struct RuntimeOptions {
    fs::path config_path = "examples/loopguard.ini";
    bool config_explicit = false;
    bool interactive_menu = false;
    bool wrapper_mode = false;
    bool resume_last = false;
    bool resume_all_attached = false;
    std::optional<std::string> resume_session_name;
    std::optional<std::string> self_test_name;
    std::vector<std::string> codex_passthrough_args;
    std::vector<std::uintptr_t> selected_attach_hwnds;
    std::optional<std::vector<AttachInventoryEntry>> attached_resume_entries_override;
};

struct SavedSessionData {
    std::string storage_name;
    std::string display_name;
    fs::path config_path;
    std::string mode;
    std::string command_line;
    std::string workdir;
    std::string transcript;
    std::string saved_at;
};

class SharedState {
public:
    explicit SharedState(std::size_t keep_lines)
        : keep_lines_(keep_lines),
          last_activity_(Clock::now()),
          last_action_(Clock::now() - std::chrono::hours(24)) {}

    void reset_for_new_session() {
        std::lock_guard<std::mutex> lock(mutex_);
        last_activity_ = Clock::now();
        last_action_ = Clock::now() - std::chrono::hours(24);
        pending_.reset();
        suppressed_trigger_key_.clear();
        consecutive_recoverable_actions_ = 0;
    }

    void note_system_event(const std::string& line) {
        std::lock_guard<std::mutex> lock(mutex_);
        push_transcript_locked("[system] " + line);
    }

    void inspect_output_text(const std::string& text, const Config& config) {
        std::lock_guard<std::mutex> lock(mutex_);
        last_activity_ = Clock::now();

        const auto haystack = to_lower_ascii(text);
        refresh_suppression_locked(haystack);
        if (const auto matched = match_any_substring(haystack, config.recoverable_error_patterns); !matched.empty()) {
            const auto key = "recoverable:" + to_lower_ascii(matched);
            if (suppressed_trigger_key_ != key) {
                queue_locked({TriggerKind::Continue, "matched recoverable_error pattern: " + matched, 2});
            }
            return;
        }
        if (const auto matched = match_any_substring(haystack, config.wait_patterns); !matched.empty()) {
            const auto key = "wait:" + to_lower_ascii(matched);
            if (suppressed_trigger_key_ != key) {
                queue_locked({TriggerKind::Continue, "matched wait pattern: " + matched, 1});
            }
        }
    }

    void note_output_line(const std::string& source, const std::string& line, const Config& config) {
        std::lock_guard<std::mutex> lock(mutex_);
        (void)config;
        last_activity_ = Clock::now();
        push_transcript_locked("[" + source + "] " + line);
    }

    void schedule_restart_resume(const std::string& reason) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_locked({TriggerKind::RestartResume, reason, 3, {}});
    }

    void schedule_resume_with_transcript(const std::string& reason, const std::string& transcript) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_locked({TriggerKind::RestartResume, reason, 3, transcript});
    }

    std::optional<PendingAction> pop_due_action(const Config& config, bool allow_idle = true) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = Clock::now();

        if (pending_ && seconds_since_locked(last_action_, now) >= config.action_cooldown_seconds) {
            auto action = pending_;
            pending_.reset();
            last_action_ = now;
            if (action && action->kind == TriggerKind::Continue && is_recoverable_error_reason(action->reason)) {
                const int next_attempt = consecutive_recoverable_actions_ + 1;
                if (config.recoverable_error_new_session_threshold > 0 &&
                    next_attempt >= config.recoverable_error_new_session_threshold) {
                    action->kind = TriggerKind::NewSessionResume;
                }
            }
            return action;
        }

        if (allow_idle && config.idle_seconds > 0 &&
            seconds_since_locked(last_activity_, now) >= config.idle_seconds &&
            seconds_since_locked(last_action_, now) >= config.action_cooldown_seconds) {
            last_activity_ = now;
            last_action_ = now;
            return PendingAction{TriggerKind::Idle, "idle for " + std::to_string(config.idle_seconds) + " seconds", 1};
        }

        return std::nullopt;
    }

    std::string recent_transcript() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream builder;
        bool first = true;
        for (const auto& line : transcript_) {
            if (!first) {
                builder << '\n';
            }
            builder << line;
            first = false;
        }
        return builder.str();
    }

    void note_action_sent(const PendingAction& action) {
        std::lock_guard<std::mutex> lock(mutex_);
        suppressed_trigger_key_ = suppression_key_from_reason(action.reason);
        if (is_recoverable_error_reason(action.reason)) {
            ++consecutive_recoverable_actions_;
        }
    }

private:
    static long long seconds_since_locked(const Clock::time_point& start, const Clock::time_point& end) {
        return std::chrono::duration_cast<std::chrono::seconds>(end - start).count();
    }

    void push_transcript_locked(const std::string& line) {
        transcript_.push_back(line);
        while (transcript_.size() > keep_lines_) {
            transcript_.pop_front();
        }
    }

    void queue_locked(PendingAction next) {
        if (!pending_ || next.priority >= pending_->priority) {
            pending_ = std::move(next);
        }
    }

    static std::string suppression_key_from_reason(const std::string& reason) {
        constexpr std::string_view wait_prefix = "matched wait pattern: ";
        constexpr std::string_view recoverable_prefix = "matched recoverable_error pattern: ";

        if (reason.rfind(wait_prefix.data(), 0) == 0) {
            return "wait:" + to_lower_ascii(reason.substr(wait_prefix.size()));
        }
        if (reason.rfind(recoverable_prefix.data(), 0) == 0) {
            return "recoverable:" + to_lower_ascii(reason.substr(recoverable_prefix.size()));
        }
        return {};
    }

    static bool is_recoverable_error_reason(const std::string& reason) {
        constexpr std::string_view recoverable_prefix = "matched recoverable_error pattern: ";
        return reason.rfind(recoverable_prefix.data(), 0) == 0;
    }

    void refresh_suppression_locked(const std::string& haystack) {
        if (suppressed_trigger_key_.empty()) {
            return;
        }

        const auto delimiter = suppressed_trigger_key_.find(':');
        if (delimiter == std::string::npos || delimiter + 1 >= suppressed_trigger_key_.size()) {
            suppressed_trigger_key_.clear();
            return;
        }

        const auto pattern = suppressed_trigger_key_.substr(delimiter + 1);
        if (haystack.find(pattern) == std::string::npos) {
            suppressed_trigger_key_.clear();
        }
    }

    mutable std::mutex mutex_;
    std::deque<std::string> transcript_;
    std::size_t keep_lines_;
    Clock::time_point last_activity_;
    Clock::time_point last_action_;
    std::optional<PendingAction> pending_;
    std::string suppressed_trigger_key_;
    int consecutive_recoverable_actions_ = 0;
};

std::string effective_session_name(const Config& config) {
    if (!config.session_name.empty()) {
        return sanitize_name(config.session_name);
    }

    fs::path workdir_path = config.workdir.empty() ? fs::current_path() : fs::path(config.workdir);
    std::error_code error;
    const auto absolute = fs::absolute(workdir_path, error);
    const auto base_name = error ? workdir_path.filename().string() : absolute.filename().string();
    return sanitize_name(base_name.empty() ? "default" : base_name);
}

fs::path session_storage_root(const Config& config) {
    return path_from_utf8(config.session_storage_dir);
}

fs::path session_metadata_path(const Config& config, const std::string& storage_name) {
    return session_storage_root(config) / sanitize_name(storage_name) / "session.ini";
}

fs::path session_transcript_path(const Config& config, const std::string& storage_name) {
    return session_storage_root(config) / sanitize_name(storage_name) / "transcript.txt";
}

std::string effective_attach_inventory_name(const Config& config) {
    return sanitize_name(config.session_name.empty() ? "attached-windows" : config.session_name);
}

fs::path attach_inventory_path(const Config& config) {
    return session_storage_root(config) / "attach" / effective_attach_inventory_name(config) / "inventory.tsv";
}

bool can_persist_session(const Config& config) {
    return config.session_enabled && config.mode == "spawn" && !config.command_line.empty();
}

bool can_persist_attach_inventory(const Config& config) {
    return config.session_enabled && config.mode == "attach";
}

void write_text_file(const fs::path& path, const std::string& content);

std::vector<std::string> split_tab_fields(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (start <= line.size()) {
        const auto pos = line.find('\t', start);
        fields.push_back(pos == std::string::npos ? line.substr(start) : line.substr(start, pos - start));
        if (pos == std::string::npos) {
            break;
        }
        start = pos + 1;
    }
    return fields;
}

std::string attach_inventory_signature(const std::vector<AttachInventoryEntry>& entries) {
    std::ostringstream builder;
    for (const auto& entry : entries) {
        builder << sanitize_single_line(entry.agent_name) << '\t' << sanitize_single_line(entry.workdir) << '\n';
    }
    return builder.str();
}

std::string prompt_line(const std::string& prompt) {
    std::cout << prompt;
    std::string line;
    std::getline(std::cin, line);
    return trim(line);
}

std::vector<int> parse_selection_indexes(const std::string& input, int max_index) {
    std::vector<int> indexes;
    std::string token;
    auto flush_token = [&]() {
        token = trim(token);
        if (token.empty()) {
            return;
        }
        const int index = parse_int(token, -1);
        if (index >= 1 && index <= max_index &&
            std::find(indexes.begin(), indexes.end(), index) == indexes.end()) {
            indexes.push_back(index);
        }
        token.clear();
    };

    for (char ch : input) {
        if (ch == ',' || ch == ' ' || ch == ';') {
            flush_token();
            continue;
        }
        token.push_back(ch);
    }
    flush_token();
    return indexes;
}

void save_attach_inventory(const Config& config,
                           const std::vector<AttachInventoryEntry>& entries,
                           Logger& logger,
                           const std::string& reason) {
    if (!can_persist_attach_inventory(config)) {
        return;
    }

    const auto path = attach_inventory_path(config);
    std::ostringstream content;
    content << "saved_at\t" << now_timestamp() << "\n";
    content << "reason\t" << sanitize_single_line(reason) << "\n";
    for (const auto& entry : entries) {
        content << "entry\t" << sanitize_single_line(entry.agent_name) << '\t' << sanitize_single_line(entry.workdir)
                << "\n";
    }

    write_text_file(path, content.str());
    logger.info("session",
                "saved attach inventory \"" + effective_attach_inventory_name(config) + "\" with " +
                    std::to_string(entries.size()) + " entr" + (entries.size() == 1 ? "y" : "ies"));
}

std::vector<AttachInventoryEntry> load_attach_inventory(const Config& config) {
    const auto path = attach_inventory_path(config);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("No attached-window inventory found. Missing: " + path.string());
    }

    std::vector<AttachInventoryEntry> entries;
    std::string line;
    while (std::getline(input, line)) {
        line = trim(strip_bom(normalize_line(line)));
        if (line.empty()) {
            continue;
        }
        const auto fields = split_tab_fields(line);
        if (fields.size() < 3 || fields[0] != "entry") {
            continue;
        }

        AttachInventoryEntry entry;
        entry.agent_name = fields[1];
        entry.workdir = fields[2];
        if (!entry.agent_name.empty() && !entry.workdir.empty()) {
            entries.push_back(std::move(entry));
        }
    }

    if (entries.empty()) {
        throw std::runtime_error("Attached-window inventory is empty: " + path.string());
    }

    return entries;
}

void write_text_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Unable to write file: " + path.string());
    }
    output << content;
}

void save_session_snapshot(const Config& config,
                           const fs::path& config_path,
                           const SharedState& state,
                           Logger& logger,
                           const std::string& reason) {
    if (!can_persist_session(config)) {
        return;
    }

    const auto storage_name = effective_session_name(config);
    const auto metadata_path = session_metadata_path(config, storage_name);
    const auto transcript_path = session_transcript_path(config, storage_name);
    const auto resolved_workdir = fs::absolute(fs::path(config.workdir.empty() ? "." : config.workdir));
    const auto resolved_config_path = fs::absolute(config_path);
    const auto transcript = state.recent_transcript();

    std::ostringstream metadata;
    metadata << "[session]\n";
    metadata << "name = " << storage_name << "\n";
    metadata << "saved_at = " << now_timestamp() << "\n";
    metadata << "reason = " << reason << "\n";
    metadata << "config_path = " << path_to_utf8(resolved_config_path) << "\n";
    metadata << "\n[agent]\n";
    metadata << "mode = " << config.mode << "\n";
    metadata << "command_line = " << config.command_line << "\n";
    metadata << "workdir = " << path_to_utf8(resolved_workdir) << "\n";

    write_text_file(metadata_path, metadata.str());
    write_text_file(transcript_path, transcript);
    write_text_file(session_storage_root(config) / "last.txt", storage_name + "\n");

    logger.info("session",
                "saved session \"" + storage_name + "\" for workdir=" + path_to_utf8(resolved_workdir) + ". reason: " + reason);
}

SavedSessionData load_saved_session(const Config& config,
                                    const std::optional<std::string>& requested_name,
                                    bool resume_last) {
    std::string storage_name;
    if (resume_last) {
        const auto last_path = session_storage_root(config) / "last.txt";
        std::ifstream input(last_path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("No saved session found. Missing: " + last_path.string());
        }
        std::getline(input, storage_name);
        storage_name = trim(strip_bom(storage_name));
        if (storage_name.empty()) {
            throw std::runtime_error("No saved session found in: " + last_path.string());
        }
    } else if (requested_name) {
        storage_name = sanitize_name(*requested_name);
    } else {
        throw std::runtime_error("Internal error: no session name requested.");
    }

    const auto metadata_path = session_metadata_path(config, storage_name);
    const auto transcript_path = session_transcript_path(config, storage_name);
    const auto ini = IniFile::load(metadata_path);

    SavedSessionData data;
    data.storage_name = storage_name;
    data.display_name = ini.get("session", "name", storage_name);
    data.saved_at = ini.get("session", "saved_at", "");
    data.mode = ini.get("agent", "mode", "spawn");
    data.command_line = ini.get("agent", "command_line", "");
    data.workdir = ini.get("agent", "workdir", ".");

    const auto saved_config_path = ini.get("session", "config_path", "");
    if (!saved_config_path.empty()) {
        data.config_path = path_from_utf8(saved_config_path);
    }

    std::ifstream transcript_input(transcript_path, std::ios::binary);
    if (transcript_input) {
        std::ostringstream content;
        content << transcript_input.rdbuf();
        data.transcript = content.str();
    }

    if (data.mode != "spawn") {
        throw std::runtime_error("Saved session \"" + storage_name + "\" is not resumable in spawn mode.");
    }
    if (data.command_line.empty()) {
        throw std::runtime_error("Saved session \"" + storage_name + "\" is missing command_line.");
    }

    return data;
}

struct ChildProcess {
    HANDLE process = nullptr;
    HANDLE stdin_write = nullptr;
    DWORD pid = 0;
};

std::string build_auto_input(const Config& config,
                             const PendingAction& action,
                             const SharedState& state,
                             const fs::path& log_dir,
                             Logger& logger);

bool expect_self_test(bool condition, const std::string& message, std::string& error);

#include "loopguard_attach.inl"

bool send_text_to_child(HANDLE stdin_write, const std::string& text, Logger& logger) {
    if (stdin_write == nullptr) {
        logger.info("watchdog", "stdin pipe is not available; auto input skipped.", true);
        return false;
    }

    std::string payload = text;
    if (payload.empty()) {
        return false;
    }

    if (payload.back() != '\n') {
        payload += "\r\n";
    }

    DWORD written = 0;
    const BOOL ok = WriteFile(stdin_write, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr);
    if (!ok) {
        logger.info("watchdog", "failed to write auto input to child process.", true);
        return false;
    }

    FlushFileBuffers(stdin_write);
    logger.info("watchdog", "sent auto input: " + shorten_for_log(text));
    return true;
}

std::string render_template(std::string text, const std::string& reason, const std::string& transcript) {
    replace_all(text, "{reason}", reason);
    replace_all(text, "{transcript}", transcript.empty() ? "(no transcript captured)" : transcript);
    return text;
}

fs::path write_context_file(const fs::path& log_dir, const std::string& reason, const std::string& transcript) {
    fs::create_directories(log_dir);
    const auto path = log_dir / ("decision-context-" + compact_timestamp() + ".txt");
    std::ofstream output(path, std::ios::binary);
    output << "reason: " << reason << "\n\n";
    output << transcript << "\n";
    return path;
}

std::optional<std::string> run_external_decider(const Config& config,
                                                const std::string& reason,
                                                const std::string& transcript,
                                                const fs::path& log_dir,
                                                Logger& logger) {
    if (config.external_command.empty()) {
        logger.info("decider", "decision_mode=external but external_command is empty; falling back.", true);
        return std::nullopt;
    }

    auto command_line = config.external_command;
    const auto context_file = write_context_file(log_dir, reason, transcript);
    replace_all(command_line, "{context_file}", context_file.u8string());
    replace_all(command_line, "{reason}", reason);
    replace_all(command_line, "{workdir}", path_to_utf8(fs::absolute(fs::path(config.workdir.empty() ? "." : config.workdir))));
    replace_all(command_line, "{session_name}", effective_session_name(config));
    replace_all(command_line, "{mode}", config.mode);

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    if (!CreatePipe(&stdout_read, &stdout_write, &security, 0)) {
        logger.info("decider", "CreatePipe failed while starting external decider.", true);
        return std::nullopt;
    }

    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = stdout_write;
    startup.hStdError = stdout_write;

    PROCESS_INFORMATION process_info{};
    auto command_utf16 = utf8_to_wide(command_line);
    std::vector<wchar_t> mutable_command(command_utf16.begin(), command_utf16.end());
    mutable_command.push_back(L'\0');

    const BOOL created = CreateProcessW(nullptr,
                                        mutable_command.data(),
                                        nullptr,
                                        nullptr,
                                        TRUE,
                                        CREATE_NO_WINDOW,
                                        nullptr,
                                        nullptr,
                                        &startup,
                                        &process_info);

    close_handle(stdout_write);

    if (!created) {
        close_handle(stdout_read);
        logger.info("decider", "CreateProcessW failed while starting external decider.", true);
        return std::nullopt;
    }

    close_handle(process_info.hThread);

    std::string output;
    const auto deadline = Clock::now() + std::chrono::seconds(config.decision_timeout_seconds);
    bool timed_out = true;

    while (Clock::now() < deadline) {
        DWORD available = 0;
        if (PeekNamedPipe(stdout_read, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (ReadFile(stdout_read, chunk.data(), available, &read, nullptr) && read > 0) {
                chunk.resize(read);
                output += chunk;
            }
        }

        const DWORD wait = WaitForSingleObject(process_info.hProcess, 100);
        if (wait == WAIT_OBJECT_0) {
            timed_out = false;
            break;
        }
    }

    DWORD remaining = 0;
    while (PeekNamedPipe(stdout_read, nullptr, 0, nullptr, &remaining, nullptr) && remaining > 0) {
        std::string chunk(remaining, '\0');
        DWORD read = 0;
        if (!ReadFile(stdout_read, chunk.data(), remaining, &read, nullptr) || read == 0) {
            break;
        }
        chunk.resize(read);
        output += chunk;
    }

    if (WaitForSingleObject(process_info.hProcess, 0) != WAIT_OBJECT_0) {
        TerminateProcess(process_info.hProcess, 1);
        timed_out = true;
    }

    if (timed_out) {
        logger.info("decider", "external decider timed out; falling back.", true);
    }

    close_handle(stdout_read);
    close_handle(process_info.hProcess);

    if (timed_out) {
        return std::nullopt;
    }

    auto trimmed = trim(output);
    if (trimmed.empty()) {
        logger.info("decider", "external decider returned empty output; falling back.", true);
        return std::nullopt;
    }

    logger.info("decider", "external decider produced auto input: " + shorten_for_log(trimmed));
    return trimmed;
}

std::string build_auto_input(const Config& config,
                             const PendingAction& action,
                             const SharedState& state,
                             const fs::path& log_dir,
                             Logger& logger) {
    const auto transcript = action.transcript_override.empty() ? state.recent_transcript() : action.transcript_override;
    if (config.decision_mode == "external") {
        if (const auto decided = run_external_decider(config, action.reason, transcript, log_dir, logger)) {
            return *decided;
        }
    }

    if (action.kind == TriggerKind::RestartResume) {
        return render_template(config.resume_template, action.reason, transcript);
    }
    if (action.kind == TriggerKind::NewSessionResume) {
        const auto resume_text = render_template(config.new_session_resume_template, action.reason, transcript);
        if (config.new_session_message.empty()) {
            return resume_text;
        }
        if (resume_text.empty()) {
            return config.new_session_message;
        }
        return config.new_session_message + "\n" + resume_text;
    }

    return render_template(config.continue_message, action.reason, transcript);
}

void reader_loop(HANDLE pipe_handle,
                 const std::string& source,
                 const Config& config,
                 SharedState& state,
                 Logger& logger) {
    std::string pending;
    char buffer[2048];

    while (!g_stop_requested.load()) {
        DWORD read = 0;
        const BOOL ok = ReadFile(pipe_handle, buffer, sizeof(buffer), &read, nullptr);
        if (!ok || read == 0) {
            break;
        }

        pending.append(buffer, buffer + read);
        state.inspect_output_text(pending, config);

        std::size_t position = 0;
        while ((position = pending.find('\n')) != std::string::npos) {
            auto line = normalize_line(pending.substr(0, position));
            state.note_output_line(source, line, config);
            logger.child_line(source, line, config.echo_output);
            pending.erase(0, position + 1);
        }
    }

    if (!pending.empty()) {
        auto line = normalize_line(pending);
        state.note_output_line(source, line, config);
        logger.child_line(source, line, config.echo_output);
    }

    close_handle(pipe_handle);
}

bool launch_child(const Config& config,
                  SharedState& state,
                  Logger& logger,
                  ChildProcess& child,
                  std::thread& stdout_thread,
                  std::thread& stderr_thread) {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE stderr_read = nullptr;
    HANDLE stderr_write = nullptr;
    HANDLE stdin_read = nullptr;
    HANDLE stdin_write = nullptr;

    if (!CreatePipe(&stdout_read, &stdout_write, &security, 0) ||
        !CreatePipe(&stderr_read, &stderr_write, &security, 0) ||
        !CreatePipe(&stdin_read, &stdin_write, &security, 0)) {
        close_handle(stdout_read);
        close_handle(stdout_write);
        close_handle(stderr_read);
        close_handle(stderr_write);
        close_handle(stdin_read);
        close_handle(stdin_write);
        logger.info("watchdog", "CreatePipe failed while starting child process.", true);
        return false;
    }

    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdin_read;
    startup.hStdOutput = stdout_write;
    startup.hStdError = stderr_write;

    PROCESS_INFORMATION process_info{};
    auto command_utf16 = utf8_to_wide(config.command_line);
    std::vector<wchar_t> mutable_command(command_utf16.begin(), command_utf16.end());
    mutable_command.push_back(L'\0');
    const auto workdir_utf16 = utf8_to_wide(config.workdir);

    const BOOL created = CreateProcessW(nullptr,
                                        mutable_command.data(),
                                        nullptr,
                                        nullptr,
                                        TRUE,
                                        CREATE_NO_WINDOW,
                                        nullptr,
                                        workdir_utf16.empty() ? nullptr : workdir_utf16.c_str(),
                                        &startup,
                                        &process_info);

    close_handle(stdout_write);
    close_handle(stderr_write);
    close_handle(stdin_read);

    if (!created) {
        close_handle(stdout_read);
        close_handle(stderr_read);
        close_handle(stdin_write);
        logger.info("watchdog", "CreateProcessW failed while starting child command.", true);
        return false;
    }

    close_handle(process_info.hThread);

    child.process = process_info.hProcess;
    child.stdin_write = stdin_write;
    child.pid = process_info.dwProcessId;

    state.reset_for_new_session();
    state.note_system_event("started child pid=" + std::to_string(child.pid));
    logger.info("watchdog", "started child pid=" + std::to_string(child.pid));

    stdout_thread = std::thread(reader_loop, stdout_read, "stdout", std::cref(config), std::ref(state), std::ref(logger));
    stderr_thread = std::thread(reader_loop, stderr_read, "stderr", std::cref(config), std::ref(state), std::ref(logger));
    return true;
}

void stop_child(ChildProcess& child, Logger& logger) {
    if (child.stdin_write != nullptr) {
        close_handle(child.stdin_write);
    }

    if (child.process == nullptr) {
        return;
    }

    if (WaitForSingleObject(child.process, 1500) == WAIT_TIMEOUT) {
        logger.info("watchdog", "terminating child process after shutdown request.", true);
        TerminateProcess(child.process, 1);
    }
}

int run_spawn_mode(const Config& config,
                   const fs::path& config_path,
                   const fs::path& log_dir,
                   Logger& logger,
                   SharedState& state,
                   std::optional<PendingAction> startup_resume) {
    int restart_count = 0;
    std::string restart_reason;
    std::optional<PendingAction> pending_startup_resume = std::move(startup_resume);

    while (!g_stop_requested.load()) {
        ChildProcess child;
        std::thread stdout_thread;
        std::thread stderr_thread;

        if (!launch_child(config, state, logger, child, stdout_thread, stderr_thread)) {
            return 1;
        }

        std::optional<Clock::time_point> resume_due;
        std::optional<PendingAction> deferred_resume;
        if (pending_startup_resume) {
            deferred_resume = std::move(pending_startup_resume);
            pending_startup_resume.reset();
            resume_due = Clock::now() + std::chrono::seconds(std::max(config.initial_resume_delay_seconds, 0));
        }

        while (!g_stop_requested.load()) {
            if (resume_due && Clock::now() >= *resume_due) {
                if (deferred_resume) {
                    state.schedule_resume_with_transcript(deferred_resume->reason, deferred_resume->transcript_override);
                }
                resume_due.reset();
                deferred_resume.reset();
            }

            if (const auto pending = state.pop_due_action(config)) {
                const auto text = build_auto_input(config, *pending, state, log_dir, logger);
                if (!text.empty()) {
                    if (send_text_to_child(child.stdin_write, text, logger)) {
                        state.note_action_sent(*pending);
                    }
                }
            }

            const DWORD wait = WaitForSingleObject(child.process, 500);
            if (wait == WAIT_OBJECT_0) {
                break;
            }
            if (wait == WAIT_FAILED) {
                logger.info("watchdog", "WaitForSingleObject failed on child process.", true);
                g_stop_requested.store(true);
                break;
            }
        }

        if (g_stop_requested.load()) {
            stop_child(child, logger);
        }

        close_handle(child.stdin_write);

        if (stdout_thread.joinable()) {
            stdout_thread.join();
        }
            if (stderr_thread.joinable()) {
                stderr_thread.join();
            }

            const auto transcript_before_exit = state.recent_transcript();
            DWORD exit_code = 0;
            if (child.process != nullptr) {
                GetExitCodeProcess(child.process, &exit_code);
            }

            state.note_system_event("child exited with code " + std::to_string(exit_code));
            logger.info("watchdog", "child exited with code " + std::to_string(exit_code));
            save_session_snapshot(config,
                                  config_path,
                                  state,
                                  logger,
                                  "child exited with code " + std::to_string(exit_code));
            close_handle(child.process);

            if (g_stop_requested.load()) {
                break;
            }

        if (!config.restart_on_exit) {
            logger.info("watchdog", "restart_on_exit=false, supervisor will stop now.");
            break;
        }

        if (config.max_restarts >= 0 && restart_count >= config.max_restarts) {
            logger.info("watchdog", "max_restarts reached, supervisor will stop now.", true);
            break;
        }

            ++restart_count;
            if (!pending_startup_resume) {
                restart_reason = "child exited with code " + std::to_string(exit_code);
                pending_startup_resume =
                    PendingAction{TriggerKind::RestartResume, restart_reason, 3, transcript_before_exit};
            } else {
                restart_reason = pending_startup_resume->reason;
            }
            logger.info("watchdog",
                        "restarting child in " + std::to_string(config.restart_delay_seconds) + " seconds. reason: " +
                            restart_reason);

        if (!sleep_with_stop(std::max(config.restart_delay_seconds, 0))) {
            break;
        }
    }

    return 0;
}

BOOL WINAPI console_handler(DWORD signal) {
    switch (signal) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_stop_requested.store(true);
        return TRUE;
    default:
        return FALSE;
    }
}

void print_usage() {
    std::cout
        << "Usage: loopcode [codex args...]\n"
        << "       loopcode [--config path-to-ini] [--resume-last | --resume-session name | --resume-all-attached] "
           "[--menu] [--self-test name]\n";
}

RuntimeOptions parse_runtime_options(int argc, char* argv[]) {
    RuntimeOptions options;
    if (argc <= 1) {
        options.wrapper_mode = true;
    }

    bool passthrough_only = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (passthrough_only) {
            options.wrapper_mode = true;
            options.codex_passthrough_args.push_back(arg);
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            print_usage();
            std::exit(0);
        }
        if (arg == "--") {
            options.wrapper_mode = true;
            passthrough_only = true;
            continue;
        }
        if (arg == "--menu") {
            options.interactive_menu = true;
            continue;
        }
        if (arg == "--config" && i + 1 < argc) {
            options.config_path = argv[++i];
            options.config_explicit = true;
            continue;
        }
        if (arg == "--resume-last") {
            options.resume_last = true;
            continue;
        }
        if (arg == "--resume-session" && i + 1 < argc) {
            options.resume_session_name = argv[++i];
            continue;
        }
        if (arg == "--resume-all-attached") {
            options.resume_all_attached = true;
            continue;
        }
        if (arg == "--self-test" && i + 1 < argc) {
            options.self_test_name = argv[++i];
            continue;
        }
        if (looks_like_ini_path(arg)) {
            options.config_path = arg;
            options.config_explicit = true;
            continue;
        }
        options.wrapper_mode = true;
        options.codex_passthrough_args.push_back(arg);
    }

    const int resume_mode_count = (options.resume_last ? 1 : 0) + (options.resume_session_name ? 1 : 0) +
                                  (options.resume_all_attached ? 1 : 0);
    if (resume_mode_count > 1) {
        throw std::runtime_error("Use only one resume mode at a time.");
    }

    if (options.self_test_name && (options.resume_last || options.resume_session_name || options.resume_all_attached)) {
        throw std::runtime_error("Use --self-test by itself.");
    }

    if (options.wrapper_mode &&
        (options.interactive_menu || options.resume_last || options.resume_session_name || options.resume_all_attached)) {
        throw std::runtime_error("Codex wrapper mode cannot be combined with menu or resume options.");
    }

    if (options.interactive_menu && !options.config_explicit) {
        options.config_path = "examples/loopguard-attach.ini";
    }

    return options;
}

bool expect_self_test(bool condition, const std::string& message, std::string& error) {
    if (!condition) {
        error = message;
        return false;
    }
    return true;
}

int run_self_test(const std::string& name) {
    if (name == "attach-detection") {
        std::string error;
        if (!run_attach_detection_self_test(error)) {
            std::cerr << "[self-test] attach-detection failed: " << error << std::endl;
            return 1;
        }
        std::cout << "attach-detection self-test passed." << std::endl;
        return 0;
    }

    throw std::runtime_error("Unknown self-test: " + name);
}

}  // namespace

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleCtrlHandler(console_handler, TRUE);

    try {
        const auto options = parse_runtime_options(argc, argv);
        if (options.self_test_name) {
            return run_self_test(*options.self_test_name);
        }
        auto config_path = options.config_path;
        Config config;
        if (options.config_explicit || fs::exists(config_path)) {
            config = load_config(config_path);
        }
        auto runtime_options = options;
        std::optional<PendingAction> startup_resume;

        if (runtime_options.wrapper_mode) {
            config.mode = "spawn";
            config.command_line = build_codex_wrapper_command_line(runtime_options.codex_passthrough_args);
            if (config.workdir.empty() || config.workdir == ".") {
                config.workdir = path_to_utf8(fs::current_path());
            }
        }

        if (runtime_options.interactive_menu) {
            apply_interactive_menu(runtime_options, config);
            config.mode = "attach";
        }

        if (runtime_options.resume_last || runtime_options.resume_session_name) {
            auto saved = load_saved_session(config, runtime_options.resume_session_name, runtime_options.resume_last);
            if (!saved.config_path.empty() && fs::exists(saved.config_path)) {
                config_path = saved.config_path;
                config = load_config(config_path);
            }

            config.mode = "spawn";
            config.command_line = saved.command_line;
            config.workdir = saved.workdir;

            startup_resume = PendingAction{
                TriggerKind::RestartResume,
                "manual resume of saved session \"" + saved.display_name + "\"",
                3,
                saved.transcript,
            };
        }

        const auto log_dir = path_from_utf8(config.log_dir);
        Logger logger(log_dir / "loopguard.log");
        SharedState state(static_cast<std::size_t>(std::max(config.transcript_keep_lines, 20)));

        logger.info("watchdog", "loaded config from " + config_path.string() + " mode=" + config.mode);
        if (runtime_options.resume_all_attached || runtime_options.attached_resume_entries_override) {
            const auto entries = runtime_options.attached_resume_entries_override
                                     ? *runtime_options.attached_resume_entries_override
                                     : load_attach_inventory(config);
            logger.info("session",
                        "loaded attached-window inventory with " + std::to_string(entries.size()) + " entr" +
                            (entries.size() == 1 ? "y" : "ies"));

            int launched = 0;
            for (const auto& entry : entries) {
                if (launch_attached_resume_entry(entry, logger)) {
                    ++launched;
                }
            }
            logger.info("session", "launched " + std::to_string(launched) + " attached resume command(s).");
            std::this_thread::sleep_for(std::chrono::seconds(std::max(config.initial_resume_delay_seconds, 2)));
        }
        if (startup_resume) {
            logger.info("session", "resuming saved session in workdir=" + config.workdir);
        }

        const int exit_code = config.mode == "attach"
                                  ? run_attach_mode(config, log_dir, logger, state, runtime_options.selected_attach_hwnds)
                                                      : run_spawn_mode(config, config_path, log_dir, logger, state, startup_resume);
        save_session_snapshot(config, config_path, state, logger, "loopguard exited");
        logger.info("watchdog", "supervisor exiting.");
        return exit_code;
    } catch (const std::exception& error) {
        std::cerr << "[fatal] " << error.what() << std::endl;
        return 1;
    }
}

struct WindowInfo {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    std::string title;
    std::string class_name;
    std::string selection_reason;
};

struct ProcessInfo {
    DWORD pid = 0;
    DWORD parent_pid = 0;
    std::string exe_name;
};

struct ProcessTreeResolution {
    std::vector<DWORD> target_pids;
    std::unordered_set<DWORD> terminal_pids;
};

struct RemoteUnicodeString {
    USHORT Length = 0;
    USHORT MaximumLength = 0;
    PWSTR Buffer = nullptr;
};

struct RemoteCurrentDirectory {
    RemoteUnicodeString DosPath;
    HANDLE Handle = nullptr;
};

struct RemoteProcessParameters {
    ULONG MaximumLength = 0;
    ULONG Length = 0;
    ULONG Flags = 0;
    ULONG DebugFlags = 0;
    HANDLE ConsoleHandle = nullptr;
    ULONG ConsoleFlags = 0;
    HANDLE StandardInput = nullptr;
    HANDLE StandardOutput = nullptr;
    HANDLE StandardError = nullptr;
    RemoteCurrentDirectory CurrentDirectory;
    RemoteUnicodeString DllPath;
    RemoteUnicodeString ImagePathName;
    RemoteUnicodeString CommandLine;
};

struct RemotePeb {
    BYTE Reserved1[2];
    BYTE BeingDebugged;
    BYTE Reserved2[1];
    PVOID Reserved3[2];
    PVOID Ldr;
    RemoteProcessParameters* ProcessParameters = nullptr;
};

struct AttachSession {
    WindowInfo window;
    SharedState state;
    std::string last_snapshot;
    bool snapshot_confirmed = false;
    std::string snapshot_confirmation_reason;
    bool unconfirmed_logged = false;

    explicit AttachSession(std::size_t keep_lines)
        : state(keep_lines) {}
};

bool has_title_filters(const Config& config) {
    return !config.window_title_contains.empty() || !config.window_class_contains.empty();
}

std::string attach_snapshot_confirmation_reason(const std::string& snapshot, const Config& config) {
    if (snapshot.empty()) {
        return {};
    }

    const auto haystack = to_lower_ascii(snapshot);
    if (const auto matched = match_any_substring(haystack, config.wait_patterns); !matched.empty()) {
        return "wait pattern: " + matched;
    }
    if (const auto matched = match_any_substring(haystack, config.recoverable_error_patterns); !matched.empty()) {
        return "recoverable_error pattern: " + matched;
    }
    if (const auto matched = match_any_substring(haystack, config.attach_visible_text_patterns); !matched.empty()) {
        return "visible-text hint: " + matched;
    }

    return {};
}

class ComApartment {
public:
    ComApartment() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            throw std::runtime_error("CoInitializeEx failed.");
        }
        initialized_ = SUCCEEDED(hr);
    }

    ~ComApartment() {
        if (initialized_) {
            CoUninitialize();
        }
    }

private:
    bool initialized_ = false;
};

class UiAutomationClient {
public:
    UiAutomationClient() {
        const HRESULT hr = CoCreateInstance(CLSID_CUIAutomation,
                                            nullptr,
                                            CLSCTX_INPROC_SERVER,
                                            IID_PPV_ARGS(&automation_));
        if (FAILED(hr) || !automation_) {
            throw std::runtime_error("CoCreateInstance(CUIAutomation) failed.");
        }
    }

    std::string read_visible_text(HWND hwnd) const {
        if (hwnd == nullptr) {
            return {};
        }

        ComPtr<IUIAutomationElement> element;
        if (FAILED(automation_->ElementFromHandle(hwnd, &element)) || !element) {
            return {};
        }

        if (const auto direct = try_text_from_element(element.Get()); !direct.empty()) {
            return direct;
        }

        VARIANT variant{};
        variant.vt = VT_BOOL;
        variant.boolVal = VARIANT_TRUE;

        ComPtr<IUIAutomationCondition> condition;
        if (SUCCEEDED(automation_->CreatePropertyCondition(UIA_IsTextPatternAvailablePropertyId, variant, &condition)) &&
            condition) {
            ComPtr<IUIAutomationElement> match;
            if (SUCCEEDED(element->FindFirst(TreeScope_Subtree, condition.Get(), &match)) && match) {
                if (const auto text = try_text_from_element(match.Get()); !text.empty()) {
                    return text;
                }
            }
        }

        if (SUCCEEDED(automation_->CreatePropertyCondition(UIA_IsValuePatternAvailablePropertyId, variant, &condition)) &&
            condition) {
            ComPtr<IUIAutomationElement> match;
            if (SUCCEEDED(element->FindFirst(TreeScope_Subtree, condition.Get(), &match)) && match) {
                if (const auto text = try_value_from_element(match.Get()); !text.empty()) {
                    return text;
                }
            }
        }

        return {};
    }

private:
    static std::string try_text_from_element(IUIAutomationElement* element) {
        if (element == nullptr) {
            return {};
        }

        ComPtr<IUIAutomationTextPattern> text_pattern;
        if (FAILED(element->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&text_pattern))) || !text_pattern) {
            return {};
        }

        ComPtr<IUIAutomationTextRange> range;
        if (FAILED(text_pattern->get_DocumentRange(&range)) || !range) {
            return {};
        }

        BSTR text = nullptr;
        if (FAILED(range->GetText(-1, &text)) || text == nullptr) {
            return {};
        }

        std::string result = bstr_to_utf8(text);
        SysFreeString(text);
        return result;
    }

    static std::string try_value_from_element(IUIAutomationElement* element) {
        if (element == nullptr) {
            return {};
        }

        ComPtr<IUIAutomationValuePattern> value_pattern;
        if (FAILED(element->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&value_pattern))) || !value_pattern) {
            return {};
        }

        BSTR value = nullptr;
        if (FAILED(value_pattern->get_CurrentValue(&value)) || value == nullptr) {
            return {};
        }

        std::string result = bstr_to_utf8(value);
        SysFreeString(value);
        return result;
    }

    ComPtr<IUIAutomation> automation_;
};

struct FindWindowContext {
    const Config* config = nullptr;
    std::vector<WindowInfo> matches;
};

struct FindWindowByPidContext {
    const std::unordered_set<DWORD>* candidate_pids = nullptr;
    std::vector<WindowInfo> matches;
};

std::string window_title(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring buffer(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(hwnd, buffer.data(), length + 1);
    buffer.resize(length);
    return wide_to_utf8(buffer);
}

std::string window_class_name(HWND hwnd) {
    wchar_t buffer[256];
    const int length = GetClassNameW(hwnd, buffer, static_cast<int>(sizeof(buffer) / sizeof(buffer[0])));
    return length > 0 ? wide_to_utf8(std::wstring(buffer, buffer + length)) : std::string{};
}

BOOL CALLBACK enum_windows_proc(HWND hwnd, LPARAM lparam) {
    auto* context = reinterpret_cast<FindWindowContext*>(lparam);
    if (context == nullptr || context->config == nullptr) {
        return TRUE;
    }

    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) {
        return TRUE;
    }

    const auto title = window_title(hwnd);
    const auto class_name = window_class_name(hwnd);
    const auto title_lower = to_lower_ascii(title);
    const auto class_lower = to_lower_ascii(class_name);

    const auto title_filter = to_lower_ascii(context->config->window_title_contains);
    const auto class_filter = to_lower_ascii(context->config->window_class_contains);

    if (!title_filter.empty() && title_lower.find(title_filter) == std::string::npos) {
        return TRUE;
    }

    if (!class_filter.empty() && class_lower.find(class_filter) == std::string::npos) {
        return TRUE;
    }

    WindowInfo info;
    info.hwnd = hwnd;
    info.title = title;
    info.class_name = class_name;
    GetWindowThreadProcessId(hwnd, &info.pid);
    context->matches.push_back(std::move(info));
    return TRUE;
}

BOOL CALLBACK enum_windows_by_pid_proc(HWND hwnd, LPARAM lparam) {
    auto* context = reinterpret_cast<FindWindowByPidContext*>(lparam);
    if (context == nullptr || context->candidate_pids == nullptr) {
        return TRUE;
    }

    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) {
        return TRUE;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!context->candidate_pids->count(pid)) {
        return TRUE;
    }

    WindowInfo info;
    info.hwnd = hwnd;
    info.pid = pid;
    info.title = window_title(hwnd);
    info.class_name = window_class_name(hwnd);
    context->matches.push_back(std::move(info));
    return TRUE;
}

std::vector<WindowInfo> annotate_window_matches(std::vector<WindowInfo> matches, const std::string& strategy_label) {
    if (matches.empty()) {
        return {};
    }

    const HWND foreground = GetForegroundWindow();
    for (auto& match : matches) {
        if (match.hwnd == foreground) {
            match.selection_reason = strategy_label + " foreground match";
        } else {
            match.selection_reason = strategy_label + " match";
        }
    }

    if (matches.size() == 1 && matches.front().selection_reason == strategy_label + " match") {
        matches.front().selection_reason = strategy_label + " single match";
    }
    return matches;
}

bool try_snapshot_processes(std::unordered_map<DWORD, ProcessInfo>& processes, std::string& error) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        error = "CreateToolhelp32Snapshot failed with error " + std::to_string(GetLastError()) + ".";
        return false;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot, &entry)) {
        error = "Process32FirstW failed with error " + std::to_string(GetLastError()) + ".";
        close_handle(snapshot);
        return false;
    }

    do {
        ProcessInfo info;
        info.pid = entry.th32ProcessID;
        info.parent_pid = entry.th32ParentProcessID;
        info.exe_name = to_lower_ascii(wide_to_utf8(entry.szExeFile));
        processes.emplace(info.pid, std::move(info));
    } while (Process32NextW(snapshot, &entry));

    close_handle(snapshot);
    return true;
}

ProcessTreeResolution resolve_process_tree_candidates(const std::unordered_map<DWORD, ProcessInfo>& processes,
                                                      const std::vector<std::string>& target_process_names,
                                                      const std::vector<std::string>& terminal_process_names) {
    ProcessTreeResolution resolution;

    for (const auto& [pid, info] : processes) {
        if (!process_name_matches(info.exe_name, target_process_names)) {
            continue;
        }

        resolution.target_pids.push_back(pid);

        std::unordered_set<DWORD> visited;
        DWORD current_pid = pid;
        for (int depth = 0; current_pid != 0 && depth < 64; ++depth) {
            if (!visited.insert(current_pid).second) {
                break;
            }

            const auto current = processes.find(current_pid);
            if (current == processes.end()) {
                break;
            }

            if (process_name_matches(current->second.exe_name, terminal_process_names)) {
                resolution.terminal_pids.insert(current->second.pid);
                break;
            }

            current_pid = current->second.parent_pid;
        }
    }

    return resolution;
}

template <typename T>
bool read_remote_value(HANDLE process, LPCVOID address, T& value) {
    SIZE_T bytes_read = 0;
    return ReadProcessMemory(process, address, &value, sizeof(T), &bytes_read) && bytes_read == sizeof(T);
}

std::optional<std::string> normalize_workdir(std::string candidate) {
    candidate = trim(candidate);
    if (candidate.empty()) {
        return std::nullopt;
    }

    std::error_code error;
    fs::path path = path_from_utf8(candidate);
    path = fs::absolute(path, error);
    if (error) {
        return std::nullopt;
    }

    if (fs::exists(path, error) && fs::is_regular_file(path, error)) {
        path = path.parent_path();
    }
    if (error || path.empty()) {
        return std::nullopt;
    }

    return path_to_utf8(path.lexically_normal());
}

std::optional<std::string> try_read_process_current_directory(DWORD pid) {
    using NtQueryInformationProcessFn = NTSTATUS(NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);

    const auto ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return std::nullopt;
    }

    const auto query_process =
        reinterpret_cast<NtQueryInformationProcessFn>(GetProcAddress(ntdll, "NtQueryInformationProcess"));
    if (query_process == nullptr) {
        return std::nullopt;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (process == nullptr) {
        return std::nullopt;
    }

    PROCESS_BASIC_INFORMATION basic_info{};
    const auto status =
        query_process(process, ProcessBasicInformation, &basic_info, sizeof(basic_info), nullptr);
    if (status < 0 || basic_info.PebBaseAddress == nullptr) {
        close_handle(process);
        return std::nullopt;
    }

    RemotePeb peb{};
    if (!read_remote_value(process, basic_info.PebBaseAddress, peb) || peb.ProcessParameters == nullptr) {
        close_handle(process);
        return std::nullopt;
    }

    RemoteProcessParameters parameters{};
    if (!read_remote_value(process, peb.ProcessParameters, parameters) ||
        parameters.CurrentDirectory.DosPath.Buffer == nullptr || parameters.CurrentDirectory.DosPath.Length == 0) {
        close_handle(process);
        return std::nullopt;
    }

    std::wstring buffer(parameters.CurrentDirectory.DosPath.Length / sizeof(wchar_t), L'\0');
    SIZE_T bytes_read = 0;
    const bool ok = ReadProcessMemory(process,
                                      parameters.CurrentDirectory.DosPath.Buffer,
                                      buffer.data(),
                                      parameters.CurrentDirectory.DosPath.Length,
                                      &bytes_read) &&
                    bytes_read == parameters.CurrentDirectory.DosPath.Length;
    close_handle(process);
    if (!ok) {
        return std::nullopt;
    }

    return normalize_workdir(wide_to_utf8(buffer));
}

DWORD find_terminal_ancestor_pid(const std::unordered_map<DWORD, ProcessInfo>& processes,
                                 DWORD target_pid,
                                 const std::vector<std::string>& terminal_process_names) {
    std::unordered_set<DWORD> visited;
    DWORD current_pid = target_pid;
    for (int depth = 0; current_pid != 0 && depth < 64; ++depth) {
        if (!visited.insert(current_pid).second) {
            break;
        }

        const auto current = processes.find(current_pid);
        if (current == processes.end()) {
            break;
        }

        if (process_name_matches(current->second.exe_name, terminal_process_names)) {
            return current->second.pid;
        }

        current_pid = current->second.parent_pid;
    }

    return 0;
}

std::optional<std::string> resolve_target_workdir(const std::unordered_map<DWORD, ProcessInfo>& processes,
                                                  DWORD target_pid,
                                                  DWORD terminal_pid) {
    if (const auto direct = try_read_process_current_directory(target_pid)) {
        return direct;
    }

    std::unordered_set<DWORD> visited;
    DWORD current_pid = target_pid;
    for (int depth = 0; current_pid != 0 && depth < 64; ++depth) {
        if (!visited.insert(current_pid).second) {
            break;
        }

        const auto current = processes.find(current_pid);
        if (current == processes.end()) {
            break;
        }

        if (current_pid != target_pid) {
            if (const auto current_dir = try_read_process_current_directory(current_pid)) {
                return current_dir;
            }
        }
        if (current_pid == terminal_pid) {
            break;
        }

        current_pid = current->second.parent_pid;
    }

    return std::nullopt;
}

std::vector<AttachInventoryEntry> discover_attach_inventory_entries(const Config& config) {
    std::unordered_map<DWORD, ProcessInfo> processes;
    std::string error;
    if (!try_snapshot_processes(processes, error)) {
        return {};
    }

    std::vector<AttachInventoryEntry> entries;
    for (const auto& [pid, info] : processes) {
        if (!process_name_matches(info.exe_name, config.target_process_names)) {
            continue;
        }

        const DWORD terminal_pid = find_terminal_ancestor_pid(processes, pid, config.terminal_process_names);
        if (terminal_pid == 0) {
            continue;
        }

        const auto workdir = resolve_target_workdir(processes, pid, terminal_pid);
        if (!workdir) {
            continue;
        }

        AttachInventoryEntry entry;
        entry.agent_name = strip_exe_suffix(info.exe_name);
        entry.workdir = *workdir;
        entries.push_back(std::move(entry));
    }

    std::sort(entries.begin(), entries.end(), [](const AttachInventoryEntry& left, const AttachInventoryEntry& right) {
        if (left.agent_name != right.agent_name) {
            return left.agent_name < right.agent_name;
        }
        return left.workdir < right.workdir;
    });
    entries.erase(std::unique(entries.begin(),
                              entries.end(),
                              [](const AttachInventoryEntry& left, const AttachInventoryEntry& right) {
                                  return left.agent_name == right.agent_name && left.workdir == right.workdir;
                              }),
                  entries.end());
    return entries;
}

std::vector<WindowInfo> find_windows_by_pid_set(const std::unordered_set<DWORD>& candidate_pids) {
    FindWindowByPidContext context;
    context.candidate_pids = &candidate_pids;
    EnumWindows(enum_windows_by_pid_proc, reinterpret_cast<LPARAM>(&context));
    return context.matches;
}

std::vector<WindowInfo> find_target_windows_by_title_match(const Config& config) {
    FindWindowContext context;
    context.config = &config;
    EnumWindows(enum_windows_proc, reinterpret_cast<LPARAM>(&context));
    return annotate_window_matches(std::move(context.matches), "title_match");
}

std::vector<WindowInfo> find_target_windows_by_process_tree(const Config& config) {
    std::unordered_map<DWORD, ProcessInfo> processes;
    std::string error;
    if (!try_snapshot_processes(processes, error)) {
        return {};
    }

    const auto resolution =
        resolve_process_tree_candidates(processes, config.target_process_names, config.terminal_process_names);
    if (resolution.terminal_pids.empty()) {
        return {};
    }

    auto matches = find_windows_by_pid_set(resolution.terminal_pids);
    const auto label = "process_tree (" + std::to_string(resolution.target_pids.size()) + " target process(es), " +
                       std::to_string(matches.size()) + " terminal window match(es))";
    return annotate_window_matches(std::move(matches), label);
}

std::vector<WindowInfo> find_target_windows(const Config& config) {
    if (config.attach_strategy == "title_match") {
        return find_target_windows_by_title_match(config);
    }

    if (config.attach_strategy == "process_tree") {
        return find_target_windows_by_process_tree(config);
    }

    if (auto found = find_target_windows_by_process_tree(config); !found.empty()) {
        return found;
    }

    if (has_title_filters(config)) {
        return find_target_windows_by_title_match(config);
    }

    return {};
}

std::optional<WindowInfo> find_target_window(const Config& config) {
    auto matches = find_target_windows(config);
    if (matches.empty()) {
        return std::nullopt;
    }

    const HWND foreground = GetForegroundWindow();
    for (auto& match : matches) {
        if (match.hwnd == foreground) {
            return match;
        }
    }

    auto selected = matches.front();
    if (matches.size() > 1) {
        selected.selection_reason += "; first of " + std::to_string(matches.size()) +
                                     " matches chosen because no candidate is in the foreground";
    }
    return selected;
}

std::string attach_window_summary(const WindowInfo& info) {
    return "hwnd=" + hex_uintptr(reinterpret_cast<std::uintptr_t>(info.hwnd)) + " pid=" + std::to_string(info.pid) +
           " title=" + shorten_for_log(info.title);
}

std::vector<WindowInfo> filter_target_windows_by_hwnd(const std::vector<WindowInfo>& windows,
                                                      const std::unordered_set<std::uintptr_t>& allowed_hwnds) {
    if (allowed_hwnds.empty()) {
        return windows;
    }

    std::vector<WindowInfo> filtered;
    for (const auto& window : windows) {
        if (allowed_hwnds.count(reinterpret_cast<std::uintptr_t>(window.hwnd))) {
            filtered.push_back(window);
        }
    }
    return filtered;
}

void print_attach_window_candidates(const std::vector<WindowInfo>& windows) {
    std::cout << "\n当前可附加窗口:\n";
    for (std::size_t i = 0; i < windows.size(); ++i) {
        std::cout << "  " << (i + 1) << ". " << attach_window_summary(windows[i]) << '\n';
    }
}

void print_attach_inventory_candidates(const std::vector<AttachInventoryEntry>& entries) {
    std::cout << "\n当前可恢复目录:\n";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        std::cout << "  " << (i + 1) << ". [" << entries[i].agent_name << "] " << entries[i].workdir << '\n';
    }
}

void apply_interactive_menu(RuntimeOptions& options, const Config& config) {
    while (true) {
        std::cout << "\nLoopGuard 菜单\n";
        std::cout << "  1. 附加全部\n";
        std::cout << "  2. 附加指定\n";
        std::cout << "  3. 恢复\n";
        std::cout << "  4. 退出\n";

        const auto choice = prompt_line("选择操作 [1-4]: ");
        if (choice == "1") {
            return;
        }

        if (choice == "2") {
            const auto windows = find_target_windows(config);
            if (windows.empty()) {
                std::cout << "没有发现可附加窗口。\n";
                continue;
            }

            print_attach_window_candidates(windows);
            const auto raw_indexes = prompt_line("输入窗口编号，可多选，用逗号分隔: ");
            const auto indexes = parse_selection_indexes(raw_indexes, static_cast<int>(windows.size()));
            if (indexes.empty()) {
                std::cout << "没有选中有效窗口。\n";
                continue;
            }

            options.selected_attach_hwnds.clear();
            for (const int index : indexes) {
                options.selected_attach_hwnds.push_back(
                    reinterpret_cast<std::uintptr_t>(windows[static_cast<std::size_t>(index - 1)].hwnd));
            }
            return;
        }

        if (choice == "3") {
            std::cout << "\n恢复方式\n";
            std::cout << "  1. 恢复全部\n";
            std::cout << "  2. 恢复指定\n";
            std::cout << "  3. 返回\n";

            const auto resume_choice = prompt_line("选择恢复方式 [1-3]: ");
            if (resume_choice == "3") {
                continue;
            }
            if (resume_choice == "1") {
                try {
                    const auto entries = load_attach_inventory(config);
                    if (entries.empty()) {
                        std::cout << "没有可恢复目录。\n";
                        continue;
                    }
                } catch (const std::exception& ex) {
                    std::cout << ex.what() << '\n';
                    continue;
                }
                options.resume_all_attached = true;
                return;
            }
            if (resume_choice == "2") {
                std::vector<AttachInventoryEntry> entries;
                try {
                    entries = load_attach_inventory(config);
                } catch (const std::exception& ex) {
                    std::cout << ex.what() << '\n';
                    continue;
                }
                print_attach_inventory_candidates(entries);
                const auto raw_indexes = prompt_line("输入要恢复的编号，可多选，用逗号分隔: ");
                const auto indexes = parse_selection_indexes(raw_indexes, static_cast<int>(entries.size()));
                if (indexes.empty()) {
                    std::cout << "没有选中有效目录。\n";
                    continue;
                }

                std::vector<AttachInventoryEntry> selected_entries;
                for (const int index : indexes) {
                    selected_entries.push_back(entries[static_cast<std::size_t>(index - 1)]);
                }
                options.attached_resume_entries_override = std::move(selected_entries);
                return;
            }

            std::cout << "无效选择。\n";
            continue;
        }

        if (choice == "4") {
            std::exit(0);
        }

        std::cout << "无效选择。\n";
    }
}

bool focus_window(HWND hwnd) {
    if (hwnd == nullptr || !IsWindow(hwnd)) {
        return false;
    }

    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }

    const HWND foreground = GetForegroundWindow();
    const DWORD foreground_thread = foreground != nullptr ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const DWORD current_thread = GetCurrentThreadId();

    if (foreground_thread != 0 && foreground_thread != current_thread) {
        AttachThreadInput(foreground_thread, current_thread, TRUE);
    }

    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    if (foreground_thread != 0 && foreground_thread != current_thread) {
        AttachThreadInput(foreground_thread, current_thread, FALSE);
    }

    return GetForegroundWindow() == hwnd;
}

void append_unicode_key(std::vector<INPUT>& inputs, wchar_t ch) {
    INPUT down{};
    down.type = INPUT_KEYBOARD;
    down.ki.wScan = ch;
    down.ki.dwFlags = KEYEVENTF_UNICODE;

    INPUT up = down;
    up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;

    inputs.push_back(down);
    inputs.push_back(up);
}

void append_vk_key(std::vector<INPUT>& inputs, WORD vk) {
    INPUT down{};
    down.type = INPUT_KEYBOARD;
    down.ki.wVk = vk;

    INPUT up = down;
    up.ki.dwFlags = KEYEVENTF_KEYUP;

    inputs.push_back(down);
    inputs.push_back(up);
}

bool send_text_to_window(HWND hwnd, const std::string& text, Logger& logger) {
    if (hwnd == nullptr || !IsWindow(hwnd)) {
        logger.info("watchdog", "target window is unavailable; attach auto input skipped.", true);
        return false;
    }

    if (!focus_window(hwnd)) {
        logger.info("watchdog", "failed to focus target window before sending auto input.", true);
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    const auto wide_text = utf8_to_wide(text);
    std::vector<INPUT> inputs;
    inputs.reserve(wide_text.size() * 2 + 4);

    for (wchar_t ch : wide_text) {
        if (ch == L'\r') {
            continue;
        }
        if (ch == L'\n') {
            append_vk_key(inputs, VK_RETURN);
            continue;
        }
        append_unicode_key(inputs, ch);
    }

    if (wide_text.empty() || wide_text.back() != L'\n') {
        append_vk_key(inputs, VK_RETURN);
    }

    const UINT sent = SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    if (sent != inputs.size()) {
        logger.info("watchdog", "SendInput did not submit the full auto input payload.", true);
        return false;
    }

    logger.info("watchdog", "sent auto input to attached window: " + shorten_for_log(text));
    return true;
}

void note_snapshot_lines(SharedState& state,
                         Logger& logger,
                         const Config& config,
                         const std::string& snapshot,
                         const std::string& source) {
    const auto clipped = tail_lines(snapshot, 40);
    for (const auto& line : split_lines(clipped)) {
        if (trim(line).empty()) {
            continue;
        }
        state.note_output_line(source, line, config);
        logger.child_line(source, line, config.echo_output);
    }
}

std::string attach_snapshot_source(const WindowInfo& info) {
    return "attach hwnd=" + hex_uintptr(reinterpret_cast<std::uintptr_t>(info.hwnd)) + " pid=" +
           std::to_string(info.pid);
}

std::string resume_command_for_agent(const std::string& agent_name) {
    const auto normalized = to_lower_ascii(strip_exe_suffix(agent_name));
    if (normalized == "codex") {
        return "codex resume --last";
    }
    if (normalized == "claude") {
        return "claude --continue";
    }
    return strip_exe_suffix(agent_name);
}

bool launch_attached_resume_entry(const AttachInventoryEntry& entry, Logger& logger) {
    const auto workdir = normalize_workdir(entry.workdir);
    if (!workdir) {
        logger.info("session",
                    "skipping attached resume for agent=" + entry.agent_name + " because workdir is invalid: " +
                        entry.workdir,
                    true);
        return false;
    }

    const auto resume_command = resume_command_for_agent(entry.agent_name);
    if (resume_command.empty()) {
        logger.info("session", "skipping attached resume because agent command is empty.", true);
        return false;
    }

    const auto workdir_wide = utf8_to_wide(*workdir);
    const auto command_line = std::wstring(L"wt.exe -d ") + quote_windows_argument(workdir_wide) + L" " +
                              utf8_to_wide(resume_command);
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process_info{};
    const BOOL created = CreateProcessW(nullptr,
                                        mutable_command.data(),
                                        nullptr,
                                        nullptr,
                                        FALSE,
                                        CREATE_NEW_CONSOLE,
                                        nullptr,
                                        workdir_wide.c_str(),
                                        &startup,
                                        &process_info);
    if (!created) {
        logger.info("session",
                    "failed to launch attached resume for agent=" + entry.agent_name + " workdir=" + *workdir +
                        " command=" + resume_command,
                    true);
        return false;
    }

    close_handle(process_info.hThread);
    close_handle(process_info.hProcess);
    logger.info("session",
                "launched attached resume for agent=" + entry.agent_name + " workdir=" + *workdir +
                    " command=" + resume_command);
    return true;
}

int run_attach_mode(const Config& config,
                    const fs::path& log_dir,
                    Logger& logger,
                    SharedState& state,
                    const std::vector<std::uintptr_t>& selected_attach_hwnds = {}) {
    ComApartment apartment;
    UiAutomationClient automation;
    std::map<HWND, std::unique_ptr<AttachSession>> sessions;
    bool waiting_logged = false;
    std::string last_attach_inventory_signature;
    const auto keep_lines = static_cast<std::size_t>(std::max(config.transcript_keep_lines, 20));
    const std::unordered_set<std::uintptr_t> selected_hwnd_set(selected_attach_hwnds.begin(), selected_attach_hwnds.end());

    logger.info("watchdog",
                "attach mode started. strategy=" + config.attach_strategy + " target_processes=" +
                    join_patterns(config.target_process_names) + " terminal_processes=" +
                    join_patterns(config.terminal_process_names) + " scope=" + config.attach_window_scope +
                    " title~\"" + config.window_title_contains + "\" class~\"" + config.window_class_contains + "\"");

    while (!g_stop_requested.load()) {
        auto targets = filter_target_windows_by_hwnd(find_target_windows(config), selected_hwnd_set);
        if (config.attach_window_scope == "single" && targets.size() > 1) {
            if (const auto selected = find_target_window(config)) {
                const auto selected_hwnd = reinterpret_cast<std::uintptr_t>(selected->hwnd);
                if (selected_hwnd_set.empty() || selected_hwnd_set.count(selected_hwnd)) {
                    targets = {*selected};
                } else {
                    targets.clear();
                }
            } else {
                targets.clear();
            }
        }

        const auto inventory_entries = discover_attach_inventory_entries(config);
        const auto inventory_signature = attach_inventory_signature(inventory_entries);
        if (inventory_signature != last_attach_inventory_signature) {
            save_attach_inventory(config, inventory_entries, logger, "attach scan updated");
            last_attach_inventory_signature = inventory_signature;
        }

        if (targets.empty()) {
            if (!waiting_logged) {
                logger.info("watchdog", "target window not found yet; still waiting.");
                waiting_logged = true;
            }
        } else {
            waiting_logged = false;
        }

        std::unordered_set<HWND> active_windows;
        for (const auto& target : targets) {
            active_windows.insert(target.hwnd);

            auto& session = sessions[target.hwnd];
            if (!session) {
                session = std::make_unique<AttachSession>(keep_lines);
                session->window = target;
                state.note_system_event("attached window " + attach_window_summary(target) + " via " +
                                        target.selection_reason);
                logger.info("watchdog",
                            "attached window " + attach_window_summary(target) + " via " + target.selection_reason);
            } else {
                session->window = target;
            }
        }

        for (auto it = sessions.begin(); it != sessions.end();) {
            if (!active_windows.count(it->first) || !IsWindow(it->first)) {
                logger.info("watchdog", "detached window " + attach_window_summary(it->second->window));
                state.note_system_event("detached window " + attach_window_summary(it->second->window));
                it = sessions.erase(it);
                continue;
            }
            ++it;
        }

        for (auto& [hwnd, session] : sessions) {
            const auto snapshot = automation.read_visible_text(hwnd);
            if (snapshot.empty() || snapshot == session->last_snapshot) {
                continue;
            }

            const auto confirmation_reason = attach_snapshot_confirmation_reason(snapshot, config);
            const bool confirmed = !confirmation_reason.empty();
            if (confirmed != session->snapshot_confirmed ||
                confirmation_reason != session->snapshot_confirmation_reason) {
                if (confirmed) {
                    logger.info("watchdog",
                                "attach visible-text confirmation matched " + confirmation_reason + " for " +
                                    attach_window_summary(session->window));
                } else if (!session->unconfirmed_logged) {
                    logger.info("watchdog",
                                "attach window found, but visible text does not currently look like the target agent "
                                "for " +
                                    attach_window_summary(session->window) +
                                    "; idle auto input will stay paused.");
                }
            }

            session->snapshot_confirmed = confirmed;
            session->snapshot_confirmation_reason = confirmation_reason;
            session->unconfirmed_logged = !confirmed;
            session->state.inspect_output_text(snapshot, config);
            note_snapshot_lines(session->state, logger, config, snapshot, attach_snapshot_source(session->window));
            session->last_snapshot = snapshot;
        }

        for (auto& [hwnd, session] : sessions) {
            if (const auto pending = session->state.pop_due_action(config, session->snapshot_confirmed)) {
                const auto text = build_auto_input(config, *pending, session->state, log_dir, logger);
                if (!text.empty()) {
                    logger.info("watchdog",
                                "dispatching auto input to " + attach_window_summary(session->window) +
                                    " reason=" + pending->reason);
                    if (send_text_to_window(hwnd, text, logger)) {
                        session->state.note_action_sent(*pending);
                    }
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(std::max(config.attach_poll_millis, 250)));
    }

    return 0;
}

bool run_attach_detection_self_test(std::string& error) {
    std::unordered_map<DWORD, ProcessInfo> processes;
    processes.emplace(100, ProcessInfo{100, 1, "windowsterminal.exe"});
    processes.emplace(110, ProcessInfo{110, 100, "pwsh.exe"});
    processes.emplace(120, ProcessInfo{120, 110, "node.exe"});
    processes.emplace(130, ProcessInfo{130, 120, "codex.exe"});
    processes.emplace(200, ProcessInfo{200, 1, "openconsole.exe"});
    processes.emplace(210, ProcessInfo{210, 200, "claude.exe"});
    processes.emplace(300, ProcessInfo{300, 1, "powershell.exe"});
    processes.emplace(310, ProcessInfo{310, 300, "git.exe"});

    const auto resolution =
        resolve_process_tree_candidates(processes, {"codex", "claude.exe"}, {"WindowsTerminal.exe", "OpenConsole.exe"});
    if (!expect_self_test(resolution.target_pids.size() == 2, "expected two target agent processes", error)) {
        return false;
    }
    if (!expect_self_test(resolution.terminal_pids.size() == 2, "expected two terminal ancestors", error)) {
        return false;
    }
    if (!expect_self_test(resolution.terminal_pids.count(100) == 1 && resolution.terminal_pids.count(200) == 1,
                          "expected WindowsTerminal.exe and OpenConsole.exe ancestors",
                          error)) {
        return false;
    }

    Config config;
    config.wait_patterns = {"need your approval"};
    config.recoverable_error_patterns = {"service unavailable"};
    config.attach_visible_text_patterns = {"codex", "thinking"};

    if (!expect_self_test(attach_snapshot_confirmation_reason("Need your approval before continuing", config) ==
                              "wait pattern: need your approval",
                          "expected wait-pattern confirmation",
                          error)) {
        return false;
    }
    if (!expect_self_test(attach_snapshot_confirmation_reason("Service unavailable, retry later", config) ==
                              "recoverable_error pattern: service unavailable",
                          "expected recoverable-error confirmation",
                          error)) {
        return false;
    }
    if (!expect_self_test(attach_snapshot_confirmation_reason("Codex is thinking about the patch", config) ==
                              "visible-text hint: codex",
                          "expected visible-text confirmation",
                          error)) {
        return false;
    }
    if (!expect_self_test(attach_snapshot_confirmation_reason("PS D:\\Repos\\LoopCode>", config).empty(),
                          "plain shell prompt should not be confirmed as the target agent",
                          error)) {
        return false;
    }
    if (!expect_self_test(resume_command_for_agent("codex.exe") == "codex resume --last",
                          "codex attached resume command should use codex resume --last",
                          error)) {
        return false;
    }
    if (!expect_self_test(resume_command_for_agent("claude.exe") == "claude --continue",
                          "claude attached resume command should use claude --continue",
                          error)) {
        return false;
    }

    const auto temp_root = fs::temp_directory_path() / ("loopguard-selftest-" + compact_timestamp());
    fs::create_directories(temp_root);

    Config attach_config;
    attach_config.mode = "attach";
    attach_config.session_enabled = true;
    attach_config.session_name = "attach-selftest";
    attach_config.session_storage_dir = path_to_utf8(temp_root / "sessions");
    attach_config.log_dir = path_to_utf8(temp_root / "logs");

    try {
        Logger logger(path_from_utf8(attach_config.log_dir) / "selftest.log");
        save_attach_inventory(attach_config,
                              {AttachInventoryEntry{"codex", "D:\\Repos\\LoopCode"},
                               AttachInventoryEntry{"claude", "D:\\Repos\\AnotherRepo"}},
                              logger,
                              "self-test");

        const auto loaded = load_attach_inventory(attach_config);
        if (!expect_self_test(loaded.size() == 2, "attach inventory roundtrip should keep both entries", error)) {
            fs::remove_all(temp_root);
            return false;
        }
        if (!expect_self_test(loaded[0].agent_name == "codex" && loaded[0].workdir == "D:\\Repos\\LoopCode",
                              "attach inventory should preserve the first entry",
                              error)) {
            fs::remove_all(temp_root);
            return false;
        }
    } catch (const std::exception& ex) {
        error = ex.what();
        fs::remove_all(temp_root);
        return false;
    }

    fs::remove_all(temp_root);

    return true;
}

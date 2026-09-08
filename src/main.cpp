#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kClassName[] = L"LittleTipsWindow";
constexpr wchar_t kAppName[] = L"LittleTips";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT_PTR kSaveTimer = 1;
constexpr UINT kTrayId = 1;
constexpr int kMenuEdit = 1001;
constexpr int kMenuTopmost = 1002;
constexpr int kMenuStartup = 1003;
constexpr int kMenuShow = 1004;
constexpr int kMenuExit = 1005;

HWND g_window = nullptr;
HWND g_editor = nullptr;
HFONT g_font = nullptr;
HBRUSH g_noteBrush = nullptr;
bool g_editing = false;
bool g_topmost = true;
bool g_exiting = false;
bool g_loading = false;
UINT g_taskbarCreated = 0;
std::filesystem::path g_dataDirectory;
std::filesystem::path g_notePath;
std::filesystem::path g_configPath;

std::wstring GetExecutablePath() {
    std::wstring path(260, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) return {};
        if (length < path.size() - 1) {
            path.resize(length);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

void InitializePaths() {
    wchar_t buffer[MAX_PATH]{};
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        GetTempPathW(MAX_PATH, buffer);
    }
    g_dataDirectory = std::filesystem::path(buffer) / kAppName;
    std::error_code error;
    std::filesystem::create_directories(g_dataDirectory, error);
    g_notePath = g_dataDirectory / L"note.txt";
    g_configPath = g_dataDirectory / L"settings.ini";
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

void SaveNote() {
    if (!g_editor || g_loading) return;
    const int length = GetWindowTextLengthW(g_editor);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(g_editor, text.data(), length + 1);
    text.resize(length);
    std::ofstream file(g_notePath, std::ios::binary | std::ios::trunc);
    const std::string utf8 = WideToUtf8(text);
    file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
}

void LoadNote() {
    std::ifstream file(g_notePath, std::ios::binary);
    if (!file) {
        SetWindowTextW(g_editor, L"双击托盘图标显示便签。\r\n右键打开菜单，切换编辑模式。");
        return;
    }
    const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::wstring text = Utf8ToWide(bytes);
    g_loading = true;
    SetWindowTextW(g_editor, text.c_str());
    g_loading = false;
}

int ReadSetting(const wchar_t* key, int fallback) {
    return GetPrivateProfileIntW(L"window", key, fallback, g_configPath.c_str());
}

void WriteSetting(const wchar_t* key, int value) {
    wchar_t text[32]{};
    wsprintfW(text, L"%d", value);
    WritePrivateProfileStringW(L"window", key, text, g_configPath.c_str());
}

bool IsStartupEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    wchar_t value[32768]{};
    DWORD type = 0;
    DWORD bytes = sizeof(value);
    const LSTATUS status = RegQueryValueExW(key, kAppName, nullptr, &type, reinterpret_cast<BYTE*>(value), &bytes);
    RegCloseKey(key);
    const std::wstring expected = L"\"" + GetExecutablePath() + L"\"";
    return status == ERROR_SUCCESS && type == REG_SZ && expected == value;
}

bool SetStartupEnabled(bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    LSTATUS status = ERROR_SUCCESS;
    if (enabled) {
        const std::wstring value = L"\"" + GetExecutablePath() + L"\"";
        status = RegSetValueExW(key, kAppName, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, kAppName);
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

void AddTrayIcon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = g_window;
    data.uID = kTrayId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = LoadIconW(nullptr, IDI_INFORMATION);
    lstrcpynW(data.szTip, kAppName, ARRAYSIZE(data.szTip));
    Shell_NotifyIconW(NIM_ADD, &data);
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
}

void RemoveTrayIcon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = g_window;
    data.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &data);
}

void SetTopmost(bool topmost) {
    g_topmost = topmost;
    SetWindowPos(g_window, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    WriteSetting(L"topmost", topmost ? 1 : 0);
}

void SetEditing(bool editing) {
    g_editing = editing;
    SendMessageW(g_editor, EM_SETREADONLY, editing ? FALSE : TRUE, 0);
    if (editing) {
        ShowWindow(g_window, SW_SHOW);
        SetForegroundWindow(g_window);
        SetFocus(g_editor);
        SendMessageW(g_editor, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    } else {
        KillTimer(g_window, kSaveTimer);
        SaveNote();
        SetFocus(g_window);
    }
    InvalidateRect(g_editor, nullptr, TRUE);
}

void ShowContextMenu(POINT point) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (g_editing ? MF_CHECKED : 0), kMenuEdit, L"编辑模式\tCtrl+E");
    AppendMenuW(menu, MF_STRING | (g_topmost ? MF_CHECKED : 0), kMenuTopmost, L"始终置顶\tCtrl+T");
    AppendMenuW(menu, MF_STRING | (IsStartupEnabled() ? MF_CHECKED : 0), kMenuStartup, L"开机自启");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuShow, IsWindowVisible(g_window) ? L"隐藏便签" : L"显示便签");
    AppendMenuW(menu, MF_STRING, kMenuExit, L"退出");
    SetForegroundWindow(g_window);
    const int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, g_window, nullptr);
    DestroyMenu(menu);
    switch (command) {
    case kMenuEdit: SetEditing(!g_editing); break;
    case kMenuTopmost: SetTopmost(!g_topmost); break;
    case kMenuStartup:
        if (!SetStartupEnabled(!IsStartupEnabled())) MessageBoxW(g_window, L"无法修改开机自启设置。", kAppName, MB_OK | MB_ICONERROR);
        break;
    case kMenuShow:
        if (IsWindowVisible(g_window)) ShowWindow(g_window, SW_HIDE);
        else { ShowWindow(g_window, SW_SHOW); SetForegroundWindow(g_window); }
        break;
    case kMenuExit:
        g_exiting = true;
        DestroyWindow(g_window);
        break;
    }
}

void SaveWindowPlacement() {
    if (IsIconic(g_window)) return;
    RECT rect{};
    GetWindowRect(g_window, &rect);
    WriteSetting(L"x", rect.left);
    WriteSetting(L"y", rect.top);
    WriteSetting(L"width", rect.right - rect.left);
    WriteSetting(L"height", rect.bottom - rect.top);
}

LRESULT CALLBACK EditorSubclass(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR) {
    if (message == WM_RBUTTONUP) {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ClientToScreen(window, &point);
        ShowContextMenu(point);
        return 0;
    }
    if (!g_editing && message == WM_LBUTTONDOWN) {
        ReleaseCapture();
        SendMessageW(g_window, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return 0;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

void LayoutEditor() {
    RECT client{};
    GetClientRect(g_window, &client);
    const int margin = MulDiv(12, GetDpiForWindow(g_window), 96);
    MoveWindow(g_editor, margin, margin, std::max(0, static_cast<int>(client.right) - margin * 2),
        std::max(0, static_cast<int>(client.bottom) - margin * 2), TRUE);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == g_taskbarCreated) {
        AddTrayIcon();
        return 0;
    }
    switch (message) {
    case WM_CREATE: {
        g_editor = CreateWindowExW(0, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(1), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(g_editor, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(2, 2));
        SendMessageW(g_editor, EM_SETLIMITTEXT, 1024 * 1024, 0);
        SetWindowSubclass(g_editor, EditorSubclass, 1, 0);
        const int fontSize = -MulDiv(15, GetDpiForWindow(window), 72);
        g_font = CreateFontW(fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        SendMessageW(g_editor, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        LoadNote();
        SendMessageW(g_editor, EM_SETREADONLY, TRUE, 0);
        return 0;
    }
    case WM_SIZE:
        LayoutEditor();
        return 0;
    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
            suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_font) DeleteObject(g_font);
        const int fontSize = -MulDiv(15, HIWORD(wparam), 72);
        g_font = CreateFontW(fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        SendMessageW(g_editor, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        LayoutEditor();
        return 0;
    }
    case WM_COMMAND:
        if (reinterpret_cast<HWND>(lparam) == g_editor && HIWORD(wparam) == EN_CHANGE && !g_loading) {
            SetTimer(window, kSaveTimer, 700, nullptr);
        }
        return 0;
    case WM_TIMER:
        if (wparam == kSaveTimer) {
            KillTimer(window, kSaveTimer);
            SaveNote();
        }
        return 0;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, RGB(45, 43, 35));
        SetBkColor(dc, RGB(255, 246, 178));
        return reinterpret_cast<LRESULT>(g_noteBrush);
    }
    case WM_CONTEXTMENU: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (point.x == -1 && point.y == -1) GetCursorPos(&point);
        ShowContextMenu(point);
        return 0;
    }
    case WM_HOTKEY:
        return 0;
    case WM_KEYDOWN:
        if (GetKeyState(VK_CONTROL) < 0 && wparam == 'E') { SetEditing(!g_editing); return 0; }
        if (GetKeyState(VK_CONTROL) < 0 && wparam == 'T') { SetTopmost(!g_topmost); return 0; }
        break;
    case kTrayMessage:
        if (LOWORD(lparam) == WM_CONTEXTMENU) {
            POINT point{};
            GetCursorPos(&point);
            ShowContextMenu(point);
        } else if (LOWORD(lparam) == WM_LBUTTONDBLCLK) {
            ShowWindow(window, SW_SHOW);
            SetForegroundWindow(window);
        }
        return 0;
    case WM_CLOSE:
        if (!g_exiting) {
            SetEditing(false);
            ShowWindow(window, SW_HIDE);
            return 0;
        }
        break;
    case WM_DESTROY:
        KillTimer(window, kSaveTimer);
        SaveNote();
        SaveWindowPlacement();
        RemoveTrayIcon();
        if (g_font) DeleteObject(g_font);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

RECT InitialWindowRect() {
    const int width = std::clamp(ReadSetting(L"width", 340), 220, 1600);
    const int height = std::clamp(ReadSetting(L"height", 260), 140, 1200);
    const int fallbackX = GetSystemMetrics(SM_CXSCREEN) - width - 36;
    const int fallbackY = 60;
    RECT rect{ReadSetting(L"x", fallbackX), ReadSetting(L"y", fallbackY), 0, 0};
    rect.right = rect.left + width;
    rect.bottom = rect.top + height;
    HMONITOR monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    rect.left = std::clamp(rect.left, info.rcWork.left, std::max(info.rcWork.left, info.rcWork.right - width));
    rect.top = std::clamp(rect.top, info.rcWork.top, std::max(info.rcWork.top, info.rcWork.bottom - height));
    rect.right = rect.left + width;
    rect.bottom = rect.top + height;
    return rect;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\LittleTips.SingleInstance");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kClassName, nullptr)) {
            ShowWindow(existing, SW_SHOW);
            SetForegroundWindow(existing);
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    InitializePaths();
    InitCommonControls();
    g_noteBrush = CreateSolidBrush(RGB(255, 246, 178));
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_INFORMATION);
    windowClass.hbrBackground = g_noteBrush;
    windowClass.lpszClassName = kClassName;
    RegisterClassExW(&windowClass);

    const RECT rect = InitialWindowRect();
    g_topmost = ReadSetting(L"topmost", 1) != 0;
    g_window = CreateWindowExW(g_topmost ? WS_EX_TOPMOST : 0, kClassName, kAppName,
        WS_POPUP | WS_THICKFRAME, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, instance, nullptr);
    if (!g_window) {
        DeleteObject(g_noteBrush);
        CloseHandle(mutex);
        return 1;
    }

    // WM_CREATE runs before CreateWindowExW returns, so initialize operations
    // that need the final HWND only after the handle has been assigned.
    AddTrayIcon();

    ShowWindow(g_window, showCommand == SW_HIDE ? SW_HIDE : SW_SHOW);
    UpdateWindow(g_window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (GetKeyState(VK_CONTROL) < 0 && message.message == WM_KEYDOWN && message.wParam == 'E') {
            SetEditing(!g_editing);
            continue;
        }
        if (GetKeyState(VK_CONTROL) < 0 && message.message == WM_KEYDOWN && message.wParam == 'T') {
            SetTopmost(!g_topmost);
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    DeleteObject(g_noteBrush);
    CloseHandle(mutex);
    return static_cast<int>(message.wParam);
}

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
// MSVC's gdiplus.h needs the COM basics (STDMETHOD, IStream) that
// WIN32_LEAN_AND_MEAN removes from windows.h; objbase.h puts them back.
#include <objbase.h>
#include <gdiplus.h>
#include <mmsystem.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kClassName[] = L"LittleTipsWindow";
constexpr wchar_t kAppName[] = L"LittleTips";
constexpr COLORREF kNoteColor = RGB(255, 246, 178);
constexpr COLORREF kBorderColor = RGB(198, 178, 94);
constexpr COLORREF kTextColor = RGB(45, 43, 35);
constexpr float kBorderWidth = 4.0f;
constexpr int kBorderSteps = 12;
constexpr float kNoteCornerRadius = 8.0f;
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT_PTR kSaveTimer = 1;
constexpr UINT_PTR kPhysicsTimer = 2;
constexpr UINT_PTR kPlacementSaveTimer = 3;
constexpr UINT kPlacementSaveDelayMs = 500;
constexpr UINT kPhysicsActiveIntervalMs = 16;
constexpr UINT kPhysicsIdleIntervalMs = 120;
constexpr UINT kTrayId = 1;
constexpr WORD kTipsIcon = 101;
constexpr int kMenuEdit = 1001;
constexpr int kMenuStartup = 1003;
constexpr int kMenuExit = 1005;
// DWMWINDOWATTRIBUTE values newer than some SDK headers; keep the app buildable
// on older toolchains and on systems that simply reject them.
constexpr DWORD kDwmCornerPreferenceAttribute = 33;
constexpr DWORD kDwmBorderColorAttribute = 34;
constexpr int kDwmCornerRound = 2;

HWND g_window = nullptr;
HWND g_editor = nullptr;
HFONT g_font = nullptr;
HBRUSH g_noteBrush = nullptr;
ULONG_PTR g_gdiplusToken = 0;
bool g_gdiplusReady = false;
bool g_editing = false;
bool g_loading = false;
bool g_inSizeMove = false;
bool g_physicsPositionValid = false;
double g_physicsX = 0.0;
double g_physicsY = 0.0;
double g_velocityX = 0.0;
double g_velocityY = 0.0;
LARGE_INTEGER g_lastPhysicsTick{};
LARGE_INTEGER g_performanceFrequency{};
UINT g_taskbarCreated = 0;
std::filesystem::path g_dataDirectory;
std::filesystem::path g_notePath;
std::filesystem::path g_configPath;

void SaveWindowPlacement();
void ResizeWindowToFitText(HWND window);

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
        SetWindowTextW(g_editor, L"点击便签任意位置可拖动。\r\n右键打开菜单，Ctrl+E 进入编辑。");
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

// One line per launch; the first thing to look at when autostart misbehaves.
void LogStartup(const wchar_t* event) {
    if (g_dataDirectory.empty()) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t stamp[32]{};
    wsprintfW(stamp, L"%04u-%02u-%02u %02u:%02u:%02u", now.wYear, now.wMonth, now.wDay, now.wHour,
        now.wMinute, now.wSecond);
    const std::wstring line = std::wstring(stamp) + L"  " + event + L"  " + GetExecutablePath() + L"\r\n";

    const std::filesystem::path path = g_dataDirectory / L"startup.log";
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    const bool rotate = !error && size > 64 * 1024;
    std::ofstream file(path, std::ios::binary | (rotate ? std::ios::trunc : std::ios::app));
    const std::string utf8 = WideToUtf8(line);
    file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
}

void AddTrayIcon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = g_window;
    data.uID = kTrayId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kTipsIcon));
    if (!data.hIcon) data.hIcon = LoadIconW(nullptr, IDI_INFORMATION);
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

void ResetPhysicsMotion() {
    g_velocityX = 0.0;
    g_velocityY = 0.0;
    g_physicsPositionValid = false;
    QueryPerformanceCounter(&g_lastPhysicsTick);
}

void UpdatePhysicsTimer() {
    if (!g_window) return;
    const bool shouldRun = IsWindowVisible(g_window) && !g_editing && !g_inSizeMove;
    if (shouldRun) {
        QueryPerformanceCounter(&g_lastPhysicsTick);
        SetTimer(g_window, kPhysicsTimer, kPhysicsIdleIntervalMs, nullptr);
    } else {
        KillTimer(g_window, kPhysicsTimer);
        ResetPhysicsMotion();
    }
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
    UpdatePhysicsTimer();
    InvalidateRect(g_editor, nullptr, TRUE);
}

void ShowContextMenu(POINT point) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (g_editing ? MF_CHECKED : 0), kMenuEdit, L"编辑模式\tCtrl+E");
    AppendMenuW(menu, MF_STRING | (IsStartupEnabled() ? MF_CHECKED : 0), kMenuStartup, L"开机自启");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"退出");
    SetForegroundWindow(g_window);
    const int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, g_window, nullptr);
    DestroyMenu(menu);
    switch (command) {
    case kMenuEdit: SetEditing(!g_editing); break;
    case kMenuStartup:
        if (!SetStartupEnabled(!IsStartupEnabled())) MessageBoxW(g_window, L"无法修改开机自启设置。", kAppName, MB_OK | MB_ICONERROR);
        break;
    case kMenuExit:
        DestroyWindow(g_window);
        break;
    }
}

void StepPhysics() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const double elapsed = static_cast<double>(now.QuadPart - g_lastPhysicsTick.QuadPart) /
        static_cast<double>(g_performanceFrequency.QuadPart);
    g_lastPhysicsTick = now;
    const double dt = std::clamp(elapsed, 0.0, 0.033);
    if (dt <= 0.0) return;

    RECT rect{};
    GetWindowRect(g_window, &rect);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (!g_physicsPositionValid) {
        g_physicsX = static_cast<double>(rect.left);
        g_physicsY = static_cast<double>(rect.top);
        g_physicsPositionValid = true;
    }

    POINT cursor{};
    GetCursorPos(&cursor);
    const double centerX = g_physicsX + width * 0.5;
    const double centerY = g_physicsY + height * 0.5;
    double awayX = centerX - cursor.x;
    double awayY = centerY - cursor.y;
    double distance = std::hypot(awayX, awayY);
    if (distance < 0.001) {
        awayX = g_velocityX == 0.0 ? 1.0 : g_velocityX;
        awayY = g_velocityY;
        distance = std::hypot(awayX, awayY);
    }

    const double dpiScale = GetDpiForWindow(g_window) / 96.0;
    const double influenceRadius = 180.0 * dpiScale;
    const double falloffStart = std::hypot(static_cast<double>(width), static_cast<double>(height)) * 0.5;
    const bool inRange = distance < falloffStart + influenceRadius;
    if (inRange) {
        const double proximity = std::clamp((falloffStart + influenceRadius - distance) / influenceRadius, 0.0, 1.0);
        const double acceleration = 4800.0 * dpiScale * proximity * proximity;
        g_velocityX += awayX / distance * acceleration * dt;
        g_velocityY += awayY / distance * acceleration * dt;
    }

    // Once the cursor is outside an edge, keep a resting window attached to it
    // instead of letting the edge force pull it a few pixels inward.
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    GetMonitorInfoW(MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST), &monitorInfo);
    const RECT& bounds = monitorInfo.rcWork;
    if (g_physicsX <= bounds.left && cursor.x < bounds.left && g_velocityX > 0.0) g_velocityX = 0.0;
    if (g_physicsX + width >= bounds.right && cursor.x > bounds.right && g_velocityX < 0.0) g_velocityX = 0.0;
    if (g_physicsY <= bounds.top && cursor.y < bounds.top && g_velocityY > 0.0) g_velocityY = 0.0;
    if (g_physicsY + height >= bounds.bottom && cursor.y > bounds.bottom && g_velocityY < 0.0) g_velocityY = 0.0;

    const double damping = std::exp(-4.1 * dt);
    g_velocityX *= damping;
    g_velocityY *= damping;
    const double maxSpeed = 1050.0 * dpiScale;
    const double speed = std::hypot(g_velocityX, g_velocityY);
    if (speed > maxSpeed) {
        g_velocityX *= maxSpeed / speed;
        g_velocityY *= maxSpeed / speed;
    }
    const bool isIdle = !inRange && speed < 2.0;
    if (isIdle) {
        g_velocityX = 0.0;
        g_velocityY = 0.0;
    }
    SetTimer(g_window, kPhysicsTimer, isIdle ? kPhysicsIdleIntervalMs : kPhysicsActiveIntervalMs, nullptr);

    g_physicsX += g_velocityX * dt;
    g_physicsY += g_velocityY * dt;

    constexpr double restitution = 0.48;
    constexpr double wallFriction = 0.62;
    constexpr double edgeStickSpeed = 35.0;
    if (g_physicsX < bounds.left) {
        g_physicsX = static_cast<double>(bounds.left);
        if (g_velocityX < 0.0) g_velocityX = -g_velocityX * restitution;
        g_velocityY *= wallFriction;
        if (std::abs(g_velocityX) < edgeStickSpeed) g_velocityX = 0.0;
    } else if (g_physicsX + width > bounds.right) {
        g_physicsX = static_cast<double>(bounds.right - width);
        if (g_velocityX > 0.0) g_velocityX = -g_velocityX * restitution;
        g_velocityY *= wallFriction;
        if (std::abs(g_velocityX) < edgeStickSpeed) g_velocityX = 0.0;
    }
    if (g_physicsY < bounds.top) {
        g_physicsY = static_cast<double>(bounds.top);
        if (g_velocityY < 0.0) g_velocityY = -g_velocityY * restitution;
        g_velocityX *= wallFriction;
        if (std::abs(g_velocityY) < edgeStickSpeed) g_velocityY = 0.0;
    } else if (g_physicsY + height > bounds.bottom) {
        g_physicsY = static_cast<double>(bounds.bottom - height);
        if (g_velocityY > 0.0) g_velocityY = -g_velocityY * restitution;
        g_velocityX *= wallFriction;
        if (std::abs(g_velocityY) < edgeStickSpeed) g_velocityY = 0.0;
    }

    SetWindowPos(g_window, nullptr, static_cast<int>(std::lround(g_physicsX)),
        static_cast<int>(std::lround(g_physicsY)), 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
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
    if (!g_editing && message == WM_SETCURSOR) {
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;
    }
    if (!g_editing && (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK)) {
        ReleaseCapture();
        if (HWND host = GetParent(window)) {
            SendMessageW(host, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
        return 0;
    }
    if (message == WM_RBUTTONUP) {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ClientToScreen(window, &point);
        ShowContextMenu(point);
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

void ResizeWindowToFitText(HWND window) {
    if (!window || !g_editor) return;

    const int textLength = GetWindowTextLengthW(g_editor);
    std::wstring text(static_cast<size_t>(textLength) + 1, L'\0');
    GetWindowTextW(g_editor, text.data(), textLength + 1);
    text.resize(textLength);

    HDC dc = GetDC(g_editor);
    if (!dc) return;
    HFONT editorFont = reinterpret_cast<HFONT>(SendMessageW(g_editor, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = editorFont ? SelectObject(dc, editorFont) : nullptr;
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);

    int widestLine = 0;
    int lineCount = 1;
    size_t lineStart = 0;
    for (size_t index = 0; index <= text.size(); ++index) {
        const bool atEnd = index == text.size();
        const bool atBreak = !atEnd && (text[index] == L'\r' || text[index] == L'\n');
        if (!atEnd && !atBreak) continue;

        const int lineLength = static_cast<int>(index - lineStart);
        if (lineLength > 0) {
            const DWORD extent = GetTabbedTextExtentW(dc, text.data() + lineStart, lineLength, 0, nullptr);
            widestLine = std::max(widestLine, static_cast<int>(LOWORD(extent)));
        }
        if (atEnd) break;
        if (text[index] == L'\r' && index + 1 < text.size() && text[index + 1] == L'\n') ++index;
        lineStart = index + 1;
        ++lineCount;
    }

    if (oldFont) SelectObject(dc, oldFont);
    ReleaseDC(g_editor, dc);

    const UINT dpi = GetDpiForWindow(window);
    const int outerMargin = MulDiv(12, dpi, 96);
    const DWORD editMargins = static_cast<DWORD>(SendMessageW(g_editor, EM_GETMARGINS, 0, 0));
    const int horizontalEditMargins = LOWORD(editMargins) + HIWORD(editMargins);
    const int caretRoom = std::max(2, static_cast<int>(metrics.tmAveCharWidth));
    const int minimumWidth = MulDiv(96, dpi, 96);
    int desiredWidth = std::max(minimumWidth,
        widestLine + caretRoom + horizontalEditMargins + outerMargin * 2);
    int desiredHeight = lineCount * static_cast<int>(metrics.tmHeight) + outerMargin * 2;

    RECT current{};
    GetWindowRect(window, &current);
    HMONITOR monitor = MonitorFromRect(&current, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    GetMonitorInfoW(monitor, &monitorInfo);
    const RECT& workArea = monitorInfo.rcWork;
    desiredWidth = std::min(desiredWidth, static_cast<int>(workArea.right - workArea.left));
    desiredHeight = std::min(desiredHeight, static_cast<int>(workArea.bottom - workArea.top));
    const int x = std::clamp(current.left, workArea.left, workArea.right - desiredWidth);
    const int y = std::clamp(current.top, workArea.top, workArea.bottom - desiredHeight);

    if (current.right - current.left == desiredWidth && current.bottom - current.top == desiredHeight &&
        current.left == x && current.top == y) return;
    ResetPhysicsMotion();
    SetWindowPos(window, nullptr, x, y, desiredWidth, desiredHeight,
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void ApplyCustomFrame(HWND window) {
    const int cornerPreference = kDwmCornerRound;
    DwmSetWindowAttribute(window, static_cast<DWMWINDOWATTRIBUTE>(kDwmCornerPreferenceAttribute),
        &cornerPreference, sizeof(cornerPreference));
    // The border is painted below as well; this hairline keeps the outermost
    // pixels solid where the antialiased stroke fades out.
    const COLORREF borderColor = kBorderColor;
    DwmSetWindowAttribute(window, static_cast<DWMWINDOWATTRIBUTE>(kDwmBorderColorAttribute),
        &borderColor, sizeof(borderColor));
}

COLORREF BlendColor(COLORREF from, COLORREF to, float amount) {
    const auto mix = [amount](BYTE a, BYTE b) {
        return static_cast<BYTE>(std::lround(a + (b - a) * amount));
    };
    return RGB(mix(GetRValue(from), GetRValue(to)), mix(GetGValue(from), GetGValue(to)),
        mix(GetBValue(from), GetBValue(to)));
}

void StrokeRoundedBand(Gdiplus::Graphics& graphics, float width, float height, float scale, COLORREF color,
    float centerLine, float thickness) {
    // The window corners are rounded by DWM, so each step is centred on its own
    // centre line and its outer half follows the same rounded clip.
    const float radius = std::max(1.0f, kNoteCornerRadius * scale - centerLine);
    const float inset = centerLine;
    const float diameter = radius * 2.0f;
    const Gdiplus::RectF bounds(inset, inset, width - inset * 2.0f, height - inset * 2.0f);

    Gdiplus::GraphicsPath path;
    path.AddArc(bounds.X, bounds.Y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(bounds.GetRight() - diameter, bounds.Y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(bounds.GetRight() - diameter, bounds.GetBottom() - diameter, diameter, diameter, 0.0f, 90.0f);
    path.AddArc(bounds.X, bounds.GetBottom() - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();

    Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)), thickness);
    graphics.DrawPath(&pen, &path);
}

void PaintNoteBorder(HWND window, HDC dc) {
    if (!g_gdiplusReady) return;
    RECT client{};
    if (!GetClientRect(window, &client)) return;
    const float scale = static_cast<float>(GetDpiForWindow(window)) / 96.0f;
    const float width = static_cast<float>(client.right - client.left);
    const float height = static_cast<float>(client.bottom - client.top);
    const float thickness = kBorderWidth * scale;
    if (width <= thickness * 2.0f || height <= thickness * 2.0f) return;

    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    // A short monotonic ramp from a darker edge tone to the note colour: the
    // edge gives a crisp outline, the fade reads as the rounded edge curving
    // into shadow. Thin concentric strokes are antialiased into a smooth ramp.
    const float step = thickness / kBorderSteps;
    for (int index = 0; index < kBorderSteps; ++index) {
        const float progress = (index + 0.5f) / static_cast<float>(kBorderSteps);
        StrokeRoundedBand(graphics, width, height, scale, BlendColor(kBorderColor, kNoteColor, progress),
            (index + 0.5f) * step, step);
    }
}

// Resizing leaves the old border behind: the newly exposed frame strips get
// erased by the class brush but are never repainted. Invalidate just the ring
// between the window edge and the editor so only the frame is redrawn.
void InvalidateNoteFrame(HWND window) {
    RECT client{};
    if (!GetClientRect(window, &client)) return;
    if (client.right <= 0 || client.bottom <= 0) return;
    const int margin = MulDiv(12, static_cast<int>(GetDpiForWindow(window)), 96);
    HRGN outer = CreateRectRgn(0, 0, client.right, client.bottom);
    HRGN inner = CreateRectRgn(margin, margin, std::max(margin, static_cast<int>(client.right) - margin),
        std::max(margin, static_cast<int>(client.bottom) - margin));
    if (outer && inner) {
        CombineRgn(outer, outer, inner, RGN_DIFF);
        InvalidateRgn(window, outer, TRUE);
    }
    if (outer) DeleteObject(outer);
    if (inner) DeleteObject(inner);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == g_taskbarCreated) {
        AddTrayIcon();
        return 0;
    }
    switch (message) {
    case WM_NCCALCSIZE:
        // Drop the standard non-client frame entirely so the class brush paints
        // pale yellow edge to edge and the visible window matches GetWindowRect.
        if (wparam) return 0;
        break;
    case WM_NCACTIVATE:
        // Re-assert the custom frame; activation changes otherwise make the
        // default handler paint the classic frame inward over the client area.
        ApplyCustomFrame(window);
        return TRUE;
    case WM_NCPAINT:
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        PaintNoteBorder(window, dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_NCHITTEST: {
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        RECT rect{};
        GetWindowRect(window, &rect);
        if (PtInRect(&rect, point)) return g_editing ? HTCLIENT : HTCAPTION;
        break;
    }
    case WM_NCRBUTTONUP:
        if (!g_editing && wparam == HTCAPTION) {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ShowContextMenu(point);
            return 0;
        }
        break;
    case WM_CREATE: {
        g_editor = CreateWindowExW(0, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(1), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(g_editor, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(2, 2));
        SendMessageW(g_editor, EM_SETLIMITTEXT, 1024 * 1024, 0);
        SetWindowSubclass(g_editor, EditorSubclass, 1, 0);
        const int fontSize = -MulDiv(13, GetDpiForWindow(window), 72);
        g_font = CreateFontW(fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
        SendMessageW(g_editor, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        LoadNote();
        SendMessageW(g_editor, EM_SETREADONLY, TRUE, 0);
        ApplyCustomFrame(window);
        return 0;
    }
    case WM_SIZE:
        LayoutEditor();
        InvalidateNoteFrame(window);
        return 0;
    case WM_SHOWWINDOW:
        UpdatePhysicsTimer();
        return 0;
    case WM_ENTERSIZEMOVE:
        g_inSizeMove = true;
        UpdatePhysicsTimer();
        return 0;
    case WM_EXITSIZEMOVE:
        g_inSizeMove = false;
        ResetPhysicsMotion();
        UpdatePhysicsTimer();
        KillTimer(window, kPlacementSaveTimer);
        SaveWindowPlacement();
        return 0;
    case WM_WINDOWPOSCHANGED: {
        const WINDOWPOS* position = reinterpret_cast<WINDOWPOS*>(lparam);
        if (!(position->flags & SWP_NOMOVE) || !(position->flags & SWP_NOSIZE)) {
            SetTimer(window, kPlacementSaveTimer, kPlacementSaveDelayMs, nullptr);
        }
        break;
    }
    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
            suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_font) DeleteObject(g_font);
        const int fontSize = -MulDiv(13, HIWORD(wparam), 72);
        g_font = CreateFontW(fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
        SendMessageW(g_editor, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        LayoutEditor();
        ResizeWindowToFitText(window);
        return 0;
    }
    case WM_COMMAND:
        if (reinterpret_cast<HWND>(lparam) == g_editor && HIWORD(wparam) == EN_CHANGE && !g_loading) {
            SetTimer(window, kSaveTimer, 700, nullptr);
            ResizeWindowToFitText(window);
        }
        return 0;
    case WM_TIMER:
        if (wparam == kSaveTimer) {
            KillTimer(window, kSaveTimer);
            SaveNote();
        } else if (wparam == kPhysicsTimer) {
            StepPhysics();
        } else if (wparam == kPlacementSaveTimer) {
            KillTimer(window, kPlacementSaveTimer);
            SaveWindowPlacement();
        }
        return 0;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, kTextColor);
        SetBkColor(dc, kNoteColor);
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
        break;
    case kTrayMessage:
        if (LOWORD(lparam) == WM_CONTEXTMENU) {
            POINT point{};
            GetCursorPos(&point);
            ShowContextMenu(point);
        } else if (LOWORD(lparam) == WM_LBUTTONUP || LOWORD(lparam) == WM_LBUTTONDBLCLK) {
            ShowWindow(window, SW_SHOW);
            SetForegroundWindow(window);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kSaveTimer);
        KillTimer(window, kPhysicsTimer);
        KillTimer(window, kPlacementSaveTimer);
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
    InitializePaths();
    LogStartup(L"launch");
    // CreateMutexW only sets the last error when the mutex already exists, so
    // clear it first or a stale error would make every launch look like a copy.
    SetLastError(0);
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\LittleTips.SingleInstance");
    const DWORD mutexState = GetLastError();
    if (!mutex || mutexState == ERROR_ALREADY_EXISTS) {
        LogStartup(L"already running");
        if (HWND existing = FindWindowW(kClassName, nullptr)) {
            ShowWindow(existing, SW_SHOW);
            SetForegroundWindow(existing);
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    QueryPerformanceFrequency(&g_performanceFrequency);
    timeBeginPeriod(1);
    InitCommonControls();
    const Gdiplus::GdiplusStartupInput gdiplusInput;
    g_gdiplusReady = Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusInput, nullptr) == Gdiplus::Ok;
    g_noteBrush = CreateSolidBrush(kNoteColor);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(kTipsIcon));
    if (!windowClass.hIcon) windowClass.hIcon = LoadIconW(nullptr, IDI_INFORMATION);
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.hbrBackground = g_noteBrush;
    windowClass.lpszClassName = kClassName;
    RegisterClassExW(&windowClass);

    const RECT rect = InitialWindowRect();
    const DWORD extendedStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
    g_window = CreateWindowExW(extendedStyle, kClassName, kAppName,
        WS_POPUP, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, instance, nullptr);
    if (!g_window) {
        DeleteObject(g_noteBrush);
        if (g_gdiplusReady) Gdiplus::GdiplusShutdown(g_gdiplusToken);
        timeEndPeriod(1);
        CloseHandle(mutex);
        return 1;
    }

    // WM_CREATE runs before CreateWindowExW returns, so initialize operations
    // that need the final HWND only after the handle has been assigned.
    AddTrayIcon();
    LayoutEditor();
    ResizeWindowToFitText(g_window);

    ShowWindow(g_window, showCommand == SW_HIDE ? SW_HIDE : SW_SHOW);
    UpdateWindow(g_window);
    UpdatePhysicsTimer();

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (GetKeyState(VK_CONTROL) < 0 && message.message == WM_KEYDOWN && message.wParam == 'E') {
            SetEditing(!g_editing);
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    DeleteObject(g_noteBrush);
    if (g_gdiplusReady) Gdiplus::GdiplusShutdown(g_gdiplusToken);
    timeEndPeriod(1);
    CloseHandle(mutex);
    return static_cast<int>(message.wParam);
}

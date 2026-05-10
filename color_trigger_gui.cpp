/*
  ColorBot - simple color-trigger keyboard utility for Windows 10/11.

  Build on Windows:
    cl /std:c++20 /O2 /EHsc /DUNICODE /D_UNICODE color_trigger_gui.cpp user32.lib gdi32.lib comdlg32.lib /link /SUBSYSTEM:WINDOWS

  Build from macOS with mingw-w64:
    x86_64-w64-mingw32-g++ -std=c++20 -O2 -municode -mwindows -static -static-libgcc -static-libstdc++ color_trigger_gui.cpp -o colorbot.exe -luser32 -lgdi32 -lcomdlg32

  Use:
    1. Pick a color mode: Yellow, Red, Purple, or Custom.
    2. Pick the key to press.
    3. Pick how many times per second it may press.
    4. Click START.
    5. Press F8 to turn it on/off while in another program.
    6. Press F9 to stop immediately.

  This app watches the center of the screen only. It sends keyboard input, not mouse input.
*/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <mutex>
#include <random>
#include <string>
#include <thread>

enum : int {
    IDC_MODE = 1001,
    IDC_KEY,
    IDC_BOX,
    IDC_SPEED,
    IDC_RATE,
    IDC_SENSITIVITY,
    IDC_CUSTOM_R,
    IDC_CUSTOM_G,
    IDC_CUSTOM_B,
    IDC_PICK_COLOR,
    IDC_PREVIEW,
    IDC_START,
    IDC_STOP,
    IDC_STATUS
};

enum class ColorMode { Yellow = 0, Red = 1, Purple = 2, Custom = 3 };
enum class SpeedMode { Fast = 0, Balanced = 1, Human = 2 };
enum class RateMode { OnePerSecond = 0, TwoPerSecond = 1, FivePerSecond = 2, TenPerSecond = 3, Spam = 4 };
enum class SensitivityMode { Strict = 0, Normal = 1, Loose = 2 };

struct Config {
    ColorMode color_mode = ColorMode::Yellow;
    SpeedMode speed = SpeedMode::Balanced;
    RateMode rate = RateMode::TwoPerSecond;
    SensitivityMode sensitivity = SensitivityMode::Normal;
    WORD vk = 'F';
    int box_size = 40;
    int custom_r = 255;
    int custom_g = 230;
    int custom_b = 0;
    int tolerance = 35;
    int min_pixels = 3;
    int pre_min_ms = 15;
    int pre_max_ms = 55;
    int hold_min_ms = 35;
    int hold_max_ms = 90;
    int trigger_interval_ms = 500;
    int loop_min_ms = 0;
    int loop_max_ms = 2;
};

struct CaptureBuffer {
    HDC screen_dc = nullptr;
    HDC memory_dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ old_bitmap = nullptr;
    uint8_t* pixels = nullptr;
    int size = 0;

    bool init(int new_size) {
        destroy();
        size = new_size;

        screen_dc = GetDC(nullptr);
        if (!screen_dc) return false;

        memory_dc = CreateCompatibleDC(screen_dc);
        if (!memory_dc) return false;

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = size;
        bmi.bmiHeader.biHeight = -size;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        bitmap = CreateDIBSection(memory_dc, &bmi, DIB_RGB_COLORS,
                                  reinterpret_cast<void**>(&pixels), nullptr, 0);
        if (!bitmap || !pixels) return false;

        old_bitmap = SelectObject(memory_dc, bitmap);
        return old_bitmap != nullptr;
    }

    bool capture_center() {
        const int screen_w = GetSystemMetrics(SM_CXSCREEN);
        const int screen_h = GetSystemMetrics(SM_CYSCREEN);
        const int x = (screen_w - size) / 2;
        const int y = (screen_h - size) / 2;
        return BitBlt(memory_dc, 0, 0, size, size, screen_dc, x, y, SRCCOPY | CAPTUREBLT) != 0;
    }

    void destroy() {
        if (memory_dc && old_bitmap) SelectObject(memory_dc, old_bitmap);
        if (bitmap) DeleteObject(bitmap);
        if (memory_dc) DeleteDC(memory_dc);
        if (screen_dc) ReleaseDC(nullptr, screen_dc);
        screen_dc = nullptr;
        memory_dc = nullptr;
        bitmap = nullptr;
        old_bitmap = nullptr;
        pixels = nullptr;
        size = 0;
    }

    ~CaptureBuffer() { destroy(); }
};

static HWND g_main = nullptr;
static HWND g_mode = nullptr;
static HWND g_key = nullptr;
static HWND g_box = nullptr;
static HWND g_speed = nullptr;
static HWND g_rate = nullptr;
static HWND g_sensitivity = nullptr;
static HWND g_custom_r = nullptr;
static HWND g_custom_g = nullptr;
static HWND g_custom_b = nullptr;
static HWND g_pick_color = nullptr;
static HWND g_preview = nullptr;
static HWND g_status = nullptr;
static HBRUSH g_preview_brush = nullptr;

static std::mutex g_config_mutex;
static Config g_config;
static std::atomic<bool> g_running{false};
static std::thread g_worker;

static int clamp_int(int v, int lo, int hi) {
    return std::max(lo, std::min(v, hi));
}

static int rand_range(std::mt19937& rng, int lo, int hi) {
    if (hi <= lo) return lo;
    std::uniform_int_distribution<int> dist(lo, hi);
    return dist(rng);
}

static void set_text(HWND hwnd, const wchar_t* text) {
    SetWindowTextW(hwnd, text);
}

static void set_int(HWND hwnd, int value) {
    wchar_t buf[32]{};
    wsprintfW(buf, L"%d", value);
    SetWindowTextW(hwnd, buf);
}

static int get_int(HWND hwnd, int fallback, int lo, int hi) {
    wchar_t buf[64]{};
    GetWindowTextW(hwnd, buf, 64);
    wchar_t* end = nullptr;
    const long value = wcstol(buf, &end, 10);
    if (end == buf) return fallback;
    return clamp_int(static_cast<int>(value), lo, hi);
}

static std::wstring get_text(HWND hwnd) {
    wchar_t buf[128]{};
    GetWindowTextW(hwnd, buf, 128);
    return buf;
}

static WORD parse_key(HWND hwnd) {
    std::wstring s = get_text(hwnd);
    while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    if (s.empty()) return 'F';

    if (s.size() == 1) {
        const wchar_t c = towupper(s[0]);
        if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) return static_cast<WORD>(c);
    }

    std::wstring u;
    for (wchar_t c : s) u.push_back(static_cast<wchar_t>(towupper(c)));

    if (u == L"SPACE") return VK_SPACE;
    if (u == L"ENTER") return VK_RETURN;
    if (u == L"TAB") return VK_TAB;
    if (u == L"SHIFT") return VK_SHIFT;
    if (u == L"CTRL" || u == L"CONTROL") return VK_CONTROL;
    if (u == L"ALT") return VK_MENU;
    if (u == L"ESC" || u == L"ESCAPE") return VK_ESCAPE;
    if (u == L"UP") return VK_UP;
    if (u == L"DOWN") return VK_DOWN;
    if (u == L"LEFT") return VK_LEFT;
    if (u == L"RIGHT") return VK_RIGHT;

    if (u.size() >= 2 && u[0] == L'F') {
        const int n = _wtoi(u.c_str() + 1);
        if (n >= 1 && n <= 24) return static_cast<WORD>(VK_F1 + n - 1);
    }

    return 'F';
}

static int combo_index(HWND combo) {
    const LRESULT index = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    return index == CB_ERR ? 0 : static_cast<int>(index);
}

static void combo_add(HWND combo, const wchar_t* text) {
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
}

static void set_speed(Config& cfg) {
    switch (cfg.speed) {
    case SpeedMode::Fast:
        cfg.pre_min_ms = 0;
        cfg.pre_max_ms = 12;
        cfg.hold_min_ms = 25;
        cfg.hold_max_ms = 55;
        cfg.loop_min_ms = 0;
        cfg.loop_max_ms = 1;
        break;
    case SpeedMode::Balanced:
        cfg.pre_min_ms = 15;
        cfg.pre_max_ms = 55;
        cfg.hold_min_ms = 35;
        cfg.hold_max_ms = 90;
        cfg.loop_min_ms = 0;
        cfg.loop_max_ms = 2;
        break;
    case SpeedMode::Human:
        cfg.pre_min_ms = 35;
        cfg.pre_max_ms = 110;
        cfg.hold_min_ms = 50;
        cfg.hold_max_ms = 150;
        cfg.loop_min_ms = 1;
        cfg.loop_max_ms = 4;
        break;
    }
}

static void set_rate(Config& cfg) {
    switch (cfg.rate) {
    case RateMode::OnePerSecond: cfg.trigger_interval_ms = 1000; break;
    case RateMode::TwoPerSecond: cfg.trigger_interval_ms = 500; break;
    case RateMode::FivePerSecond: cfg.trigger_interval_ms = 200; break;
    case RateMode::TenPerSecond: cfg.trigger_interval_ms = 100; break;
    case RateMode::Spam: cfg.trigger_interval_ms = 25; break;
    }
}

static void set_sensitivity(Config& cfg) {
    switch (cfg.sensitivity) {
    case SensitivityMode::Strict:
        cfg.tolerance = 22;
        cfg.min_pixels = 6;
        break;
    case SensitivityMode::Normal:
        cfg.tolerance = 35;
        cfg.min_pixels = 3;
        break;
    case SensitivityMode::Loose:
        cfg.tolerance = 55;
        cfg.min_pixels = 2;
        break;
    }
}

static Config read_config_from_ui() {
    Config cfg;
    cfg.color_mode = static_cast<ColorMode>(combo_index(g_mode));
    cfg.speed = static_cast<SpeedMode>(combo_index(g_speed));
    cfg.rate = static_cast<RateMode>(combo_index(g_rate));
    cfg.sensitivity = static_cast<SensitivityMode>(combo_index(g_sensitivity));
    cfg.vk = parse_key(g_key);
    cfg.custom_r = get_int(g_custom_r, 255, 0, 255);
    cfg.custom_g = get_int(g_custom_g, 230, 0, 255);
    cfg.custom_b = get_int(g_custom_b, 0, 0, 255);

    switch (combo_index(g_box)) {
    case 0: cfg.box_size = 30; break;
    case 1: cfg.box_size = 40; break;
    case 2: cfg.box_size = 60; break;
    default: cfg.box_size = 80; break;
    }

    set_speed(cfg);
    set_rate(cfg);
    set_sensitivity(cfg);
    return cfg;
}

static Config current_config_copy() {
    std::lock_guard<std::mutex> lock(g_config_mutex);
    return g_config;
}

static bool close_to(uint8_t value, int target, int tolerance) {
    return std::abs(static_cast<int>(value) - target) <= tolerance;
}

static bool pixel_matches(uint8_t r, uint8_t g, uint8_t b, const Config& cfg) {
    switch (cfg.color_mode) {
    case ColorMode::Yellow:
        return r >= 145 && g >= 135 && b <= 115 &&
               std::abs(static_cast<int>(r) - static_cast<int>(g)) <= 110;

    case ColorMode::Red:
        return r >= 150 && g <= 115 && b <= 115 &&
               r >= g + 45 && r >= b + 45;

    case ColorMode::Purple:
        return r >= 120 && b >= 120 && g <= 135 &&
               std::abs(static_cast<int>(r) - static_cast<int>(b)) <= 100;

    case ColorMode::Custom:
        return close_to(r, cfg.custom_r, cfg.tolerance) &&
               close_to(g, cfg.custom_g, cfg.tolerance) &&
               close_to(b, cfg.custom_b, cfg.tolerance);
    }

    return false;
}

static bool contains_target_color(const uint8_t* pixels, int size, const Config& cfg) {
    const int count = size * size;
    int matches = 0;

    for (int i = 0; i < count; ++i) {
        const uint8_t b = pixels[i * 4 + 0];
        const uint8_t g = pixels[i * 4 + 1];
        const uint8_t r = pixels[i * 4 + 2];
        if (pixel_matches(r, g, b, cfg) && ++matches >= cfg.min_pixels) return true;
    }

    return false;
}

static void send_key(WORD vk, const Config& cfg, std::mt19937& rng) {
    std::this_thread::sleep_for(std::chrono::milliseconds(rand_range(rng, cfg.pre_min_ms, cfg.pre_max_ms)));

    INPUT down{};
    down.type = INPUT_KEYBOARD;
    down.ki.wVk = vk;
    SendInput(1, &down, sizeof(INPUT));

    std::this_thread::sleep_for(std::chrono::milliseconds(rand_range(rng, cfg.hold_min_ms, cfg.hold_max_ms)));

    INPUT up{};
    up.type = INPUT_KEYBOARD;
    up.ki.wVk = vk;
    up.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &up, sizeof(INPUT));
}

static void worker_loop() {
    std::random_device rd;
    std::mt19937 rng(rd());
    CaptureBuffer capture;
    int last_size = 0;

    using clock = std::chrono::steady_clock;
    auto next_allowed = clock::now();

    while (g_running.load(std::memory_order_relaxed)) {
        const Config cfg = current_config_copy();

        if (cfg.box_size != last_size || !capture.pixels) {
            if (!capture.init(cfg.box_size)) {
                g_running.store(false);
                PostMessageW(g_main, WM_APP + 1, 0, 0);
                return;
            }
            last_size = cfg.box_size;
        }

        if (capture.capture_center() && contains_target_color(capture.pixels, cfg.box_size, cfg)) {
            const auto now = clock::now();
            if (now >= next_allowed) {
                send_key(cfg.vk, cfg, rng);
                next_allowed = clock::now() + std::chrono::milliseconds(cfg.trigger_interval_ms);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(rand_range(rng, cfg.loop_min_ms, cfg.loop_max_ms)));
    }
}

static void refresh_custom_controls() {
    const bool custom = static_cast<ColorMode>(combo_index(g_mode)) == ColorMode::Custom;
    EnableWindow(g_custom_r, custom ? TRUE : FALSE);
    EnableWindow(g_custom_g, custom ? TRUE : FALSE);
    EnableWindow(g_custom_b, custom ? TRUE : FALSE);
    EnableWindow(g_pick_color, custom ? TRUE : FALSE);
    InvalidateRect(g_preview, nullptr, TRUE);
}

static void set_running_ui(bool running) {
    EnableWindow(GetDlgItem(g_main, IDC_START), running ? FALSE : TRUE);
    EnableWindow(GetDlgItem(g_main, IDC_STOP), running ? TRUE : FALSE);
    set_text(g_status, running ? L"ON - F8 toggles, F9 stops" : L"OFF - click START or press F8");
}

static void apply_config() {
    Config cfg = read_config_from_ui();
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        g_config = cfg;
    }
}

static void start_worker() {
    if (g_running.load()) return;
    apply_config();
    g_running.store(true);
    g_worker = std::thread(worker_loop);
    set_running_ui(true);
}

static void stop_worker() {
    g_running.store(false);
    if (g_worker.joinable()) g_worker.join();
    set_running_ui(false);
}

static void toggle_worker() {
    if (g_running.load()) stop_worker();
    else start_worker();
}

static HWND add_label(HWND parent, const wchar_t* text, int x, int y, int w, int h) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                           x, y, w, h, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
}

static HWND add_edit(HWND parent, int id, int x, int y, int w, int h, const wchar_t* text) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                           x, y, w, h, parent, reinterpret_cast<HMENU>(id), GetModuleHandleW(nullptr), nullptr);
}

static HWND add_combo(HWND parent, int id, int x, int y, int w, int h) {
    return CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                           x, y, w, h, parent, reinterpret_cast<HMENU>(id), GetModuleHandleW(nullptr), nullptr);
}

static HWND add_button(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h) {
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                           x, y, w, h, parent, reinterpret_cast<HMENU>(id), GetModuleHandleW(nullptr), nullptr);
}

static void create_controls(HWND hwnd) {
    add_label(hwnd, L"ColorBot", 20, 16, 170, 28);
    g_status = add_label(hwnd, L"OFF - click START or press F8", 230, 18, 260, 24);

    add_label(hwnd, L"Color to watch", 24, 62, 120, 20);
    g_mode = add_combo(hwnd, IDC_MODE, 160, 58, 220, 200);
    combo_add(g_mode, L"Yellow outline");
    combo_add(g_mode, L"Red outline");
    combo_add(g_mode, L"Purple outline");
    combo_add(g_mode, L"Custom color");
    SendMessageW(g_mode, CB_SETCURSEL, 0, 0);

    g_preview = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                400, 58, 46, 26, hwnd, reinterpret_cast<HMENU>(IDC_PREVIEW),
                                GetModuleHandleW(nullptr), nullptr);

    add_label(hwnd, L"Key to press", 24, 108, 120, 20);
    g_key = add_edit(hwnd, IDC_KEY, 160, 104, 100, 26, L"F");
    add_label(hwnd, L"Examples: F, E, SPACE, SHIFT, CTRL", 280, 108, 240, 20);

    add_label(hwnd, L"Reaction speed", 24, 154, 120, 20);
    g_speed = add_combo(hwnd, IDC_SPEED, 160, 150, 220, 150);
    combo_add(g_speed, L"Fast");
    combo_add(g_speed, L"Balanced");
    combo_add(g_speed, L"More human");
    SendMessageW(g_speed, CB_SETCURSEL, 1, 0);

    add_label(hwnd, L"Press rate", 24, 200, 120, 20);
    g_rate = add_combo(hwnd, IDC_RATE, 160, 196, 220, 170);
    combo_add(g_rate, L"1 press / second");
    combo_add(g_rate, L"2 presses / second");
    combo_add(g_rate, L"5 presses / second");
    combo_add(g_rate, L"10 presses / second");
    combo_add(g_rate, L"Spam while visible");
    SendMessageW(g_rate, CB_SETCURSEL, 1, 0);

    add_label(hwnd, L"Detection", 24, 246, 120, 20);
    g_sensitivity = add_combo(hwnd, IDC_SENSITIVITY, 160, 242, 220, 150);
    combo_add(g_sensitivity, L"Strict");
    combo_add(g_sensitivity, L"Normal");
    combo_add(g_sensitivity, L"Loose");
    SendMessageW(g_sensitivity, CB_SETCURSEL, 1, 0);

    add_label(hwnd, L"Center box", 24, 292, 120, 20);
    g_box = add_combo(hwnd, IDC_BOX, 160, 288, 220, 150);
    combo_add(g_box, L"Tiny - 30 x 30");
    combo_add(g_box, L"Small - 40 x 40");
    combo_add(g_box, L"Medium - 60 x 60");
    combo_add(g_box, L"Large - 80 x 80");
    SendMessageW(g_box, CB_SETCURSEL, 1, 0);

    add_label(hwnd, L"Custom RGB", 24, 338, 120, 20);
    g_custom_r = add_edit(hwnd, IDC_CUSTOM_R, 160, 334, 54, 26, L"255");
    g_custom_g = add_edit(hwnd, IDC_CUSTOM_G, 222, 334, 54, 26, L"230");
    g_custom_b = add_edit(hwnd, IDC_CUSTOM_B, 284, 334, 54, 26, L"0");
    g_pick_color = add_button(hwnd, IDC_PICK_COLOR, L"Pick", 354, 333, 70, 28);

    add_button(hwnd, IDC_START, L"START", 110, 386, 145, 42);
    add_button(hwnd, IDC_STOP, L"STOP", 275, 386, 145, 42);
    EnableWindow(GetDlgItem(hwnd, IDC_STOP), FALSE);

    add_label(hwnd, L"Hotkeys: F8 start/stop   F9 stop", 140, 448, 260, 20);
    refresh_custom_controls();
}

static COLORREF preview_color() {
    switch (static_cast<ColorMode>(combo_index(g_mode))) {
    case ColorMode::Yellow: return RGB(255, 230, 0);
    case ColorMode::Red: return RGB(235, 40, 35);
    case ColorMode::Purple: return RGB(175, 75, 255);
    case ColorMode::Custom:
        return RGB(get_int(g_custom_r, 255, 0, 255),
                   get_int(g_custom_g, 230, 0, 255),
                   get_int(g_custom_b, 0, 0, 255));
    }
    return RGB(255, 230, 0);
}

static void choose_color(HWND hwnd) {
    COLORREF custom_colors[16]{};
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = hwnd;
    cc.rgbResult = preview_color();
    cc.lpCustColors = custom_colors;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;

    if (ChooseColorW(&cc)) {
        set_int(g_custom_r, GetRValue(cc.rgbResult));
        set_int(g_custom_g, GetGValue(cc.rgbResult));
        set_int(g_custom_b, GetBValue(cc.rgbResult));
        SendMessageW(g_mode, CB_SETCURSEL, static_cast<int>(ColorMode::Custom), 0);
        refresh_custom_controls();
    }
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CREATE:
        create_controls(hwnd);
        RegisterHotKey(hwnd, 1, 0, VK_F8);
        RegisterHotKey(hwnd, 2, 0, VK_F9);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_START:
            start_worker();
            return 0;
        case IDC_STOP:
            stop_worker();
            return 0;
        case IDC_PICK_COLOR:
            choose_color(hwnd);
            return 0;
        case IDC_MODE:
            if (HIWORD(wparam) == CBN_SELCHANGE) refresh_custom_controls();
            return 0;
        case IDC_CUSTOM_R:
        case IDC_CUSTOM_G:
        case IDC_CUSTOM_B:
            if (HIWORD(wparam) == EN_CHANGE) InvalidateRect(g_preview, nullptr, TRUE);
            return 0;
        }
        break;

    case WM_HOTKEY:
        if (wparam == 1) toggle_worker();
        if (wparam == 2) stop_worker();
        return 0;

    case WM_CTLCOLORSTATIC:
        if (reinterpret_cast<HWND>(lparam) == g_preview) {
            if (g_preview_brush) DeleteObject(g_preview_brush);
            g_preview_brush = CreateSolidBrush(preview_color());
            return reinterpret_cast<LRESULT>(g_preview_brush);
        }
        break;

    case WM_APP + 1:
        stop_worker();
        MessageBoxW(hwnd, L"Screen capture could not start.", L"ColorBot", MB_ICONERROR);
        return 0;

    case WM_CLOSE:
        stop_worker();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        stop_worker();
        UnregisterHotKey(hwnd, 1);
        UnregisterHotKey(hwnd, 2);
        if (g_preview_brush) DeleteObject(g_preview_brush);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_cmd) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const wchar_t class_name[] = L"ColorBotWindow";
    WNDCLASSW wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = class_name;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    if (!RegisterClassW(&wc)) return 1;

    g_main = CreateWindowExW(0, class_name, L"ColorBot",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 545, 525,
                             nullptr, nullptr, instance, nullptr);
    if (!g_main) return 1;

    ShowWindow(g_main, show_cmd);
    UpdateWindow(g_main);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return 0;
}

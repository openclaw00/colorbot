/*
  minhan-time - simple color-trigger keyboard utility for Windows 10/11.

  Build on Windows:
    cl /std:c++20 /O2 /EHsc /DUNICODE /D_UNICODE color_trigger_gui.cpp user32.lib gdi32.lib comdlg32.lib /link /SUBSYSTEM:WINDOWS

  Build from macOS with mingw-w64:
    x86_64-w64-mingw32-g++ -std=c++20 -O2 -municode -mwindows -static -static-libgcc -static-libstdc++ color_trigger_gui.cpp -o minhan-time.exe -luser32 -lgdi32 -lcomdlg32

  Use:
    1. Pick a color mode: Yellow, Red, Purple, or Custom.
    2. Pick the key to press.
    3. Pick tap mode or hold mode.
    4. Set reaction delay, press interval, and scan box size.
    4. Click START.
    5. Press F8 to turn it on/off while in another program.
    6. Press F9 to stop immediately.

  This app watches a box centered on the screen. It sends keyboard input, not mouse input.
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
#include <string>
#include <thread>

enum : int {
    IDC_MODE = 1001,
    IDC_KEY,
    IDC_ACTION,
    IDC_REACTION_MS,
    IDC_PRESS_INTERVAL_MS,
    IDC_BOX_W,
    IDC_BOX_H,
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
enum class ActionMode { TapRepeatedly = 0, HoldWhileVisible = 1 };
enum class SensitivityMode { Strict = 0, Normal = 1, Loose = 2 };

struct Config {
    ColorMode color_mode = ColorMode::Yellow;
    ActionMode action_mode = ActionMode::TapRepeatedly;
    SensitivityMode sensitivity = SensitivityMode::Normal;
    WORD vk = 'F';
    int box_w = 40;
    int box_h = 40;
    int custom_r = 255;
    int custom_g = 230;
    int custom_b = 0;
    int tolerance = 35;
    int min_pixels = 3;
    int reaction_ms = 0;
    int hold_ms = 20;
    int press_interval_ms = 25;
    int loop_sleep_ms = 0;
};

struct CaptureBuffer {
    HDC screen_dc = nullptr;
    HDC memory_dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ old_bitmap = nullptr;
    uint8_t* pixels = nullptr;
    int width = 0;
    int height = 0;

    bool init(int new_width, int new_height) {
        destroy();
        width = new_width;
        height = new_height;

        screen_dc = GetDC(nullptr);
        if (!screen_dc) return false;

        memory_dc = CreateCompatibleDC(screen_dc);
        if (!memory_dc) return false;

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height;
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
        const int x = (screen_w - width) / 2;
        const int y = (screen_h - height) / 2;
        return BitBlt(memory_dc, 0, 0, width, height, screen_dc, x, y, SRCCOPY | CAPTUREBLT) != 0;
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
        width = 0;
        height = 0;
    }

    ~CaptureBuffer() { destroy(); }
};

static HWND g_main = nullptr;
static HWND g_mode = nullptr;
static HWND g_key = nullptr;
static HWND g_action = nullptr;
static HWND g_reaction_ms = nullptr;
static HWND g_press_interval_ms = nullptr;
static HWND g_box_w = nullptr;
static HWND g_box_h = nullptr;
static HWND g_sensitivity = nullptr;
static HWND g_custom_r = nullptr;
static HWND g_custom_g = nullptr;
static HWND g_custom_b = nullptr;
static HWND g_pick_color = nullptr;
static HWND g_preview = nullptr;
static HWND g_status = nullptr;
static HBRUSH g_preview_brush = nullptr;
static HBRUSH g_bg_brush = nullptr;
static HBRUSH g_panel_brush = nullptr;
static HBRUSH g_input_brush = nullptr;
static HBRUSH g_label_brush = nullptr;
static HFONT g_font = nullptr;
static HFONT g_font_bold = nullptr;
static HFONT g_font_title = nullptr;
static HFONT g_font_small = nullptr;
static HICON g_app_icon = nullptr;

static constexpr COLORREF COLOR_BG = RGB(15, 16, 20);
static constexpr COLORREF COLOR_PANEL = RGB(24, 25, 31);
static constexpr COLORREF COLOR_INPUT = RGB(45, 46, 54);
static constexpr COLORREF COLOR_TEXT = RGB(238, 235, 244);
static constexpr COLORREF COLOR_MUTED = RGB(160, 154, 171);
static constexpr COLORREF COLOR_ACCENT = RGB(188, 80, 255);
static constexpr COLORREF COLOR_ACCENT_2 = RGB(64, 189, 239);
static constexpr COLORREF COLOR_STOP = RGB(62, 58, 68);

static std::mutex g_config_mutex;
static Config g_config;
static std::atomic<bool> g_running{false};
static std::thread g_worker;

static int clamp_int(int v, int lo, int hi) {
    return std::max(lo, std::min(v, hi));
}

static void set_text(HWND hwnd, const wchar_t* text) {
    SetWindowTextW(hwnd, text);
}

static void set_int(HWND hwnd, int value) {
    wchar_t buf[32]{};
    wsprintfW(buf, L"%d", value);
    SetWindowTextW(hwnd, buf);
}

static HFONT make_font(int size, int weight) {
    return CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

static void apply_font(HWND hwnd, HFONT font) {
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

static void init_theme() {
    g_bg_brush = CreateSolidBrush(COLOR_BG);
    g_panel_brush = CreateSolidBrush(COLOR_PANEL);
    g_input_brush = CreateSolidBrush(COLOR_INPUT);
    g_label_brush = CreateSolidBrush(COLOR_BG);
    g_font = make_font(15, FW_NORMAL);
    g_font_bold = make_font(15, FW_SEMIBOLD);
    g_font_title = make_font(24, FW_BOLD);
    g_font_small = make_font(13, FW_NORMAL);
}

static HICON create_app_icon() {
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = 32;
    bmi.bmiHeader.biHeight = -32;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    uint32_t* pixels = nullptr;
    HBITMAP color = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
    HGDIOBJ old = SelectObject(dc, color);

    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const int dx = x - 16;
            const int dy = y - 16;
            const bool inside = dx * dx + dy * dy <= 14 * 14;
            if (!inside) {
                pixels[y * 32 + x] = 0;
                continue;
            }
            const uint8_t r = static_cast<uint8_t>(120 + x * 4);
            const uint8_t g = static_cast<uint8_t>(46 + y * 3);
            const uint8_t b = static_cast<uint8_t>(210 + (31 - x) * 1);
            pixels[y * 32 + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }

    HPEN pen = CreatePen(PS_SOLID, 3, RGB(236, 220, 255));
    HGDIOBJ old_pen = SelectObject(dc, pen);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, 8, 8, 24, 24);
    MoveToEx(dc, 22, 22, nullptr);
    LineTo(dc, 28, 28);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(pen);

    SelectObject(dc, old);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);

    HBITMAP mask = CreateBitmap(32, 32, 1, 1, nullptr);
    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = color;
    info.hbmMask = mask;
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
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
    cfg.action_mode = static_cast<ActionMode>(combo_index(g_action));
    cfg.sensitivity = static_cast<SensitivityMode>(combo_index(g_sensitivity));
    cfg.vk = parse_key(g_key);
    cfg.reaction_ms = get_int(g_reaction_ms, 0, 0, 10000);
    cfg.press_interval_ms = get_int(g_press_interval_ms, 25, 0, 60000);
    cfg.box_w = get_int(g_box_w, 40, 1, 800);
    cfg.box_h = get_int(g_box_h, 40, 1, 800);
    cfg.custom_r = get_int(g_custom_r, 255, 0, 255);
    cfg.custom_g = get_int(g_custom_g, 230, 0, 255);
    cfg.custom_b = get_int(g_custom_b, 0, 0, 255);
    set_sensitivity(cfg);

    set_int(g_reaction_ms, cfg.reaction_ms);
    set_int(g_press_interval_ms, cfg.press_interval_ms);
    set_int(g_box_w, cfg.box_w);
    set_int(g_box_h, cfg.box_h);

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

static bool contains_target_color(const uint8_t* pixels, int width, int height, const Config& cfg) {
    const int count = width * height;
    int matches = 0;

    for (int i = 0; i < count; ++i) {
        const uint8_t b = pixels[i * 4 + 0];
        const uint8_t g = pixels[i * 4 + 1];
        const uint8_t r = pixels[i * 4 + 2];
        if (pixel_matches(r, g, b, cfg) && ++matches >= cfg.min_pixels) return true;
    }

    return false;
}

static void key_down(WORD vk) {
    INPUT down{};
    down.type = INPUT_KEYBOARD;
    down.ki.wVk = vk;
    SendInput(1, &down, sizeof(INPUT));
}

static void key_up(WORD vk) {
    INPUT up{};
    up.type = INPUT_KEYBOARD;
    up.ki.wVk = vk;
    up.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &up, sizeof(INPUT));
}

static void tap_key(WORD vk, const Config& cfg) {
    if (cfg.reaction_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(cfg.reaction_ms));
    }

    key_down(vk);
    std::this_thread::sleep_for(std::chrono::milliseconds(cfg.hold_ms));
    key_up(vk);
}

static void worker_loop() {
    CaptureBuffer capture;
    int last_w = 0;
    int last_h = 0;
    bool key_is_down = false;
    WORD held_vk = 0;

    using clock = std::chrono::steady_clock;
    auto next_allowed = clock::now();

    while (g_running.load(std::memory_order_relaxed)) {
        const Config cfg = current_config_copy();

        if (cfg.box_w != last_w || cfg.box_h != last_h || !capture.pixels) {
            if (!capture.init(cfg.box_w, cfg.box_h)) {
                g_running.store(false);
                PostMessageW(g_main, WM_APP + 1, 0, 0);
                return;
            }
            last_w = cfg.box_w;
            last_h = cfg.box_h;
        }

        const bool visible =
            capture.capture_center() && contains_target_color(capture.pixels, cfg.box_w, cfg.box_h, cfg);

        if (cfg.action_mode == ActionMode::HoldWhileVisible) {
            if (visible && !key_is_down) {
                if (cfg.reaction_ms > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(cfg.reaction_ms));
                }
                key_down(cfg.vk);
                key_is_down = true;
                held_vk = cfg.vk;
            } else if (!visible && key_is_down) {
                key_up(held_vk);
                key_is_down = false;
                held_vk = 0;
            }
        } else if (visible) {
            const auto now = clock::now();
            if (now >= next_allowed) {
                next_allowed = now + std::chrono::milliseconds(cfg.press_interval_ms);
                tap_key(cfg.vk, cfg);
            }
        } else if (key_is_down) {
            key_up(held_vk);
            key_is_down = false;
            held_vk = 0;
        }

        if (cfg.loop_sleep_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg.loop_sleep_ms));
        } else {
            std::this_thread::yield();
        }
    }

    if (key_is_down) {
        key_up(held_vk);
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

static void refresh_action_controls() {
    const bool tap_mode = static_cast<ActionMode>(combo_index(g_action)) == ActionMode::TapRepeatedly;
    EnableWindow(g_press_interval_ms, tap_mode ? TRUE : FALSE);
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
    HWND hwnd = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                                x, y, w, h, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    apply_font(hwnd, g_font_bold);
    return hwnd;
}

static HWND add_edit(HWND parent, int id, int x, int y, int w, int h, const wchar_t* text) {
    HWND hwnd = CreateWindowExW(0, L"EDIT", text, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_BORDER,
                                x, y, w, h, parent, reinterpret_cast<HMENU>(id), GetModuleHandleW(nullptr), nullptr);
    apply_font(hwnd, g_font);
    return hwnd;
}

static HWND add_combo(HWND parent, int id, int x, int y, int w, int h) {
    HWND hwnd = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                x, y, w, h, parent, reinterpret_cast<HMENU>(id), GetModuleHandleW(nullptr), nullptr);
    apply_font(hwnd, g_font);
    return hwnd;
}

static HWND add_button(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h) {
    HWND hwnd = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                x, y, w, h, parent, reinterpret_cast<HMENU>(id), GetModuleHandleW(nullptr), nullptr);
    apply_font(hwnd, g_font_bold);
    return hwnd;
}

static void create_controls(HWND hwnd) {
    g_status = add_label(hwnd, L"OFF - press F8", 615, 38, 135, 26);
    apply_font(g_status, g_font_small);

    add_label(hwnd, L"Color to watch", 64, 166, 135, 22);
    g_mode = add_combo(hwnd, IDC_MODE, 64, 192, 250, 180);
    combo_add(g_mode, L"Yellow outline");
    combo_add(g_mode, L"Red outline");
    combo_add(g_mode, L"Purple outline");
    combo_add(g_mode, L"Custom color");
    SendMessageW(g_mode, CB_SETCURSEL, 0, 0);

    g_preview = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                334, 192, 54, 28, hwnd, reinterpret_cast<HMENU>(IDC_PREVIEW),
                                GetModuleHandleW(nullptr), nullptr);

    add_label(hwnd, L"Key", 544, 166, 70, 22);
    g_key = add_edit(hwnd, IDC_KEY, 544, 192, 132, 28, L"F");

    add_label(hwnd, L"Action", 64, 248, 120, 22);
    g_action = add_combo(hwnd, IDC_ACTION, 64, 274, 250, 120);
    combo_add(g_action, L"Tap repeatedly");
    combo_add(g_action, L"Hold while visible");
    SendMessageW(g_action, CB_SETCURSEL, 0, 0);

    add_label(hwnd, L"Reaction", 544, 248, 120, 22);
    g_reaction_ms = add_edit(hwnd, IDC_REACTION_MS, 544, 274, 80, 28, L"0");
    add_label(hwnd, L"ms", 636, 279, 40, 20);

    add_label(hwnd, L"Tap interval", 64, 330, 120, 22);
    g_press_interval_ms = add_edit(hwnd, IDC_PRESS_INTERVAL_MS, 64, 356, 90, 28, L"25");
    add_label(hwnd, L"ms", 166, 361, 40, 20);

    add_label(hwnd, L"Detection", 236, 330, 120, 22);
    g_sensitivity = add_combo(hwnd, IDC_SENSITIVITY, 236, 356, 150, 140);
    combo_add(g_sensitivity, L"Strict");
    combo_add(g_sensitivity, L"Normal");
    combo_add(g_sensitivity, L"Loose");
    SendMessageW(g_sensitivity, CB_SETCURSEL, 1, 0);

    add_label(hwnd, L"Scan box", 544, 330, 100, 22);
    g_box_w = add_edit(hwnd, IDC_BOX_W, 544, 356, 64, 28, L"40");
    add_label(hwnd, L"x", 618, 361, 16, 20);
    g_box_h = add_edit(hwnd, IDC_BOX_H, 638, 356, 64, 28, L"40");

    add_label(hwnd, L"Custom RGB", 64, 414, 120, 22);
    g_custom_r = add_edit(hwnd, IDC_CUSTOM_R, 64, 440, 58, 28, L"255");
    g_custom_g = add_edit(hwnd, IDC_CUSTOM_G, 134, 440, 58, 28, L"230");
    g_custom_b = add_edit(hwnd, IDC_CUSTOM_B, 204, 440, 58, 28, L"0");
    g_pick_color = add_button(hwnd, IDC_PICK_COLOR, L"PICK", 284, 438, 92, 32);

    add_button(hwnd, IDC_START, L"START", 120, 498, 250, 44);
    add_button(hwnd, IDC_STOP, L"STOP", 430, 498, 250, 44);
    EnableWindow(GetDlgItem(hwnd, IDC_STOP), FALSE);

    add_label(hwnd, L"F8 start/stop    F9 stop", 300, 558, 250, 22);
    refresh_custom_controls();
    refresh_action_controls();
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

static void fill_round(HDC dc, RECT rect, int radius, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
}

static void draw_text(HDC dc, const wchar_t* text, RECT rect, HFONT font, COLORREF color, UINT flags) {
    HGDIOBJ old_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &rect, flags);
    SelectObject(dc, old_font);
}

static void paint_window(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);

    RECT client{};
    GetClientRect(hwnd, &client);
    FillRect(dc, &client, g_bg_brush);

    RECT brand{40, 28, 200, 62};
    draw_text(dc, L"minhan", brand, g_font_title, COLOR_TEXT, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    RECT dot{126, 28, 235, 62};
    draw_text(dc, L".time", dot, g_font_title, COLOR_ACCENT, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    RECT subtitle{42, 66, 230, 90};
    draw_text(dc, L"screen color control", subtitle, g_font_small, COLOR_MUTED, DT_LEFT | DT_SINGLELINE);

    RECT panel_left{40, 112, 500, 430};
    RECT panel_right{520, 112, 760, 430};
    fill_round(dc, panel_left, 10, COLOR_PANEL);
    fill_round(dc, panel_right, 10, RGB(22, 23, 29));

    RECT panel_title{64, 132, 360, 158};
    draw_text(dc, L"Trigger settings", panel_title, g_font_bold, COLOR_TEXT, DT_LEFT | DT_SINGLELINE);
    RECT panel_title2{544, 132, 720, 158};
    draw_text(dc, L"Key + scan", panel_title2, g_font_bold, COLOR_TEXT, DT_LEFT | DT_SINGLELINE);

    EndPaint(hwnd, &ps);
}

static void draw_owner_button(const DRAWITEMSTRUCT* item) {
    HDC dc = item->hDC;
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;

    wchar_t text[64]{};
    GetWindowTextW(item->hwndItem, text, 64);

    COLORREF fill = COLOR_STOP;
    COLORREF text_color = disabled ? RGB(118, 112, 128) : COLOR_TEXT;
    if (item->CtlID == IDC_START) fill = pressed ? RGB(156, 55, 224) : COLOR_ACCENT;
    if (item->CtlID == IDC_PICK_COLOR) fill = pressed ? RGB(50, 155, 203) : COLOR_ACCENT_2;
    if (item->CtlID == IDC_STOP) fill = pressed ? RGB(78, 72, 88) : RGB(52, 49, 58);
    if (disabled) fill = RGB(34, 34, 40);

    RECT r = item->rcItem;
    fill_round(dc, r, 10, fill);
    draw_text(dc, text, r, g_font_bold, text_color, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CREATE:
        init_theme();
        g_app_icon = create_app_icon();
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_app_icon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_app_icon));
        create_controls(hwnd);
        RegisterHotKey(hwnd, 1, 0, VK_F8);
        RegisterHotKey(hwnd, 2, 0, VK_F9);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        paint_window(hwnd);
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
        case IDC_ACTION:
            if (HIWORD(wparam) == CBN_SELCHANGE) refresh_action_controls();
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

    case WM_DRAWITEM:
        draw_owner_button(reinterpret_cast<const DRAWITEMSTRUCT*>(lparam));
        return TRUE;

    case WM_CTLCOLORBTN:
        SetBkMode(reinterpret_cast<HDC>(wparam), TRANSPARENT);
        return reinterpret_cast<LRESULT>(g_bg_brush);

    case WM_CTLCOLOREDIT:
        SetTextColor(reinterpret_cast<HDC>(wparam), COLOR_TEXT);
        SetBkColor(reinterpret_cast<HDC>(wparam), COLOR_INPUT);
        return reinterpret_cast<LRESULT>(g_input_brush);

    case WM_CTLCOLORLISTBOX:
        SetTextColor(reinterpret_cast<HDC>(wparam), COLOR_TEXT);
        SetBkColor(reinterpret_cast<HDC>(wparam), COLOR_INPUT);
        return reinterpret_cast<LRESULT>(g_input_brush);

    case WM_CTLCOLORSTATIC:
        if (reinterpret_cast<HWND>(lparam) == g_preview) {
            if (g_preview_brush) DeleteObject(g_preview_brush);
            g_preview_brush = CreateSolidBrush(preview_color());
            return reinterpret_cast<LRESULT>(g_preview_brush);
        }
        SetBkMode(reinterpret_cast<HDC>(wparam), TRANSPARENT);
        SetTextColor(reinterpret_cast<HDC>(wparam), COLOR_TEXT);
        return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));

    case WM_APP + 1:
        stop_worker();
        MessageBoxW(hwnd, L"Screen capture could not start.", L"minhan-time", MB_ICONERROR);
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
        if (g_bg_brush) DeleteObject(g_bg_brush);
        if (g_panel_brush) DeleteObject(g_panel_brush);
        if (g_input_brush) DeleteObject(g_input_brush);
        if (g_label_brush) DeleteObject(g_label_brush);
        if (g_font) DeleteObject(g_font);
        if (g_font_bold) DeleteObject(g_font_bold);
        if (g_font_title) DeleteObject(g_font_title);
        if (g_font_small) DeleteObject(g_font_small);
        if (g_app_icon) DestroyIcon(g_app_icon);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_cmd) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const wchar_t class_name[] = L"MinhanTimeWindow";
    WNDCLASSW wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = class_name;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    if (!RegisterClassW(&wc)) return 1;

    g_main = CreateWindowExW(0, class_name, L"minhan-time",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 800, 635,
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

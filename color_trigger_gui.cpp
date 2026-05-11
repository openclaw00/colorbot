/*
  minhan-time - simple color-trigger keyboard utility for Windows 10/11.

  Build on Windows:
    cl /std:c++20 /O2 /EHsc /DUNICODE /D_UNICODE color_trigger_gui.cpp user32.lib gdi32.lib comdlg32.lib dwmapi.lib /link /SUBSYSTEM:WINDOWS

  Build from macOS with mingw-w64:
    x86_64-w64-mingw32-g++ -std=c++20 -O2 -municode -mwindows -static -static-libgcc -static-libstdc++ color_trigger_gui.cpp -o minhan-time.exe -luser32 -lgdi32 -lcomdlg32 -ldwmapi

  Use:
    1. Pick a color mode: Yellow, Red, Purple, or Custom.
    2. Pick the key to press.
    3. Pick tap mode or hold mode.
    4. Set reaction delay, press interval, and scan box size.
    5. Save up to 5 named local configs.
    6. Click START.
    5. Press F8 to turn it on/off while in another program.
    6. Press F9 to stop immediately.

  This app watches a box centered on the screen. It sends keyboard input, not mouse input.
*/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <dwmapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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
    IDC_STATUS,
    IDC_CONFIG_NAME,
    IDC_CONFIG_LIST,
    IDC_SAVE_CONFIG,
    IDC_LOAD_CONFIG,
    IDC_DELETE_CONFIG,
    IDC_TITLE_MINIMIZE,
    IDC_TITLE_CLOSE,
    IDC_SELECT_POPUP
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

struct SavedConfig {
    std::wstring name;
    Config config;
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
static HWND g_config_name = nullptr;
static HWND g_config_list = nullptr;
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
static HWND g_popup_list = nullptr;
static HWND g_popup_owner = nullptr;
static const wchar_t SELECT_CLASS_NAME[] = L"MinhanSelect";
static const wchar_t POPUP_CLASS_NAME[] = L"MinhanSelectPopup";
static std::vector<RECT> g_input_frames;

struct SelectData {
    std::vector<std::wstring> items;
    int selected = -1;
    bool open = false;
    RECT closed_rect{};
    int closed_height = 38;
    WNDPROC old_proc = nullptr;
};

static LRESULT CALLBACK select_button_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

static constexpr COLORREF COLOR_BG = RGB(9, 9, 11);
static constexpr COLORREF COLOR_SURFACE = RGB(17, 19, 24);
static constexpr COLORREF COLOR_ELEVATED = RGB(24, 27, 34);
static constexpr COLORREF COLOR_BORDER = RGB(42, 47, 58);
static constexpr COLORREF COLOR_INPUT = RGB(15, 17, 23);
static constexpr COLORREF COLOR_INPUT_BORDER = RGB(35, 40, 52);
static constexpr COLORREF COLOR_TEXT = RGB(245, 247, 250);
static constexpr COLORREF COLOR_MUTED = RGB(156, 163, 175);
static constexpr COLORREF COLOR_ACCENT = RGB(168, 85, 247);
static constexpr COLORREF COLOR_ACCENT_HOVER = RGB(192, 132, 252);
static constexpr COLORREF COLOR_DANGER = RGB(255, 91, 110);

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif

static std::mutex g_config_mutex;
static Config g_config;
static std::vector<SavedConfig> g_saved_configs;
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
    g_panel_brush = CreateSolidBrush(COLOR_SURFACE);
    g_input_brush = CreateSolidBrush(COLOR_INPUT);
    g_label_brush = CreateSolidBrush(COLOR_BG);
    g_font = make_font(13, FW_NORMAL);
    g_font_bold = make_font(13, FW_MEDIUM);
    g_font_title = make_font(16, FW_SEMIBOLD);
    g_font_small = make_font(12, FW_NORMAL);
}

static void apply_dark_window_chrome(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));

    int rounded = 2;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &rounded, sizeof(rounded));

    COLORREF caption = COLOR_BG;
    COLORREF caption_text = COLOR_TEXT;
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &caption_text, sizeof(caption_text));
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

static std::string to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(size > 0 ? size - 1 : 0, '\0');
    if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), size, nullptr, nullptr);
    return out;
}

static std::wstring from_utf8(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring out(size > 0 ? size - 1 : 0, L'\0');
    if (size > 0) MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), size);
    return out;
}

static std::wstring trim_ws(std::wstring s) {
    while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    return s;
}

static std::string json_escape(const std::wstring& text) {
    std::string input = to_utf8(text);
    std::string out;
    for (char c : input) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

static std::wstring config_file_path() {
    wchar_t appdata[MAX_PATH]{};
    DWORD len = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    std::wstring dir = (len > 0 && len < MAX_PATH) ? appdata : L".";
    dir += L"\\minhan-time";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\configs.json";
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
    SelectData* data = reinterpret_cast<SelectData*>(GetWindowLongPtrW(combo, GWLP_USERDATA));
    if (data) return data->selected < 0 ? 0 : data->selected;
    const LRESULT index = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    return index == CB_ERR ? 0 : static_cast<int>(index);
}

static void combo_add(HWND combo, const wchar_t* text) {
    SelectData* data = reinterpret_cast<SelectData*>(GetWindowLongPtrW(combo, GWLP_USERDATA));
    if (data) {
        data->items.push_back(text);
        if (data->selected < 0) data->selected = 0;
        InvalidateRect(combo, nullptr, TRUE);
        return;
    }
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
}

static void combo_set(HWND combo, int index) {
    SelectData* data = reinterpret_cast<SelectData*>(GetWindowLongPtrW(combo, GWLP_USERDATA));
    if (data) {
        data->selected = (index >= 0 && index < static_cast<int>(data->items.size())) ? index : -1;
        InvalidateRect(combo, nullptr, TRUE);
        return;
    }
    SendMessageW(combo, CB_SETCURSEL, index, 0);
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

static void refresh_custom_controls();
static void refresh_action_controls();

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

static std::wstring key_name_from_vk(WORD vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        return std::wstring(1, static_cast<wchar_t>(vk));
    }
    if (vk == VK_SPACE) return L"SPACE";
    if (vk == VK_RETURN) return L"ENTER";
    if (vk == VK_TAB) return L"TAB";
    if (vk == VK_SHIFT) return L"SHIFT";
    if (vk == VK_CONTROL) return L"CTRL";
    if (vk == VK_MENU) return L"ALT";
    if (vk == VK_ESCAPE) return L"ESC";
    if (vk >= VK_F1 && vk <= VK_F24) {
        wchar_t buf[8]{};
        wsprintfW(buf, L"F%d", vk - VK_F1 + 1);
        return buf;
    }
    return L"F";
}

static void apply_config_to_ui(const Config& cfg) {
    combo_set(g_mode, static_cast<int>(cfg.color_mode));
    combo_set(g_action, static_cast<int>(cfg.action_mode));
    combo_set(g_sensitivity, static_cast<int>(cfg.sensitivity));
    set_text(g_key, key_name_from_vk(cfg.vk).c_str());
    set_int(g_reaction_ms, cfg.reaction_ms);
    set_int(g_press_interval_ms, cfg.press_interval_ms);
    set_int(g_box_w, cfg.box_w);
    set_int(g_box_h, cfg.box_h);
    set_int(g_custom_r, cfg.custom_r);
    set_int(g_custom_g, cfg.custom_g);
    set_int(g_custom_b, cfg.custom_b);
    refresh_custom_controls();
    refresh_action_controls();
}

static bool read_file_text(const std::wstring& path, std::string& out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(file, nullptr);
    if (size == INVALID_FILE_SIZE || size == 0) {
        CloseHandle(file);
        out.clear();
        return true;
    }
    out.assign(size, '\0');
    DWORD read = 0;
    BOOL ok = ReadFile(file, out.data(), size, &read, nullptr);
    CloseHandle(file);
    if (!ok) return false;
    out.resize(read);
    return true;
}

static bool write_file_text(const std::wstring& path, const std::string& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
    return ok && written == text.size();
}

static int json_int(const std::string& obj, const char* key, int fallback) {
    std::string needle = std::string("\"") + key + "\":";
    size_t p = obj.find(needle);
    if (p == std::string::npos) return fallback;
    p += needle.size();
    while (p < obj.size() && isspace(static_cast<unsigned char>(obj[p]))) ++p;
    char* end = nullptr;
    long value = strtol(obj.c_str() + p, &end, 10);
    return end == obj.c_str() + p ? fallback : static_cast<int>(value);
}

static std::wstring json_string(const std::string& obj, const char* key) {
    std::string needle = std::string("\"") + key + "\":\"";
    size_t p = obj.find(needle);
    if (p == std::string::npos) return {};
    p += needle.size();
    std::string out;
    bool escaped = false;
    for (; p < obj.size(); ++p) {
        char c = obj[p];
        if (escaped) {
            out.push_back(c == 'n' ? '\n' : c);
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            break;
        } else {
            out.push_back(c);
        }
    }
    return from_utf8(out);
}

static void refresh_config_list() {
    SendMessageW(g_config_list, CB_RESETCONTENT, 0, 0);
    for (const SavedConfig& saved : g_saved_configs) {
        combo_add(g_config_list, saved.name.c_str());
    }
    if (!g_saved_configs.empty()) combo_set(g_config_list, 0);
}

static void load_saved_configs() {
    g_saved_configs.clear();
    std::string json;
    if (!read_file_text(config_file_path(), json)) return;

    size_t pos = 0;
    while (g_saved_configs.size() < 5) {
        size_t start = json.find('{', pos);
        if (start == std::string::npos) break;
        size_t end = json.find('}', start);
        if (end == std::string::npos) break;
        std::string obj = json.substr(start, end - start + 1);
        pos = end + 1;

        std::wstring name = json_string(obj, "name");
        if (name.empty()) continue;

        SavedConfig saved{};
        saved.name = name;
        saved.config.color_mode = static_cast<ColorMode>(clamp_int(json_int(obj, "color_mode", 0), 0, 3));
        saved.config.action_mode = static_cast<ActionMode>(clamp_int(json_int(obj, "action_mode", 0), 0, 1));
        saved.config.sensitivity = static_cast<SensitivityMode>(clamp_int(json_int(obj, "sensitivity", 1), 0, 2));
        saved.config.vk = static_cast<WORD>(clamp_int(json_int(obj, "vk", 'F'), 1, 255));
        saved.config.box_w = clamp_int(json_int(obj, "box_w", 40), 1, 800);
        saved.config.box_h = clamp_int(json_int(obj, "box_h", 40), 1, 800);
        saved.config.custom_r = clamp_int(json_int(obj, "custom_r", 255), 0, 255);
        saved.config.custom_g = clamp_int(json_int(obj, "custom_g", 230), 0, 255);
        saved.config.custom_b = clamp_int(json_int(obj, "custom_b", 0), 0, 255);
        saved.config.reaction_ms = clamp_int(json_int(obj, "reaction_ms", 0), 0, 10000);
        saved.config.press_interval_ms = clamp_int(json_int(obj, "press_interval_ms", 25), 0, 60000);
        set_sensitivity(saved.config);
        g_saved_configs.push_back(saved);
    }
    refresh_config_list();
}

static void save_saved_configs() {
    std::ostringstream out;
    out << "{\n  \"configs\": [\n";
    for (size_t i = 0; i < g_saved_configs.size(); ++i) {
        const SavedConfig& s = g_saved_configs[i];
        const Config& c = s.config;
        out << "    {\"name\":\"" << json_escape(s.name)
            << "\",\"color_mode\":" << static_cast<int>(c.color_mode)
            << ",\"action_mode\":" << static_cast<int>(c.action_mode)
            << ",\"sensitivity\":" << static_cast<int>(c.sensitivity)
            << ",\"vk\":" << c.vk
            << ",\"box_w\":" << c.box_w
            << ",\"box_h\":" << c.box_h
            << ",\"custom_r\":" << c.custom_r
            << ",\"custom_g\":" << c.custom_g
            << ",\"custom_b\":" << c.custom_b
            << ",\"reaction_ms\":" << c.reaction_ms
            << ",\"press_interval_ms\":" << c.press_interval_ms
            << "}";
        if (i + 1 < g_saved_configs.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n}\n";
    write_file_text(config_file_path(), out.str());
}

static void save_current_config_named() {
    std::wstring name = trim_ws(get_text(g_config_name));
    if (name.empty()) {
        MessageBoxW(g_main, L"Name the config first.", L"minhan-time", MB_ICONINFORMATION);
        return;
    }

    Config cfg = read_config_from_ui();
    int existing = -1;
    for (size_t i = 0; i < g_saved_configs.size(); ++i) {
        if (lstrcmpiW(g_saved_configs[i].name.c_str(), name.c_str()) == 0) {
            existing = static_cast<int>(i);
            break;
        }
    }

    if (existing < 0 && g_saved_configs.size() >= 5) {
        MessageBoxW(g_main, L"Max 5 saved configs. Delete one first.", L"minhan-time", MB_ICONINFORMATION);
        return;
    }

    if (existing >= 0) {
        g_saved_configs[existing] = SavedConfig{name, cfg};
    } else {
        g_saved_configs.push_back(SavedConfig{name, cfg});
        existing = static_cast<int>(g_saved_configs.size() - 1);
    }

    save_saved_configs();
    refresh_config_list();
    combo_set(g_config_list, existing);
}

static void load_selected_config() {
    int index = combo_index(g_config_list);
    if (index < 0 || index >= static_cast<int>(g_saved_configs.size())) return;
    set_text(g_config_name, g_saved_configs[index].name.c_str());
    apply_config_to_ui(g_saved_configs[index].config);
}

static void delete_selected_config() {
    int index = combo_index(g_config_list);
    if (index < 0 || index >= static_cast<int>(g_saved_configs.size())) return;
    g_saved_configs.erase(g_saved_configs.begin() + index);
    save_saved_configs();
    refresh_config_list();
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
    set_text(g_status, running ? L"ON      *     F8 start/stop     *     F9 stop" : L"OFF     *     F8 start/stop     *     F9 stop");
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
    g_input_frames.push_back(RECT{x, y, x + w, y + h});
    HWND hwnd = CreateWindowExW(0, L"EDIT", text, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                x + 10, y + 7, std::max(10, w - 20), std::max(10, h - 14),
                                parent, reinterpret_cast<HMENU>(id), GetModuleHandleW(nullptr), nullptr);
    apply_font(hwnd, g_font);
    return hwnd;
}

static HWND add_combo(HWND parent, int id, int x, int y, int w, int h) {
    HWND hwnd = CreateWindowExW(0, L"BUTTON", L"",
                                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | BS_NOTIFY,
                                x, y, w, 38, parent, reinterpret_cast<HMENU>(id),
                                GetModuleHandleW(nullptr), nullptr);
    SelectData* data = new SelectData();
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));
    data->old_proc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(select_button_proc)));
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
    add_button(hwnd, IDC_TITLE_MINIMIZE, L"-", 604, 8, 34, 28);
    add_button(hwnd, IDC_TITLE_CLOSE, L"x", 644, 8, 34, 28);

    add_label(hwnd, L"Color", 44, 110, 90, 20);
    g_mode = add_combo(hwnd, IDC_MODE, 44, 134, 248, 190);
    combo_add(g_mode, L"Yellow outline");
    combo_add(g_mode, L"Red outline");
    combo_add(g_mode, L"Purple outline");
    combo_add(g_mode, L"Custom color");
    combo_set(g_mode, 0);

    g_preview = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                312, 134, 54, 38, hwnd, reinterpret_cast<HMENU>(IDC_PREVIEW),
                                GetModuleHandleW(nullptr), nullptr);

    add_label(hwnd, L"Key", 448, 110, 70, 20);
    g_key = add_edit(hwnd, IDC_KEY, 448, 134, 144, 38, L"F");

    add_label(hwnd, L"Action", 44, 190, 100, 20);
    g_action = add_combo(hwnd, IDC_ACTION, 44, 214, 248, 120);
    combo_add(g_action, L"Tap repeatedly");
    combo_add(g_action, L"Hold while visible");
    combo_set(g_action, 0);

    add_label(hwnd, L"Reaction", 448, 190, 100, 20);
    g_reaction_ms = add_edit(hwnd, IDC_REACTION_MS, 448, 214, 80, 38, L"0");
    add_label(hwnd, L"ms", 544, 224, 35, 18);

    add_label(hwnd, L"Tap interval", 44, 270, 100, 20);
    g_press_interval_ms = add_edit(hwnd, IDC_PRESS_INTERVAL_MS, 44, 294, 82, 38, L"25");
    add_label(hwnd, L"ms", 142, 304, 35, 18);

    add_label(hwnd, L"Detection", 200, 270, 95, 20);
    g_sensitivity = add_combo(hwnd, IDC_SENSITIVITY, 200, 294, 150, 150);
    combo_add(g_sensitivity, L"Strict");
    combo_add(g_sensitivity, L"Normal");
    combo_add(g_sensitivity, L"Loose");
    combo_set(g_sensitivity, 1);

    add_label(hwnd, L"Scan box", 448, 270, 100, 20);
    g_box_w = add_edit(hwnd, IDC_BOX_W, 448, 294, 60, 38, L"40");
    add_label(hwnd, L"x", 520, 304, 16, 18);
    g_box_h = add_edit(hwnd, IDC_BOX_H, 540, 294, 60, 38, L"40");

    add_label(hwnd, L"Custom RGB", 44, 350, 100, 20);
    g_custom_r = add_edit(hwnd, IDC_CUSTOM_R, 44, 374, 70, 38, L"255");
    g_custom_g = add_edit(hwnd, IDC_CUSTOM_G, 128, 374, 70, 38, L"230");
    g_custom_b = add_edit(hwnd, IDC_CUSTOM_B, 212, 374, 70, 38, L"0");
    g_pick_color = add_button(hwnd, IDC_PICK_COLOR, L"PICK", 304, 374, 86, 38);

    g_config_name = add_edit(hwnd, IDC_CONFIG_NAME, 44, 452, 150, 38, L"default");
    g_config_list = add_combo(hwnd, IDC_CONFIG_LIST, 210, 452, 180, 190);
    add_button(hwnd, IDC_SAVE_CONFIG, L"SAVE", 406, 452, 64, 38);
    add_button(hwnd, IDC_LOAD_CONFIG, L"LOAD", 486, 452, 64, 38);
    add_button(hwnd, IDC_DELETE_CONFIG, L"DEL", 566, 452, 62, 38);

    add_button(hwnd, IDC_START, L"START", 20, 510, 302, 46);
    add_button(hwnd, IDC_STOP, L"STOP", 342, 510, 302, 46);
    EnableWindow(GetDlgItem(hwnd, IDC_STOP), FALSE);

    g_status = add_label(hwnd, L"OFF     *     F8 start/stop     *     F9 stop", 194, 566, 330, 20);
    apply_font(g_status, g_font_small);
    refresh_custom_controls();
    refresh_action_controls();
    load_saved_configs();
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
        combo_set(g_mode, static_cast<int>(ColorMode::Custom));
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

static void stroke_round(HDC dc, RECT rect, int radius, COLORREF color, int width = 1) {
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
}

static void draw_shadow(HDC dc, RECT rect, int radius) {
    RECT outer = rect;
    InflateRect(&outer, 4, 6);
    fill_round(dc, outer, radius + 8, RGB(7, 8, 12));
}

static void draw_text(HDC dc, const wchar_t* text, RECT rect, HFONT font, COLORREF color, UINT flags) {
    HGDIOBJ old_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &rect, flags);
    SelectObject(dc, old_font);
}

static SelectData* select_data(HWND hwnd) {
    return reinterpret_cast<SelectData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

static void close_select_popup() {
    if (g_popup_list) {
        ReleaseCapture();
        DestroyWindow(g_popup_list);
        g_popup_list = nullptr;
    }
    g_popup_owner = nullptr;
}

static void notify_select_change(HWND hwnd) {
    HWND parent = GetParent(hwnd);
    const int id = GetDlgCtrlID(hwnd);
    SendMessageW(parent, WM_COMMAND, MAKEWPARAM(id, CBN_SELCHANGE), reinterpret_cast<LPARAM>(hwnd));
}

static void choose_popup_index(int index) {
    if (!g_popup_owner) return;
    SelectData* data = select_data(g_popup_owner);
    if (!data) return;
    if (index >= 0 && index < static_cast<int>(data->items.size())) {
        data->selected = index;
        InvalidateRect(g_popup_owner, nullptr, TRUE);
        notify_select_change(g_popup_owner);
    }
    close_select_popup();
}

static LRESULT CALLBACK popup_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
            close_select_popup();
            return 0;
        }
        break;

    case WM_LBUTTONDOWN: {
        RECT r{};
        GetClientRect(hwnd, &r);
        const int x = GET_X_LPARAM(lparam);
        const int y = GET_Y_LPARAM(lparam);
        if (x < 0 || x >= r.right || y < 0 || y >= r.bottom) {
            close_select_popup();
            return 0;
        }
        choose_popup_index(y / 30);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r{};
        GetClientRect(hwnd, &r);
        FillRect(dc, &r, g_input_brush);
        stroke_round(dc, r, 8, COLOR_INPUT_BORDER);

        SelectData* data = g_popup_owner ? select_data(g_popup_owner) : nullptr;
        if (data) {
            for (int i = 0; i < static_cast<int>(data->items.size()); ++i) {
                RECT item_rect{1, 1 + i * 30, r.right - 1, 1 + (i + 1) * 30};
                if (i == data->selected) {
                    HBRUSH brush = CreateSolidBrush(RGB(32, 25, 43));
                    FillRect(dc, &item_rect, brush);
                    DeleteObject(brush);
                }
                RECT text_rect = item_rect;
                text_rect.left += 12;
                text_rect.right -= 12;
                draw_text(dc, data->items[i].c_str(), text_rect, g_font,
                          i == data->selected ? COLOR_ACCENT_HOVER : COLOR_TEXT,
                          DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            }
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static void show_select_popup(HWND hwnd) {
    SelectData* data = select_data(hwnd);
    if (!data || data->items.empty() || !IsWindowEnabled(hwnd)) return;
    if (g_popup_owner == hwnd) {
        close_select_popup();
        return;
    }

    close_select_popup();

    RECT select_rect{};
    GetWindowRect(hwnd, &select_rect);

    const int row_h = 30;
    const int rows = clamp_int(static_cast<int>(data->items.size()), 1, 5);
    const int width = select_rect.right - select_rect.left;
    const int height = rows * row_h + 2;
    RECT work_area{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
    int popup_y = select_rect.bottom + 4;
    if (popup_y + height > work_area.bottom - 8) popup_y = select_rect.top - height - 4;

    g_popup_owner = hwnd;
    g_popup_list = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, POPUP_CLASS_NAME, L"",
                                   WS_POPUP, select_rect.left, popup_y, width, height,
                                   GetParent(hwnd), reinterpret_cast<HMENU>(IDC_SELECT_POPUP),
                                   GetModuleHandleW(nullptr), nullptr);
    if (!g_popup_list) {
        g_popup_owner = nullptr;
        return;
    }
    SetWindowPos(g_popup_list, HWND_TOPMOST, select_rect.left, popup_y, width, height, SWP_SHOWWINDOW);
    SetFocus(g_popup_list);
    SetCapture(g_popup_list);
}

static LRESULT CALLBACK select_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    SelectData* data = select_data(hwnd);

    switch (msg) {
    case WM_CREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new SelectData()));
        return 0;

    case WM_DESTROY:
        if (g_popup_owner == hwnd) close_select_popup();
        delete data;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;

    case CB_ADDSTRING:
        if (!data) return CB_ERR;
        data->items.push_back(reinterpret_cast<const wchar_t*>(lparam));
        if (data->selected < 0) data->selected = 0;
        InvalidateRect(hwnd, nullptr, TRUE);
        return static_cast<LRESULT>(data->items.size() - 1);

    case CB_RESETCONTENT:
        if (!data) return 0;
        data->items.clear();
        data->selected = -1;
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;

    case CB_GETCURSEL:
        return data ? data->selected : CB_ERR;

    case CB_GETLBTEXT:
        if (!data || static_cast<int>(wparam) < 0 || static_cast<int>(wparam) >= static_cast<int>(data->items.size())) {
            return CB_ERR;
        }
        lstrcpyW(reinterpret_cast<wchar_t*>(lparam), data->items[static_cast<int>(wparam)].c_str());
        return static_cast<LRESULT>(data->items[static_cast<int>(wparam)].size());

    case CB_SETCURSEL:
        if (!data) return CB_ERR;
        if (static_cast<int>(wparam) >= 0 && static_cast<int>(wparam) < static_cast<int>(data->items.size())) {
            data->selected = static_cast<int>(wparam);
            InvalidateRect(hwnd, nullptr, TRUE);
            return data->selected;
        }
        data->selected = -1;
        InvalidateRect(hwnd, nullptr, TRUE);
        return CB_ERR;

    case WM_ENABLE:
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;

    case WM_LBUTTONDOWN:
        if (data && data->open) {
            const int y = GET_Y_LPARAM(lparam);
            if (y <= data->closed_height) {
                close_select_popup();
            } else {
                choose_popup_index((y - data->closed_height - 4) / 30);
            }
        } else {
            show_select_popup(hwnd);
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r{};
        GetClientRect(hwnd, &r);
        const bool disabled = !IsWindowEnabled(hwnd);
        RECT closed{0, 0, r.right, data ? data->closed_height : r.bottom};
        fill_round(dc, closed, 10, disabled ? RGB(16, 17, 22) : COLOR_INPUT);
        stroke_round(dc, closed, 10, disabled ? RGB(30, 34, 42) : COLOR_INPUT_BORDER);

        RECT text_rect = closed;
        text_rect.left += 14;
        text_rect.right -= 42;
        const wchar_t* text = L"";
        if (data && data->selected >= 0 && data->selected < static_cast<int>(data->items.size())) {
            text = data->items[data->selected].c_str();
        }
        draw_text(dc, text, text_rect, g_font, disabled ? COLOR_MUTED : COLOR_TEXT,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        HPEN pen = CreatePen(PS_SOLID, 2, disabled ? COLOR_MUTED : COLOR_TEXT);
        HGDIOBJ old_pen = SelectObject(dc, pen);
        const int cx = closed.right - 22;
        const int cy = (closed.bottom - closed.top) / 2;
        MoveToEx(dc, cx - 5, cy - 2, nullptr);
        LineTo(dc, cx, cy + 4);
        LineTo(dc, cx + 5, cy - 2);
        SelectObject(dc, old_pen);
        DeleteObject(pen);

        if (data && data->open) {
            RECT list_rect{0, data->closed_height + 4, r.right, r.bottom};
            fill_round(dc, list_rect, 8, COLOR_INPUT);
            stroke_round(dc, list_rect, 8, COLOR_INPUT_BORDER);
            const int visible_rows = clamp_int(static_cast<int>(data->items.size()), 1, 5);
            for (int i = 0; i < visible_rows; ++i) {
                RECT item_rect{1, data->closed_height + 5 + i * 30, r.right - 1,
                               data->closed_height + 5 + (i + 1) * 30};
                if (i == data->selected) {
                    HBRUSH brush = CreateSolidBrush(RGB(32, 25, 43));
                    FillRect(dc, &item_rect, brush);
                    DeleteObject(brush);
                }
                RECT item_text = item_rect;
                item_text.left += 12;
                item_text.right -= 12;
                draw_text(dc, data->items[i].c_str(), item_text, g_font,
                          i == data->selected ? COLOR_ACCENT_HOVER : COLOR_TEXT,
                          DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            }
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static LRESULT CALLBACK select_button_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    SelectData* data = select_data(hwnd);

    switch (msg) {
    case CB_ADDSTRING:
        if (!data) return CB_ERR;
        data->items.push_back(reinterpret_cast<const wchar_t*>(lparam));
        if (data->selected < 0) data->selected = 0;
        InvalidateRect(hwnd, nullptr, TRUE);
        return static_cast<LRESULT>(data->items.size() - 1);

    case CB_RESETCONTENT:
        if (!data) return 0;
        data->items.clear();
        data->selected = -1;
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;

    case CB_GETCURSEL:
        return data ? data->selected : CB_ERR;

    case CB_SETCURSEL:
        if (!data) return CB_ERR;
        if (static_cast<int>(wparam) >= 0 && static_cast<int>(wparam) < static_cast<int>(data->items.size())) {
            data->selected = static_cast<int>(wparam);
            InvalidateRect(hwnd, nullptr, TRUE);
            return data->selected;
        }
        data->selected = -1;
        InvalidateRect(hwnd, nullptr, TRUE);
        return CB_ERR;

    case WM_NCDESTROY:
        if (g_popup_owner == hwnd) close_select_popup();
        if (data) {
            WNDPROC old_proc = data->old_proc;
            delete data;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return CallWindowProcW(old_proc, hwnd, msg, wparam, lparam);
        }
        break;

    case WM_LBUTTONDOWN:
        show_select_popup(hwnd);
        return 0;

    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_HAND));
        return TRUE;
    }

    return data && data->old_proc ? CallWindowProcW(data->old_proc, hwnd, msg, wparam, lparam)
                                  : DefWindowProcW(hwnd, msg, wparam, lparam);
}

static void paint_window(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);

    RECT client{};
    GetClientRect(hwnd, &client);
    FillRect(dc, &client, g_bg_brush);

    RECT title_bar{0, 0, client.right, 48};
    fill_round(dc, title_bar, 0, COLOR_BG);

    RECT logo_ring{24, 14, 44, 34};
    stroke_round(dc, logo_ring, 20, COLOR_ACCENT, 3);
    RECT logo_dot{31, 21, 37, 27};
    fill_round(dc, logo_dot, 6, COLOR_ACCENT);
    RECT title{56, 10, 220, 38};
    draw_text(dc, L"minhan-time", title, g_font_bold, COLOR_TEXT, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    RECT panel_left{20, 64, 408, 432};
    RECT panel_right{424, 64, 644, 432};
    RECT panel_config{20, 444, 644, 500};
    draw_shadow(dc, panel_left, 12);
    draw_shadow(dc, panel_right, 12);
    draw_shadow(dc, panel_config, 12);
    fill_round(dc, panel_left, 12, COLOR_SURFACE);
    stroke_round(dc, panel_left, 12, COLOR_BORDER);
    fill_round(dc, panel_right, 12, COLOR_SURFACE);
    stroke_round(dc, panel_right, 12, COLOR_BORDER);
    fill_round(dc, panel_config, 12, COLOR_SURFACE);
    stroke_round(dc, panel_config, 12, COLOR_BORDER);

    for (const RECT& r : g_input_frames) {
        fill_round(dc, r, 10, COLOR_INPUT);
        stroke_round(dc, r, 10, COLOR_INPUT_BORDER);
    }

    RECT preview{312, 134, 366, 172};
    fill_round(dc, preview, 8, preview_color());
    stroke_round(dc, preview, 8, COLOR_INPUT_BORDER);

    RECT panel_title{44, 88, 280, 112};
    draw_text(dc, L"TRIGGER SETTINGS", panel_title, g_font_title, COLOR_ACCENT_HOVER, DT_LEFT | DT_SINGLELINE);
    RECT panel_title2{448, 88, 620, 112};
    draw_text(dc, L"SCAN SETTINGS", panel_title2, g_font_title, COLOR_ACCENT_HOVER, DT_LEFT | DT_SINGLELINE);
    EndPaint(hwnd, &ps);
}

static void draw_owner_button(const DRAWITEMSTRUCT* item) {
    HDC dc = item->hDC;
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;

    wchar_t text[64]{};
    GetWindowTextW(item->hwndItem, text, 64);

    COLORREF fill = RGB(35, 38, 47);
    COLORREF text_color = disabled ? RGB(118, 112, 128) : COLOR_TEXT;
    if (item->CtlID == IDC_TITLE_MINIMIZE || item->CtlID == IDC_TITLE_CLOSE) {
        fill = pressed ? RGB(31, 34, 42) : COLOR_BG;
        text_color = item->CtlID == IDC_TITLE_CLOSE ? COLOR_DANGER : COLOR_MUTED;
    }
    if (item->CtlID == IDC_START) fill = pressed ? COLOR_ACCENT : COLOR_ACCENT_HOVER;
    if (item->CtlID == IDC_PICK_COLOR || item->CtlID == IDC_SAVE_CONFIG || item->CtlID == IDC_LOAD_CONFIG) {
        fill = pressed ? RGB(34, 26, 44) : RGB(24, 27, 34);
        text_color = COLOR_ACCENT_HOVER;
    }
    if (item->CtlID == IDC_DELETE_CONFIG) {
        fill = pressed ? RGB(45, 26, 32) : RGB(24, 27, 34);
        text_color = COLOR_DANGER;
    }
    if (item->CtlID == IDC_STOP) fill = pressed ? RGB(45, 49, 60) : RGB(31, 34, 42);
    if (disabled) fill = RGB(34, 34, 40);

    RECT r = item->rcItem;
    fill_round(dc, r, 10, fill);
    if (item->CtlID == IDC_TITLE_MINIMIZE || item->CtlID == IDC_TITLE_CLOSE) {
        stroke_round(dc, r, 10, pressed ? COLOR_BORDER : COLOR_BG);
    } else if (item->CtlID == IDC_START) {
        RECT inner = r;
        inner.bottom = r.top + (r.bottom - r.top) / 2;
        fill_round(dc, inner, 10, COLOR_ACCENT_HOVER);
        stroke_round(dc, r, 10, COLOR_ACCENT_HOVER);
    } else if (item->CtlID == IDC_DELETE_CONFIG) {
        stroke_round(dc, r, 10, COLOR_DANGER);
    } else if (item->CtlID == IDC_PICK_COLOR || item->CtlID == IDC_SAVE_CONFIG || item->CtlID == IDC_LOAD_CONFIG) {
        stroke_round(dc, r, 10, COLOR_ACCENT);
    } else {
        stroke_round(dc, r, 10, COLOR_BORDER);
    }
    draw_text(dc, text, r, g_font_bold, text_color, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
}

static void draw_popup_item(const DRAWITEMSTRUCT* item) {
    if (item->itemID == static_cast<UINT>(-1)) return;

    HDC dc = item->hDC;
    RECT r = item->rcItem;
    const bool selected = (item->itemState & ODS_SELECTED) != 0;
    FillRect(dc, &r, g_input_brush);
    if (selected) {
        HBRUSH brush = CreateSolidBrush(RGB(32, 25, 43));
        FillRect(dc, &r, brush);
        DeleteObject(brush);
    }

    wchar_t text[128]{};
    SendMessageW(item->hwndItem, LB_GETTEXT, item->itemID, reinterpret_cast<LPARAM>(text));
    RECT text_rect = r;
    text_rect.left += 12;
    text_rect.right -= 12;
    draw_text(dc, text, text_rect, g_font, selected ? COLOR_ACCENT_HOVER : COLOR_TEXT,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

static bool is_combo_control_id(UINT id) {
    return id == IDC_MODE || id == IDC_ACTION || id == IDC_SENSITIVITY || id == IDC_CONFIG_LIST;
}

static void draw_combo_item(const DRAWITEMSTRUCT* item) {
    HDC dc = item->hDC;
    RECT r = item->rcItem;
    const bool selected = (item->itemState & ODS_SELECTED) != 0;
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    const bool focus = (item->itemState & ODS_FOCUS) != 0;
    const bool button = item->CtlType == ODT_BUTTON;

    if (button) {
        fill_round(dc, r, 10, disabled ? RGB(16, 17, 22) : COLOR_INPUT);
        stroke_round(dc, r, 10, disabled ? RGB(30, 34, 42) : COLOR_INPUT_BORDER);
    } else {
        HBRUSH base = CreateSolidBrush(COLOR_INPUT);
        FillRect(dc, &r, base);
        DeleteObject(base);
    }

    if (!button && selected) {
        HBRUSH brush = CreateSolidBrush(RGB(32, 25, 43));
        FillRect(dc, &r, brush);
        DeleteObject(brush);
    }

    wchar_t text[128]{};
    UINT item_id = item->itemID;
    if (button || item_id == static_cast<UINT>(-1)) {
        const LRESULT cur = SendMessageW(item->hwndItem, CB_GETCURSEL, 0, 0);
        item_id = cur == CB_ERR ? static_cast<UINT>(-1) : static_cast<UINT>(cur);
    }
    if (item_id != static_cast<UINT>(-1)) {
        SendMessageW(item->hwndItem, CB_GETLBTEXT, item_id, reinterpret_cast<LPARAM>(text));
    }

    RECT text_rect = r;
    text_rect.left += button ? 14 : 10;
    text_rect.right -= button ? 42 : 10;
    draw_text(dc, text, text_rect, g_font,
              disabled ? COLOR_MUTED : (selected || focus ? COLOR_ACCENT_HOVER : COLOR_TEXT),
              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    if (button) {
        HPEN pen = CreatePen(PS_SOLID, 2, disabled ? COLOR_MUTED : COLOR_TEXT);
        HGDIOBJ old_pen = SelectObject(dc, pen);
        const int cx = r.right - 22;
        const int cy = (r.bottom - r.top) / 2;
        MoveToEx(dc, cx - 5, cy - 2, nullptr);
        LineTo(dc, cx, cy + 4);
        LineTo(dc, cx + 5, cy - 2);
        SelectObject(dc, old_pen);
        DeleteObject(pen);
    }
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CREATE:
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
        case IDC_TITLE_MINIMIZE:
            ShowWindow(hwnd, SW_MINIMIZE);
            return 0;
        case IDC_TITLE_CLOSE:
            SendMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        case IDC_START:
            start_worker();
            return 0;
        case IDC_STOP:
            stop_worker();
            return 0;
        case IDC_PICK_COLOR:
            choose_color(hwnd);
            return 0;
        case IDC_SAVE_CONFIG:
            save_current_config_named();
            return 0;
        case IDC_LOAD_CONFIG:
            load_selected_config();
            return 0;
        case IDC_DELETE_CONFIG:
            delete_selected_config();
            return 0;
        case IDC_CONFIG_LIST:
            if (HIWORD(wparam) == BN_CLICKED) {
                show_select_popup(reinterpret_cast<HWND>(lparam));
                return 0;
            }
            if (HIWORD(wparam) == CBN_SELCHANGE) {
                int index = combo_index(g_config_list);
                if (index >= 0 && index < static_cast<int>(g_saved_configs.size())) {
                    set_text(g_config_name, g_saved_configs[index].name.c_str());
                }
            }
            return 0;
        case IDC_MODE:
            if (HIWORD(wparam) == BN_CLICKED) {
                show_select_popup(reinterpret_cast<HWND>(lparam));
                return 0;
            }
            if (HIWORD(wparam) == CBN_SELCHANGE) refresh_custom_controls();
            return 0;
        case IDC_ACTION:
            if (HIWORD(wparam) == BN_CLICKED) {
                show_select_popup(reinterpret_cast<HWND>(lparam));
                return 0;
            }
            if (HIWORD(wparam) == CBN_SELCHANGE) refresh_action_controls();
            return 0;
        case IDC_SENSITIVITY:
            if (HIWORD(wparam) == BN_CLICKED) {
                show_select_popup(reinterpret_cast<HWND>(lparam));
                return 0;
            }
            return 0;
        case IDC_CUSTOM_R:
        case IDC_CUSTOM_G:
        case IDC_CUSTOM_B:
            if (HIWORD(wparam) == EN_CHANGE) InvalidateRect(g_preview, nullptr, TRUE);
            return 0;
        }
        break;

    case WM_LBUTTONDOWN:
        close_select_popup();
        return 0;

    case WM_HOTKEY:
        if (wparam == 1) toggle_worker();
        if (wparam == 2) stop_worker();
        return 0;

    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(hwnd, &pt);
        if (pt.y >= 0 && pt.y < 48) return HTCAPTION;
        return HTCLIENT;
    }

    case WM_MEASUREITEM:
        if (is_combo_control_id(static_cast<UINT>(wparam))) {
            reinterpret_cast<MEASUREITEMSTRUCT*>(lparam)->itemHeight = 30;
            return TRUE;
        }
        break;

    case WM_DRAWITEM:
        if (is_combo_control_id(static_cast<UINT>(wparam))) {
            draw_combo_item(reinterpret_cast<const DRAWITEMSTRUCT*>(lparam));
            return TRUE;
        }
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
    init_theme();
    WNDCLASSW wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = class_name;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = g_bg_brush;

    if (!RegisterClassW(&wc)) return 1;

    WNDCLASSW select_wc{};
    select_wc.lpfnWndProc = select_proc;
    select_wc.hInstance = instance;
    select_wc.lpszClassName = SELECT_CLASS_NAME;
    select_wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    select_wc.hbrBackground = g_bg_brush;
    if (!RegisterClassW(&select_wc)) return 1;

    WNDCLASSW popup_wc{};
    popup_wc.lpfnWndProc = popup_proc;
    popup_wc.hInstance = instance;
    popup_wc.lpszClassName = POPUP_CLASS_NAME;
    popup_wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    popup_wc.hbrBackground = g_input_brush;
    if (!RegisterClassW(&popup_wc)) return 1;

    g_main = CreateWindowExW(WS_EX_APPWINDOW, class_name, L"minhan-time",
                             WS_POPUP | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 695, 600,
                             nullptr, nullptr, instance, nullptr);
    if (!g_main) return 1;

    apply_dark_window_chrome(g_main);
    ShowWindow(g_main, show_cmd);
    UpdateWindow(g_main);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return 0;
}

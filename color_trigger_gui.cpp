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
    5. Save up to 5 named local configs.
    6. Click START.
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
    IDC_DELETE_CONFIG
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

static constexpr COLORREF COLOR_BG = RGB(9, 9, 11);
static constexpr COLORREF COLOR_SURFACE = RGB(17, 19, 24);
static constexpr COLORREF COLOR_ELEVATED = RGB(24, 27, 34);
static constexpr COLORREF COLOR_BORDER = RGB(42, 47, 58);
static constexpr COLORREF COLOR_INPUT = RGB(18, 21, 28);
static constexpr COLORREF COLOR_TEXT = RGB(245, 247, 250);
static constexpr COLORREF COLOR_MUTED = RGB(156, 163, 175);
static constexpr COLORREF COLOR_ACCENT = RGB(168, 85, 247);
static constexpr COLORREF COLOR_ACCENT_HOVER = RGB(192, 132, 252);
static constexpr COLORREF COLOR_DANGER = RGB(255, 91, 110);

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
    g_font = make_font(14, FW_NORMAL);
    g_font_bold = make_font(14, FW_SEMIBOLD);
    g_font_title = make_font(17, FW_BOLD);
    g_font_small = make_font(12, FW_NORMAL);
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
    SendMessageW(g_mode, CB_SETCURSEL, static_cast<int>(cfg.color_mode), 0);
    SendMessageW(g_action, CB_SETCURSEL, static_cast<int>(cfg.action_mode), 0);
    SendMessageW(g_sensitivity, CB_SETCURSEL, static_cast<int>(cfg.sensitivity), 0);
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
        SendMessageW(g_config_list, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(saved.name.c_str()));
    }
    if (!g_saved_configs.empty()) SendMessageW(g_config_list, CB_SETCURSEL, 0, 0);
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
    SendMessageW(g_config_list, CB_SETCURSEL, existing, 0);
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
    add_label(hwnd, L"Color", 44, 134, 90, 20);
    g_mode = add_combo(hwnd, IDC_MODE, 44, 158, 248, 150);
    combo_add(g_mode, L"Yellow outline");
    combo_add(g_mode, L"Red outline");
    combo_add(g_mode, L"Purple outline");
    combo_add(g_mode, L"Custom color");
    SendMessageW(g_mode, CB_SETCURSEL, 0, 0);

    g_preview = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                312, 158, 54, 30, hwnd, reinterpret_cast<HMENU>(IDC_PREVIEW),
                                GetModuleHandleW(nullptr), nullptr);

    add_label(hwnd, L"Key", 448, 134, 70, 20);
    g_key = add_edit(hwnd, IDC_KEY, 448, 158, 144, 30, L"F");

    add_label(hwnd, L"Action", 44, 204, 100, 20);
    g_action = add_combo(hwnd, IDC_ACTION, 44, 228, 248, 105);
    combo_add(g_action, L"Tap repeatedly");
    combo_add(g_action, L"Hold while visible");
    SendMessageW(g_action, CB_SETCURSEL, 0, 0);

    add_label(hwnd, L"Reaction", 448, 204, 100, 20);
    g_reaction_ms = add_edit(hwnd, IDC_REACTION_MS, 448, 228, 80, 30, L"0");
    add_label(hwnd, L"ms", 544, 234, 35, 18);

    add_label(hwnd, L"Tap interval", 44, 274, 100, 20);
    g_press_interval_ms = add_edit(hwnd, IDC_PRESS_INTERVAL_MS, 44, 298, 82, 30, L"25");
    add_label(hwnd, L"ms", 142, 304, 35, 18);

    add_label(hwnd, L"Detection", 200, 274, 95, 20);
    g_sensitivity = add_combo(hwnd, IDC_SENSITIVITY, 200, 298, 150, 110);
    combo_add(g_sensitivity, L"Strict");
    combo_add(g_sensitivity, L"Normal");
    combo_add(g_sensitivity, L"Loose");
    SendMessageW(g_sensitivity, CB_SETCURSEL, 1, 0);

    add_label(hwnd, L"Scan box", 448, 274, 100, 20);
    g_box_w = add_edit(hwnd, IDC_BOX_W, 448, 298, 60, 30, L"40");
    add_label(hwnd, L"x", 520, 304, 16, 18);
    g_box_h = add_edit(hwnd, IDC_BOX_H, 540, 298, 60, 30, L"40");

    add_label(hwnd, L"Custom RGB", 44, 344, 100, 20);
    g_custom_r = add_edit(hwnd, IDC_CUSTOM_R, 44, 368, 70, 30, L"255");
    g_custom_g = add_edit(hwnd, IDC_CUSTOM_G, 128, 368, 70, 30, L"230");
    g_custom_b = add_edit(hwnd, IDC_CUSTOM_B, 212, 368, 70, 30, L"0");
    g_pick_color = add_button(hwnd, IDC_PICK_COLOR, L"PICK", 304, 366, 86, 34);

    g_config_name = add_edit(hwnd, IDC_CONFIG_NAME, 196, 360, 128, 30, L"default");
    g_config_list = add_combo(hwnd, IDC_CONFIG_LIST, 336, 360, 136, 120);
    add_button(hwnd, IDC_SAVE_CONFIG, L"SAVE", 488, 358, 58, 34);
    add_button(hwnd, IDC_LOAD_CONFIG, L"LOAD", 558, 358, 58, 34);
    add_button(hwnd, IDC_DELETE_CONFIG, L"DEL", 628, 358, 48, 34);

    add_button(hwnd, IDC_START, L"START", 32, 424, 292, 50);
    add_button(hwnd, IDC_STOP, L"STOP", 348, 424, 292, 50);
    EnableWindow(GetDlgItem(hwnd, IDC_STOP), FALSE);

    g_status = add_label(hwnd, L"OFF     *     F8 start/stop     *     F9 stop", 194, 496, 330, 20);
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

static void stroke_round(HDC dc, RECT rect, int radius, COLORREF color, int width = 1) {
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
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

    RECT top_bar{0, 0, client.right, 56};
    fill_round(dc, top_bar, 0, RGB(10, 11, 16));

    RECT logo_ring{24, 18, 44, 38};
    stroke_round(dc, logo_ring, 20, COLOR_ACCENT, 3);
    RECT logo_dot{31, 25, 37, 31};
    fill_round(dc, logo_dot, 6, COLOR_ACCENT);
    RECT title{56, 14, 220, 42};
    draw_text(dc, L"minhan-time", title, g_font_bold, COLOR_TEXT, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    RECT panel_left{20, 72, 408, 328};
    RECT panel_right{424, 72, 644, 328};
    RECT panel_config{20, 344, 644, 400};
    fill_round(dc, panel_left, 12, COLOR_SURFACE);
    stroke_round(dc, panel_left, 12, COLOR_BORDER);
    fill_round(dc, panel_right, 12, COLOR_SURFACE);
    stroke_round(dc, panel_right, 12, COLOR_BORDER);
    fill_round(dc, panel_config, 12, COLOR_SURFACE);
    stroke_round(dc, panel_config, 12, COLOR_BORDER);

    RECT panel_title{44, 96, 280, 120};
    draw_text(dc, L"TRIGGER SETTINGS", panel_title, g_font_title, COLOR_ACCENT_HOVER, DT_LEFT | DT_SINGLELINE);
    RECT panel_title2{448, 96, 620, 120};
    draw_text(dc, L"SCAN SETTINGS", panel_title2, g_font_title, COLOR_ACCENT_HOVER, DT_LEFT | DT_SINGLELINE);
    RECT panel_title3{44, 364, 180, 388};
    draw_text(dc, L"SAVED CONFIGS", panel_title3, g_font_bold, COLOR_TEXT, DT_LEFT | DT_SINGLELINE);

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
    if (item->CtlID == IDC_START) {
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
            if (HIWORD(wparam) == CBN_SELCHANGE) {
                int index = combo_index(g_config_list);
                if (index >= 0 && index < static_cast<int>(g_saved_configs.size())) {
                    set_text(g_config_name, g_saved_configs[index].name.c_str());
                }
            }
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
    init_theme();
    WNDCLASSW wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = class_name;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = g_bg_brush;

    if (!RegisterClassW(&wc)) return 1;

    g_main = CreateWindowExW(0, class_name, L"minhan-time",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 695, 565,
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

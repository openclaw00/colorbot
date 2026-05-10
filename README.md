# minhan-time

`minhan-time` is a Windows color-trigger keyboard utility.

It watches a configurable box centered on the screen. When the selected color
range appears, it can either tap a key repeatedly or hold the key while the
color stays visible.

## Download

For Windows, use:

- `minhan-time.exe`
- or download and unzip `minhan-time_windows.zip`

## Features

- Dark, cleaned-up GUI
- Yellow, red, purple, and custom color modes
- Configurable key to press
- Tap repeatedly or hold the key while the color is visible
- Custom reaction delay in milliseconds
- Custom tap interval in milliseconds
- Detection strictness presets
- Custom scan box width and height
- F8 toggles start/stop
- F9 stops immediately

## Use

1. Run `minhan-time.exe` on Windows.
2. Pick a color mode.
3. Pick the key to press.
4. Pick `Tap repeatedly` or `Hold while visible`.
5. Click `START`.
6. Use `F8` to toggle on/off and `F9` to stop.

## Build

From a Visual Studio Developer Command Prompt:

```bat
cl /std:c++20 /O2 /EHsc /DUNICODE /D_UNICODE color_trigger_gui.cpp user32.lib gdi32.lib comdlg32.lib /link /SUBSYSTEM:WINDOWS
```

From macOS with `mingw-w64`:

```bash
x86_64-w64-mingw32-g++ -std=c++20 -O2 -municode -mwindows -static -static-libgcc -static-libstdc++ color_trigger_gui.cpp -o minhan-time.exe -luser32 -lgdi32 -lcomdlg32
```

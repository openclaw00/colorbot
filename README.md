# ColorBot

ColorBot is a simple Windows color-trigger keyboard utility.

It watches a small box in the center of the screen. When it sees the selected
color range, it presses the key you choose.

## Download

For Windows, use:

- `colorbot.exe`
- or download and unzip `colorbot_windows.zip`

## Features

- Yellow, red, purple, and custom color modes
- Configurable key to press
- Press-rate dropdown: 1/sec, 2/sec, 5/sec, 10/sec, or spam while visible
- Reaction speed presets
- Detection strictness presets
- Center box size presets
- F8 toggles start/stop
- F9 stops immediately

## Use

1. Run `colorbot.exe` on Windows.
2. Pick a color mode.
3. Pick the key to press.
4. Pick the press rate.
5. Click `START`.
6. Use `F8` to toggle on/off and `F9` to stop.

## Build

From a Visual Studio Developer Command Prompt:

```bat
cl /std:c++20 /O2 /EHsc /DUNICODE /D_UNICODE color_trigger_gui.cpp user32.lib gdi32.lib comdlg32.lib /link /SUBSYSTEM:WINDOWS
```

From macOS with `mingw-w64`:

```bash
x86_64-w64-mingw32-g++ -std=c++20 -O2 -municode -mwindows -static -static-libgcc -static-libstdc++ color_trigger_gui.cpp -o colorbot.exe -luser32 -lgdi32 -lcomdlg32
```

# CaveDash

CaveDash is a Windows arcade shooter written in C++ with the Win32 API and GDI+.
The game and its Aseprite sprite assets are included in this repository.

## Controls

- Move: arrow keys or `W`/`A`/`S`/`D`
- Fire: `Space`
- Start or restart: `Enter`
- Exit: `Esc`

## Build

From a Visual Studio Developer Command Prompt with the C++ and Windows SDK
workloads installed, run:

```bat
cl /nologo /std:c++17 /EHsc Main.cpp /link /SUBSYSTEM:WINDOWS gdiplus.lib gdi32.lib user32.lib /OUT:CaveDash.exe
```

Run the game from the project directory so it can find its sprite assets.

## Asset credits

Asset-pack credits and license information are included in the relevant asset
directories. Please retain those files when redistributing the assets.

# CaveDash

CaveDash is a Windows arcade shooter written in C++ with the Win32 API and GDI+.
The game and its Aseprite sprite assets are included in this repository.

## Controls

- Move: arrow keys or `W`/`A`/`S`/`D`
- Fire: `Space`
- Start or restart: `Enter`
- Exit: `Esc`

## Build

Install Visual Studio with the C++ and Windows SDK workloads, CMake, and
[vcpkg](https://github.com/microsoft/vcpkg). From PowerShell in the project
directory, configure and build a portable x64 release (set `VCPKG_ROOT` to
your vcpkg installation directory if it is not already set):

```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The vcpkg manifest installs zlib and the static triplet bundles the C/C++
runtime into the executable. Run `build\Release\CaveDash.exe` from the project
directory so it can find its sprite assets.

## Asset credits

Asset-pack credits and license information are included in the relevant asset
directories. Please retain those files when redistributing the assets.

cmake_minimum_required(VERSION 3.21)

cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(CaveDash VERSION 0.5 LANGUAGES CXX)

find_package(ZLIB REQUIRED)

add_executable(CaveDash WIN32 Main.cpp)
target_compile_features(CaveDash PRIVATE cxx_std_17)
target_compile_definitions(CaveDash PRIVATE NOMINMAX)
target_link_libraries(CaveDash PRIVATE ZLIB::ZLIB gdiplus gdi32 user32)

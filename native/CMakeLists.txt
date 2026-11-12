cmake_minimum_required(VERSION 3.24)
project(KillerGit VERSION 0.2.0 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
FetchContent_Declare(imgui
  URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.5.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(imgui json)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS UNICODE _UNICODE)

add_executable(KillerGit WIN32
  src/main.cpp src/app.cpp src/views.cpp src/dialogs.cpp src/theme.cpp
  src/util.cpp src/git.cpp src/vault.cpp src/ssh.cpp src/providers.cpp
  src/ghdesktop.cpp src/mcp.cpp
  res/app.rc)
target_include_directories(KillerGit PRIVATE src)
target_compile_definitions(KillerGit PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000006)
if(MSVC)
  set_property(TARGET KillerGit imgui PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_compile_options(KillerGit PRIVATE /W3 /utf-8 /EHsc)
else()
  # Clang / LLVM-MinGW: Unicode entry point (wWinMain) and a fully static exe (no libc++/winpthread DLLs).
  target_compile_options(KillerGit PRIVATE -Wall -Wno-unused-function -Wno-sign-compare)
  target_link_options(KillerGit PRIVATE -municode -static -mwindows)
  target_compile_options(imgui PRIVATE -w)
endif()
target_link_libraries(KillerGit PRIVATE imgui nlohmann_json::nlohmann_json
  d3d11 dxgi d3dcompiler crypt32 bcrypt winhttp shell32 ole32 uuid shcore dwmapi comdlg32 uxtheme imm32 windowscodecs)

cmake_minimum_required(VERSION 3.24)
project(KillerGit VERSION 0.2.0 LANGUAGES C CXX)

# Windows: Win32 + Direct3D 11.   Linux: GLFW 3.4 (Wayland and X11, chosen at runtime) + Vulkan.
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

set(IMGUI_CORE
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp)

set(APP_SOURCES
  src/app.cpp src/views.cpp src/home.cpp src/markdown.cpp src/dialogs.cpp src/theme.cpp
  src/util.cpp src/git.cpp src/vault.cpp src/ssh.cpp src/providers.cpp
  src/ghdesktop.cpp src/mcp.cpp)

if(WIN32)
  enable_language(RC)
  add_library(imgui STATIC ${IMGUI_CORE}
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
  target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp)
  target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS UNICODE _UNICODE)

  add_executable(KillerGit WIN32 ${APP_SOURCES} src/main_win.cpp src/util_win.cpp res/app.rc)
  target_include_directories(KillerGit PRIVATE src)
  target_compile_definitions(KillerGit PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000006)
  if(MSVC)
    set_property(TARGET KillerGit imgui PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(KillerGit PRIVATE /W3 /utf-8 /EHsc)
  else()
    # Clang / LLVM-MinGW: Unicode entry point (wWinMain) and a fully static exe (no libc++/winpthread DLLs).
    target_compile_options(KillerGit PRIVATE -Wall -Wno-unused-function -Wno-sign-compare -Wno-deprecated-this-capture)
    target_link_options(KillerGit PRIVATE -municode -static -mwindows)
    target_compile_options(imgui PRIVATE -w)
  endif()
  target_link_libraries(KillerGit PRIVATE imgui nlohmann_json::nlohmann_json
    d3d11 dxgi d3dcompiler crypt32 bcrypt winhttp shell32 ole32 uuid shcore dwmapi comdlg32 uxtheme imm32 windowscodecs)
else()
  # GLFW 3.4 compiled with both backends. It loads libwayland / libX11 / libxkbcommon at runtime, so one
  # binary runs natively on Wayland (GNOME, KDE, Sway, Hyprland…) and on X11 (or XWayland) without extra deps.
  set(GLFW_BUILD_WAYLAND ON CACHE BOOL "" FORCE)
  set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glfw
    URL https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(glfw)
  # stb_image (public domain), only to decode the embedded icon PNG.
  set(STB_DIR ${CMAKE_BINARY_DIR}/_deps/stb)
  if(NOT EXISTS ${STB_DIR}/stb_image.h)
    file(DOWNLOAD https://raw.githubusercontent.com/nothings/stb/f58f558c120e9b32c217290b80bad1a0729fbb2c/stb_image.h ${STB_DIR}/stb_image.h STATUS st)
  endif()

  find_package(Vulkan REQUIRED)
  find_package(CURL REQUIRED)
  find_package(OpenSSL REQUIRED)
  find_package(Threads REQUIRED)

  add_library(imgui STATIC ${IMGUI_CORE}
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
  target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp)
  target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
  target_link_libraries(imgui PUBLIC glfw Vulkan::Vulkan)
  target_compile_options(imgui PRIVATE -w)

  add_executable(KillerGit ${APP_SOURCES} src/main_linux.cpp src/util_posix.cpp src/resources_linux.cpp)
  target_include_directories(KillerGit PRIVATE src ${STB_DIR})
  # Fonts and icon are embedded with .incbin (see resources_linux.cpp).
  target_compile_definitions(KillerGit PRIVATE KG_RES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/res")
  set_source_files_properties(src/resources_linux.cpp PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/res/fonts/WorkSans-Regular.ttf;${CMAKE_CURRENT_SOURCE_DIR}/res/fonts/WorkSans-SemiBold.ttf;${CMAKE_CURRENT_SOURCE_DIR}/res/icon.png")
  target_compile_options(KillerGit PRIVATE -Wall -Wno-unused-function -Wno-sign-compare -Wno-deprecated -Wno-unused-variable)
  # Static C++ runtime so the binary runs on older distros than the build machine.
  target_link_options(KillerGit PRIVATE -static-libstdc++ -static-libgcc)
  target_link_libraries(KillerGit PRIVATE imgui nlohmann_json::nlohmann_json CURL::libcurl OpenSSL::Crypto Threads::Threads ${CMAKE_DL_LIBS})
endif()

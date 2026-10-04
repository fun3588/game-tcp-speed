cmake_minimum_required(VERSION 3.15)
project(GameTcpSpeed LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_C_STANDARD 99)

# 强制开启 Unicode 并指定源码字符集为 UTF-8
add_definitions(-DUNICODE -D_UNICODE)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-finput-charset=UTF-8)
endif()

# 统一输出目录到当前 c 目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# MinHook 源文件
set(MINHOOK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/minhook")
include_directories("${MINHOOK_DIR}/include")

set(MINHOOK_SOURCES
    "${MINHOOK_DIR}/src/buffer.c"
    "${MINHOOK_DIR}/src/hook.c"
    "${MINHOOK_DIR}/src/trampoline.c"
)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    list(APPEND MINHOOK_SOURCES "${MINHOOK_DIR}/src/hde/hde64.c")
else()
    list(APPEND MINHOOK_SOURCES "${MINHOOK_DIR}/src/hde/hde32.c")
endif()

# 1. 编译 Hook DLL
add_library(hook_proxy SHARED
    dll/hook_proxy.cpp
    ${MINHOOK_SOURCES}
)
set_target_properties(hook_proxy PROPERTIES PREFIX "")
target_link_libraries(hook_proxy PRIVATE ws2_32)

# 2. 编译 GUI 注入与启动器 (WIN32 子系统，无黑窗口)
add_executable(launcher_gui WIN32
    gui/launcher_gui.cpp
)
target_link_libraries(launcher_gui PRIVATE comctl32 comdlg32 shell32 user32 gdi32 kernel32)

cmake_minimum_required(VERSION 3.15)
project(GameTcpSpeed LANGUAGES C CXX RC)

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

# 1. MinHook 源文件
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

# 2. 编译 Hook DLL (hook_proxy.dll)
add_library(hook_proxy SHARED
    dll/hook_proxy.cpp
    ${MINHOOK_SOURCES}
)
set_target_properties(hook_proxy PROPERTIES PREFIX "")
target_link_libraries(hook_proxy PRIVATE ws2_32)

# 3. 编译 v3.0 纯经典 Win32 API + GDI 旗舰图形客户端 (GameTcpSpeed.exe, 带应用图标)
add_executable(GameTcpSpeed WIN32
    gui/launcher_gui.cpp
    app.rc
)
target_link_libraries(GameTcpSpeed PRIVATE 
    comctl32 
    comdlg32 
    shell32 
    user32 
    gdi32 
    kernel32 
    ws2_32 
    wininet 
    psapi
)

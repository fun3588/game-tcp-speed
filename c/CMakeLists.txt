cmake_minimum_required(VERSION 3.15)
project(GameTcpSpeed VERSION 3.1.5 LANGUAGES C CXX RC)

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
# 关闭 MinHook 的「冻结全部线程」：受保护宿主进程里逐个 SuspendThread 会让注入线程挂死。
# 详见 minhook/src/hook.c 中 MINHOOK_NO_FREEZE 处的说明。
target_compile_definitions(hook_proxy PRIVATE MINHOOK_NO_FREEZE)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # -static：静态链接 libstdc++/libgcc/winpthread，产物零外部运行时依赖，可直接注入游戏进程
    # -s    ：链接期剥离符号表（811 KB -> 316 KB）；调试崩溃时去掉 -s 重新构建以保留符号
    target_link_options(hook_proxy PRIVATE -static -static-libgcc -static-libstdc++ -s)
endif()

# 3. 编译 v3.1.5 纯经典 Win32 API + GDI 旗舰图形客户端 (GameTcpSpeed.exe, 带应用图标)
add_executable(GameTcpSpeed WIN32
    gui/launcher_gui.cpp
    gui/manual_map.cpp
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
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # 同上：静态自包含 + 链接期剥离，保证发布包体积与零依赖
    target_link_options(GameTcpSpeed PRIVATE -static -static-libgcc -static-libstdc++ -s)
endif()

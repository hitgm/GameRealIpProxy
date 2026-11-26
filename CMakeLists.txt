cmake_minimum_required(VERSION 3.15)
project(GameRealIpProxy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8 /W3)
endif()

# WinDivert SDK 路径配置
set(WINDIVERT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/windivert")

include_directories("${WINDIVERT_DIR}/include")
link_directories("${WINDIVERT_DIR}/x64")

add_executable(GameRealIpProxy
    src/main.cpp
)

target_link_libraries(GameRealIpProxy PRIVATE
    ws2_32
    WinDivert
)

# 编译完成后自动复制 config.ini 与 WinDivert 运行时文件到输出目录
add_custom_command(TARGET GameRealIpProxy POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_CONFIG=${CMAKE_CURRENT_SOURCE_DIR}/config.ini"
        "-DOUTPUT_CONFIG=$<TARGET_FILE_DIR:GameRealIpProxy>/config.ini"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyConfigIfMissing.cmake"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WINDIVERT_DIR}/x64/WinDivert.dll"
        "$<TARGET_FILE_DIR:GameRealIpProxy>/WinDivert.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WINDIVERT_DIR}/x64/WinDivert64.sys"
        "$<TARGET_FILE_DIR:GameRealIpProxy>/WinDivert64.sys"
)

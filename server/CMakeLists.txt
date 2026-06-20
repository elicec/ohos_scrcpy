cmake_minimum_required(VERSION 3.16)
project(ohos_scrcpy_server VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)

# 服务端桩编译 - 用于验证代码结构
# 实际编译需要 OpenHarmony 全量源码环境
# 使用方法: bash build_server.sh /path/to/openharmony

# 桩头文件目录(模拟 OpenHarmony API) - 优先于 include 目录
include_directories(
    ${CMAKE_SOURCE_DIR}/stubs
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/../common
)

set(SERVER_SOURCES
    src/main.c
    src/tcp_server.c
)

# 注意: screen_capture.c, video_encoder.c, input_injector.c
# 依赖 OpenHarmony SDK，在桩编译中跳过
# 实际编译时取消注释:
# set(SERVER_SOURCES
#     src/main.c
#     src/screen_capture.c
#     src/video_encoder.c
#     src/input_injector.c
#     src/tcp_server.c
# )

add_executable(${PROJECT_NAME} ${SERVER_SOURCES})

target_link_libraries(${PROJECT_NAME} pthread)

# 安装
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)

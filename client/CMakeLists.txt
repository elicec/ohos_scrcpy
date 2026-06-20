cmake_minimum_required(VERSION 3.16)
project(ohos_scrcpy VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# 平台设置
if(WIN32)
    add_definitions(-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN)
    add_definitions(-DNOMINMAX)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif()

# 使用 pkg-config 查找依赖
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec
    libavutil
    libswscale
)
pkg_check_modules(GLEW REQUIRED glew)

# 线程库
find_package(Threads REQUIRED)

# 客户端源文件
set(CLIENT_SOURCES
    src/main.c
    src/hdc_manager.c
    src/tcp_client.c
    src/renderer.c
    src/input_handler.c
    src/video_decoder.cpp
)

# 包含目录
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/../common
    ${SDL2_INCLUDE_DIRS}
    ${GLEW_INCLUDE_DIRS}
)

# 构建可执行文件
add_executable(${PROJECT_NAME} ${CLIENT_SOURCES})

target_link_libraries(${PROJECT_NAME}
    ${SDL2_LIBRARIES}
    ${GLEW_LIBRARIES}
    PkgConfig::FFMPEG
    Threads::Threads
)

# Linux 特定链接
if(UNIX AND NOT APPLE)
    target_link_libraries(${PROJECT_NAME} m)
endif()

# 链接目录
link_directories(${SDL2_LIBRARY_DIRS} ${GLEW_LIBRARY_DIRS})

# 安装
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)

# 复制服务端二进制到输出目录(如果存在)
if(EXISTS "${CMAKE_SOURCE_DIR}/../server/bin/ohos_scrcpy_server")
    configure_file(
        "${CMAKE_SOURCE_DIR}/../server/bin/ohos_scrcpy_server"
        "${CMAKE_BINARY_DIR}/ohos_scrcpy_server"
        COPYONLY
    )
endif()

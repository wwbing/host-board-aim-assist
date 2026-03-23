cmake_minimum_required(VERSION 3.16)

project(udp_latest_frame_receiver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(UDP_CLIENT_RECV_BUILD_APP "Build standalone latest-frame receiver app" ON)

find_package(PkgConfig REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc highgui)
find_package(spdlog CONFIG REQUIRED)

pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat
    libavcodec
    libavutil
    libswscale
)

add_library(udp_receiver_core
    src/receiver.cpp
    src/latest_frame_buffer.cpp
)

target_include_directories(udp_receiver_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../ffmpeg_video_infer/include
        ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(udp_receiver_core
    PUBLIC
        PkgConfig::FFMPEG
        spdlog::spdlog_header_only
        ${OpenCV_LIBS}
)

target_compile_options(udp_receiver_core
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

if (UDP_CLIENT_RECV_BUILD_APP)
    add_executable(udp_latest_frame_receiver
        src/main.cpp
    )

    target_link_libraries(udp_latest_frame_receiver
        PRIVATE
            udp_receiver_core
    )

    target_compile_options(udp_latest_frame_receiver
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )
endif()

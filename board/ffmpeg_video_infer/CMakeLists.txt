cmake_minimum_required(VERSION 3.16)

project(ffmpeg_video_infer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenCV REQUIRED COMPONENTS core imgproc highgui imgcodecs)
find_package(spdlog CONFIG REQUIRED)

set(UDP_CLIENT_RECV_BUILD_APP OFF CACHE BOOL "" FORCE)
set(INFER_V6_CPP_BUILD_APP OFF CACHE BOOL "" FORCE)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../udp_client_recv ${CMAKE_CURRENT_BINARY_DIR}/udp_client_recv_build)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../rknn_infer/infer_v6_cpp ${CMAKE_CURRENT_BINARY_DIR}/infer_v6_cpp_build)

add_library(ffmpeg_video_infer_lib
    src/clock_offset_server.cpp
    src/detection_udp_sender.cpp
    src/video_infer_app.cpp
)

target_include_directories(ffmpeg_video_infer_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann_json_pkg/usr/include
        ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(ffmpeg_video_infer_lib
    PUBLIC
        udp_receiver_core
        infer_v6_cpp_lib
        spdlog::spdlog_header_only
        ${OpenCV_LIBS}
)

target_compile_definitions(ffmpeg_video_infer_lib
    PUBLIC
        FFMPEG_VIDEO_INFER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_options(ffmpeg_video_infer_lib
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

add_executable(ffmpeg_video_infer
    src/main.cpp
)

target_link_libraries(ffmpeg_video_infer
    PRIVATE
        ffmpeg_video_infer_lib
)

target_compile_options(ffmpeg_video_infer
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

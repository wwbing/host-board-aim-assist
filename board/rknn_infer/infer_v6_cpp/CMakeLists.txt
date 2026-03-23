cmake_minimum_required(VERSION 3.16)

project(infer_v6_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(INFER_V6_CPP_BUILD_APP "Build standalone infer_v6_cpp demo app" ON)

find_package(OpenCV REQUIRED COMPONENTS core imgproc highgui imgcodecs)
find_package(spdlog CONFIG REQUIRED)

set(RKNN_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../rknn_model_zoo-2.3.2/3rdparty/rknpu2)
set(RKNN_INCLUDE_DIR ${RKNN_ROOT}/include)
set(RKNN_LIBRARY ${RKNN_ROOT}/Linux/aarch64/librknnrt.so)

add_library(infer_v6_cpp_lib
    src/yolov6_rknn.cpp
)

target_include_directories(infer_v6_cpp_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../ffmpeg_video_infer/include
        ${RKNN_INCLUDE_DIR}
        ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(infer_v6_cpp_lib
    PUBLIC
        ${OpenCV_LIBS}
        spdlog::spdlog_header_only
        ${RKNN_LIBRARY}
        dl
)

target_compile_definitions(infer_v6_cpp_lib
    PUBLIC
        INFER_V6_CPP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_options(infer_v6_cpp_lib
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

set_target_properties(infer_v6_cpp_lib PROPERTIES
    BUILD_RPATH "${RKNN_ROOT}/Linux/aarch64"
)

if (INFER_V6_CPP_BUILD_APP)
    add_executable(infer_v6_cpp_demo
        src/main.cpp
    )

    target_link_libraries(infer_v6_cpp_demo
        PRIVATE
            infer_v6_cpp_lib
    )

    target_compile_options(infer_v6_cpp_demo
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )
endif()

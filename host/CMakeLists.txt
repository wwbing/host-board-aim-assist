cmake_minimum_required(VERSION 3.20)
project(remote_box_receiver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(LOCAL_NLOHMANN_JSON_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann_json")
set(LOCAL_NLOHMANN_JSON_CONFIG
    "${LOCAL_NLOHMANN_JSON_ROOT}/share/nlohmann_json/nlohmann_jsonConfig.cmake")
set(LOCAL_FMT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fmt")
set(LOCAL_FMT_CONFIG "${LOCAL_FMT_ROOT}/share/fmt/fmt-config.cmake")
set(LOCAL_SPDLOG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/spdlog")
set(LOCAL_SPDLOG_CONFIG "${LOCAL_SPDLOG_ROOT}/share/spdlog/spdlogConfig.cmake")

if(NOT EXISTS "${LOCAL_NLOHMANN_JSON_CONFIG}")
    message(FATAL_ERROR
        "Missing vendored nlohmann_json package: ${LOCAL_NLOHMANN_JSON_CONFIG}\n"
        "Expected files under third_party/nlohmann_json.")
endif()
if(NOT EXISTS "${LOCAL_FMT_CONFIG}")
    message(FATAL_ERROR
        "Missing vendored fmt package: ${LOCAL_FMT_CONFIG}\n"
        "Expected files under third_party/fmt.")
endif()
if(NOT EXISTS "${LOCAL_SPDLOG_CONFIG}")
    message(FATAL_ERROR
        "Missing vendored spdlog package: ${LOCAL_SPDLOG_CONFIG}\n"
        "Expected files under third_party/spdlog.")
endif()

list(PREPEND CMAKE_PREFIX_PATH
    "${LOCAL_NLOHMANN_JSON_ROOT}"
    "${LOCAL_FMT_ROOT}"
    "${LOCAL_SPDLOG_ROOT}")

add_executable(remote_box_receiver
    src/main.cpp
    src/app.cpp
    src/logging/logger.cpp
    src/profiling/clock_offset_client.cpp
    src/profiling/timing_profiler.cpp
    src/sender/ffmpeg_command_builder.cpp
    src/sender/ffmpeg_process.cpp
    src/sender/ffmpeg_sender_service.cpp
    src/udp_receiver.cpp
    include/app.hpp
    include/logging/logger.hpp
    include/mouse_driver.hpp
    include/profiling/clock_offset_client.hpp
    include/profiling/timing_profiler.hpp
    include/sender/ffmpeg_command_builder.hpp
    include/sender/ffmpeg_process.hpp
    include/sender/ffmpeg_sender_options.hpp
    include/sender/ffmpeg_sender_service.hpp
    include/udp_receiver.hpp
    include/types.hpp
)

target_compile_definitions(remote_box_receiver PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

target_include_directories(remote_box_receiver PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
target_link_libraries(remote_box_receiver PRIVATE nlohmann_json::nlohmann_json)
target_link_libraries(remote_box_receiver PRIVATE spdlog::spdlog)

target_link_libraries(remote_box_receiver PRIVATE
    ws2_32
    user32
    gdi32
)

if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/runtime/ddll64.dll")
    add_custom_command(TARGET remote_box_receiver POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/ddll64.dll"
            "$<TARGET_FILE_DIR:remote_box_receiver>/ddll64.dll"
    )
endif()

if(WIN32)
    add_custom_command(TARGET remote_box_receiver POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:remote_box_receiver>
            $<TARGET_FILE_DIR:remote_box_receiver>
        COMMAND_EXPAND_LISTS
    )
endif()

if(MSVC)
    target_compile_options(remote_box_receiver PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(remote_box_receiver PRIVATE -Wall -Wextra -Wpedantic)
endif()

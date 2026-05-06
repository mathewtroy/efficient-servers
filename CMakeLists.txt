cmake_minimum_required(VERSION 3.20)
project(efficient_servers LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "No build type set — defaulting to Release")
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Protobuf REQUIRED)

protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS proto/server.proto)

add_executable(efficient_servers
    src/main.cpp
    src/server.cpp
    src/protocol.cpp
    src/fast_protocol.cpp
    src/graph_store.cpp
    ${PROTO_SRCS}
)

target_include_directories(efficient_servers PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${Protobuf_INCLUDE_DIRS}
)

target_link_libraries(efficient_servers PRIVATE
    protobuf::libprotobuf
    pthread
)

target_compile_options(efficient_servers PRIVATE
    $<$<CONFIG:Release>:-Ofast>
    $<$<CONFIG:Release>:-march=native>
    $<$<CONFIG:Release>:-flto=auto>
    $<$<CONFIG:Release>:-funroll-loops>
    $<$<CONFIG:Release>:-fomit-frame-pointer>
    $<$<CONFIG:Debug>:-O0>
    $<$<CONFIG:Debug>:-g>
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)

target_link_options(efficient_servers PRIVATE
    $<$<CONFIG:Release>:-flto=auto>
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)

add_executable(decode_responses
    decode_responses.cpp
    ${PROTO_SRCS}
)

target_include_directories(decode_responses PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${Protobuf_INCLUDE_DIRS}
)

target_link_libraries(decode_responses PRIVATE
    protobuf::libprotobuf
)

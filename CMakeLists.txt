cmake_minimum_required(VERSION 3.20)
project(efficient_servers LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Protobuf REQUIRED)

protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS proto/server.proto)

add_executable(efficient_servers
        src/main.cpp
        src/server.cpp
        src/protocol.cpp
        src/graph_store.cpp
        src/dijkstra.cpp
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
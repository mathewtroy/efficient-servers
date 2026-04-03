cmake_minimum_required(VERSION 3.20)
project(efficient_servers LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Protobuf REQUIRED)

include_directories(${Protobuf_INCLUDE_DIRS})

# protobuf generation
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS proto/server.proto)

add_executable(efficient_servers
        src/main.cpp
        src/server.cpp
        src/protocol.cpp
        src/graph_store.cpp
        src/dijkstra.cpp
        ${PROTO_SRCS}
        src/types.hpp
)

target_include_directories(efficient_servers PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
        src
)

target_link_libraries(efficient_servers PRIVATE
        protobuf::libprotobuf
        pthread
)
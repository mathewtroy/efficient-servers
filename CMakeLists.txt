cmake_minimum_required(VERSION 3.20)
project(efficient_servers LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Protobuf REQUIRED)

add_executable(efficient_servers
        src/main.cpp
        src/server.cpp
        src/protocol.cpp
        src/graph_store.cpp
        src/dijkstra.cpp
        src/protocol.hpp
        src/graph_store.hpp
        src/dijkstra.hpp
)

target_include_directories(efficient_servers PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_BINARY_DIR}
        ${Protobuf_INCLUDE_DIRS}
)

target_link_libraries(efficient_servers PRIVATE
        protobuf::libprotobuf
)

protobuf_generate(
        TARGET efficient_servers
        LANGUAGE cpp
        PROTOS proto/server.proto
)
# Efficient Servers

TCP + protobuf server for building a directed graph from vehicle walks and answering shortest-path queries.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Structure

efficient-servers/
├── .gitignore
├── README.md
├── CMakeLists.txt
├── proto/
│   └── server.proto
├── src/
│   ├── main.cpp
│   ├── server.hpp
│   ├── server.cpp
│   ├── protocol.hpp
│   ├── protocol.cpp
│   ├── graph_store.hpp
│   ├── graph_store.cpp
│   ├── dijkstra.hpp
│   ├── dijkstra.cpp
│   └── types.hpp
└── tests/
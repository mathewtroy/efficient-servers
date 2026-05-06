#pragma once

#include "server.pb.h"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

enum class FastKind {
    Unknown,
    Reset,
    Walk,
    OneToOne,
    OneToAll,
};

struct FastRequest {
    FastKind kind = FastKind::Unknown;
    std::vector<Point> locations;
    std::vector<uint32_t> lengths;
    Point origin{};
    Point destination{};
};

bool parse_fast_request(const uint8_t* data, std::size_t size, FastRequest& request);
bool parse_fast_walk_into(const uint8_t* data,
                          std::size_t size,
                          std::vector<Point>& locations,
                          std::vector<uint32_t>& lengths);

void append_empty_ok_frame(std::vector<uint8_t>& out);
void append_uint64_response_frame(std::vector<uint8_t>& out, uint32_t field, uint64_t value);
void append_serialized_response_frame(std::vector<uint8_t>& out, const Response& response);

bool write_raw(int fd, std::span<const uint8_t> data);

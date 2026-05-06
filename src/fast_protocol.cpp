#include "fast_protocol.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace {

bool read_varint(const uint8_t*& p, const uint8_t* end, uint64_t& value) {
    value = 0;
    unsigned shift = 0;

    while (p < end && shift <= 63) {
        const uint8_t byte = *p++;
        value |= static_cast<uint64_t>(byte & 0x7fU) << shift;

        if ((byte & 0x80U) == 0) {
            return true;
        }

        shift += 7;
    }

    return false;
}

bool read_varint32(const uint8_t*& p, const uint8_t* end, uint32_t& value) {
    uint64_t tmp = 0;
    if (!read_varint(p, end, tmp)) {
        return false;
    }

    value = static_cast<uint32_t>(tmp);
    return true;
}

bool skip_field(uint32_t wire_type, const uint8_t*& p, const uint8_t* end) {
    uint64_t value = 0;

    switch (wire_type) {
        case 0:
            return read_varint(p, end, value);

        case 1:
            if (end - p < 8) {
                return false;
            }
            p += 8;
            return true;

        case 2:
            if (!read_varint(p, end, value)) {
                return false;
            }
            if (static_cast<uint64_t>(end - p) < value) {
                return false;
            }
            p += value;
            return true;

        case 5:
            if (end - p < 4) {
                return false;
            }
            p += 4;
            return true;

        default:
            return false;
    }
}

bool parse_location(const uint8_t* p, const uint8_t* end, Point& point) {
    const uint8_t* original = p;

    if (p < end && *p == 0x08) {
        ++p;

        uint32_t x = 0;
        if (read_varint32(p, end, x) && p < end && *p == 0x10) {
            ++p;

            uint32_t y = 0;
            if (read_varint32(p, end, y) && p == end) {
                point.x = static_cast<int32_t>(x);
                point.y = static_cast<int32_t>(y);
                return true;
            }
        }
    }

    p = original;
    point = {};

    while (p < end) {
        uint64_t tag = 0;
        if (!read_varint(p, end, tag)) {
            return false;
        }

        const uint32_t field = static_cast<uint32_t>(tag >> 3);
        const uint32_t wire = static_cast<uint32_t>(tag & 7U);
        uint64_t value = 0;

        if ((field == 1 || field == 2) && wire == 0) {
            if (!read_varint(p, end, value)) {
                return false;
            }

            if (field == 1) {
                point.x = static_cast<int32_t>(value);
            } else {
                point.y = static_cast<int32_t>(value);
            }
        } else if (!skip_field(wire, p, end)) {
            return false;
        }
    }

    return true;
}

bool parse_walk(const uint8_t* p, const uint8_t* end, FastRequest& request) {
    request.kind = FastKind::Walk;
    request.locations.clear();
    request.lengths.clear();

    const auto size_hint = static_cast<std::size_t>(end - p);
    request.locations.reserve(size_hint / 8 + 1);
    request.lengths.reserve(size_hint / 8 + 1);

    while (p < end) {
        uint64_t tag = 0;
        if (!read_varint(p, end, tag)) {
            return false;
        }

        const uint32_t field = static_cast<uint32_t>(tag >> 3);
        const uint32_t wire = static_cast<uint32_t>(tag & 7U);
        uint64_t value = 0;

        if (field == 1 && wire == 2) {
            if (!read_varint(p, end, value)) {
                return false;
            }
            if (static_cast<uint64_t>(end - p) < value) {
                return false;
            }

            Point point;
            if (!parse_location(p, p + value, point)) {
                return false;
            }

            request.locations.push_back(point);
            p += value;
        } else if (field == 2 && wire == 2) {
            if (!read_varint(p, end, value)) {
                return false;
            }
            if (static_cast<uint64_t>(end - p) < value) {
                return false;
            }

            const uint8_t* packed_end = p + value;
            while (p < packed_end) {
                uint64_t length = 0;
                if (!read_varint(p, packed_end, length)) {
                    return false;
                }
                request.lengths.push_back(static_cast<uint32_t>(length));
            }
        } else if (field == 2 && wire == 0) {
            if (!read_varint(p, end, value)) {
                return false;
            }
            request.lengths.push_back(static_cast<uint32_t>(value));
        } else if (!skip_field(wire, p, end)) {
            return false;
        }
    }

    return true;
}

bool parse_walk_append(const uint8_t* p,
                       const uint8_t* end,
                       std::vector<Point>& locations,
                       std::vector<uint32_t>& lengths) {
    const auto size_hint = static_cast<std::size_t>(end - p);
    const std::size_t point_need = locations.size() + size_hint / 8 + 1;
    const std::size_t length_need = lengths.size() + size_hint / 8 + 1;
    if (locations.capacity() < point_need) {
        locations.reserve(std::max(point_need, locations.capacity() * 2 + 1024));
    }
    if (lengths.capacity() < length_need) {
        lengths.reserve(std::max(length_need, lengths.capacity() * 2 + 1024));
    }

    while (p < end) {
        uint64_t tag = 0;
        if (!read_varint(p, end, tag)) {
            return false;
        }

        const uint32_t field = static_cast<uint32_t>(tag >> 3);
        const uint32_t wire = static_cast<uint32_t>(tag & 7U);
        uint64_t value = 0;

        if (field == 1 && wire == 2) {
            if (!read_varint(p, end, value)) {
                return false;
            }
            if (static_cast<uint64_t>(end - p) < value) {
                return false;
            }

            Point point;
            if (!parse_location(p, p + value, point)) {
                return false;
            }

            locations.push_back(point);
            p += value;
        } else if (field == 2 && wire == 2) {
            if (!read_varint(p, end, value)) {
                return false;
            }
            if (static_cast<uint64_t>(end - p) < value) {
                return false;
            }

            const uint8_t* packed_end = p + value;
            while (p < packed_end) {
                uint64_t length = 0;
                if (!read_varint(p, packed_end, length)) {
                    return false;
                }
                lengths.push_back(static_cast<uint32_t>(length));
            }
        } else if (field == 2 && wire == 0) {
            if (!read_varint(p, end, value)) {
                return false;
            }
            lengths.push_back(static_cast<uint32_t>(value));
        } else if (!skip_field(wire, p, end)) {
            return false;
        }
    }

    return true;
}

bool parse_one_to_one(const uint8_t* p, const uint8_t* end, FastRequest& request) {
    request.kind = FastKind::OneToOne;
    request.origin = {};
    request.destination = {};

    while (p < end) {
        uint64_t tag = 0;
        if (!read_varint(p, end, tag)) {
            return false;
        }

        const uint32_t field = static_cast<uint32_t>(tag >> 3);
        const uint32_t wire = static_cast<uint32_t>(tag & 7U);
        uint64_t value = 0;

        if ((field == 1 || field == 2) && wire == 2) {
            if (!read_varint(p, end, value)) {
                return false;
            }
            if (static_cast<uint64_t>(end - p) < value) {
                return false;
            }

            Point point;
            if (!parse_location(p, p + value, point)) {
                return false;
            }

            if (field == 1) {
                request.origin = point;
            } else {
                request.destination = point;
            }

            p += value;
        } else if (!skip_field(wire, p, end)) {
            return false;
        }
    }

    return true;
}

bool parse_one_to_all(const uint8_t* p, const uint8_t* end, FastRequest& request) {
    request.kind = FastKind::OneToAll;
    request.origin = {};

    while (p < end) {
        uint64_t tag = 0;
        if (!read_varint(p, end, tag)) {
            return false;
        }

        const uint32_t field = static_cast<uint32_t>(tag >> 3);
        const uint32_t wire = static_cast<uint32_t>(tag & 7U);
        uint64_t value = 0;

        if (field == 1 && wire == 2) {
            if (!read_varint(p, end, value)) {
                return false;
            }
            if (static_cast<uint64_t>(end - p) < value) {
                return false;
            }

            if (!parse_location(p, p + value, request.origin)) {
                return false;
            }

            p += value;
        } else if (!skip_field(wire, p, end)) {
            return false;
        }
    }

    return true;
}

void append_be32(std::vector<uint8_t>& out, uint32_t value) {
    const uint32_t be = htonl(value);
    const auto* bytes = reinterpret_cast<const uint8_t*>(&be);
    out.insert(out.end(), bytes, bytes + 4);
}

void append_varint(std::vector<uint8_t>& out, uint64_t value) {
    while (value >= 0x80U) {
        out.push_back(static_cast<uint8_t>((value & 0x7fU) | 0x80U));
        value >>= 7U;
    }

    out.push_back(static_cast<uint8_t>(value));
}

}  

bool parse_fast_request(const uint8_t* data, std::size_t size, FastRequest& request) {
    const uint8_t* p = data;
    const uint8_t* end = p + size;

    request.kind = FastKind::Unknown;
    request.locations.clear();
    request.lengths.clear();
    request.origin = {};
    request.destination = {};

    while (p < end) {
        uint64_t tag = 0;
        if (!read_varint(p, end, tag)) {
            return false;
        }

        const uint32_t field = static_cast<uint32_t>(tag >> 3);
        const uint32_t wire = static_cast<uint32_t>(tag & 7U);
        uint64_t value = 0;

        if (field >= 1 && field <= 3 && wire == 2) {
            if (!read_varint(p, end, value)) {
                return false;
            }
            if (static_cast<uint64_t>(end - p) < value) {
                return false;
            }

            const uint8_t* message_end = p + value;

            bool ok = false;
            if (field == 1) {
                ok = parse_walk(p, message_end, request);
            } else if (field == 2) {
                ok = parse_one_to_one(p, message_end, request);
            } else {
                ok = parse_one_to_all(p, message_end, request);
            }

            if (!ok) {
                return false;
            }

            p = message_end;
        } else if (field == 4 && wire == 2) {
            if (!read_varint(p, end, value)) {
                return false;
            }
            if (static_cast<uint64_t>(end - p) < value) {
                return false;
            }

            request.kind = FastKind::Reset;
            p += value;
        } else if (!skip_field(wire, p, end)) {
            return false;
        }
    }

    return request.kind != FastKind::Unknown;
}

bool parse_fast_walk_into(const uint8_t* data,
                          std::size_t size,
                          std::vector<Point>& locations,
                          std::vector<uint32_t>& lengths) {
    const uint8_t* p = data;
    const uint8_t* end = p + size;

    bool saw_walk = false;

    while (p < end) {
        uint64_t tag = 0;
        if (!read_varint(p, end, tag)) {
            return false;
        }

        const uint32_t field = static_cast<uint32_t>(tag >> 3);
        const uint32_t wire = static_cast<uint32_t>(tag & 7U);
        uint64_t value = 0;

        if (field == 1 && wire == 2) {
            if (saw_walk) return false;
            if (!read_varint(p, end, value)) {
                return false;
            }
            if (static_cast<uint64_t>(end - p) < value) {
                return false;
            }

            if (!parse_walk_append(p, p + value, locations, lengths)) {
                return false;
            }

            p += value;
            saw_walk = true;
        } else if (field >= 2 && field <= 4) {
            return false;
        } else if (!skip_field(wire, p, end)) {
            return false;
        }
    }

    return saw_walk;
}

void append_empty_ok_frame(std::vector<uint8_t>& out) {
    const std::size_t old_size = out.size();
    out.resize(old_size + 4);
    std::memset(out.data() + old_size, 0, 4);
}

void append_uint64_response_frame(std::vector<uint8_t>& out, uint32_t field, uint64_t value) {
    std::vector<uint8_t> payload;
    payload.reserve(16);

    append_varint(payload, static_cast<uint64_t>(field << 3U));
    append_varint(payload, value);

    append_be32(out, static_cast<uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
}

void append_serialized_response_frame(std::vector<uint8_t>& out, const Response& response) {
    const auto size = static_cast<uint32_t>(response.ByteSizeLong());

    append_be32(out, size);

    const std::size_t old_size = out.size();
    out.resize(old_size + size);

    response.SerializeToArray(out.data() + old_size, static_cast<int>(size));
}

bool write_raw(int fd, std::span<const uint8_t> data) {
    std::size_t total = 0;

    while (total < data.size()) {
        const auto written = ::write(fd, data.data() + total, data.size() - total);

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }

            return false;
        }

        total += static_cast<std::size_t>(written);
    }

    return true;
}

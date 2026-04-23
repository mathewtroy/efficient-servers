#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>

constexpr int32_t LOCATION_MERGE_RADIUS_MM = 500;
constexpr int32_t GRID_CELL_SIZE_MM = 1000;
constexpr uint64_t INF_DISTANCE = (1ULL << 62);

struct Point {
    int32_t x{};
    int32_t y{};
};

struct EdgeStat {
    uint64_t length_sum{0};
    uint32_t sample_count{0};

    [[nodiscard]] uint32_t average_length() const {
        return sample_count == 0 ? 0U : static_cast<uint32_t>(length_sum / sample_count);
    }

    void add_sample(uint32_t length) {
        length_sum += length;
        ++sample_count;
    }
};

struct PairHash {
    template <typename T1, typename T2>
    std::size_t operator()(const std::pair<T1, T2>& value) const noexcept {
        const auto h1 = std::hash<T1>{}(value.first);
        const auto h2 = std::hash<T2>{}(value.second);
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6U) + (h1 >> 2U));
    }
};

inline int64_t pack_cell_key(int32_t cell_x, int32_t cell_y) {
    return (static_cast<int64_t>(cell_x) << 32) ^
           static_cast<uint32_t>(cell_y);
}

inline uint64_t squared_distance(const Point& a, const Point& b) {
    const int64_t dx = static_cast<int64_t>(a.x) - static_cast<int64_t>(b.x);
    const int64_t dy = static_cast<int64_t>(a.y) - static_cast<int64_t>(b.y);
    return static_cast<uint64_t>(dx * dx + dy * dy);
}

inline int32_t floor_div(int32_t value, int32_t divisor) {
    int32_t result = value / divisor;
    const int32_t remainder = value % divisor;
    if (remainder != 0 && ((remainder > 0) != (divisor > 0))) {
        --result;
    }
    return result;
}
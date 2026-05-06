#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class BufferedReader {
public:
    explicit BufferedReader(int fd, std::size_t buf_size = 131072)
        : fd_(fd), buf_(buf_size), head_(0), tail_(0) {}

    bool read_exact(void* out, std::size_t size);
    [[nodiscard]] bool has_buffered_data() const {
        return head_ < tail_;
    }

private:
    bool fill();

    int fd_;
    std::vector<uint8_t> buf_;
    std::size_t head_;
    std::size_t tail_;
};

class Protocol {
public:
    static bool read_message(BufferedReader& reader, std::vector<uint8_t>& buffer);
    static bool write_message(int fd, const void* data, std::size_t size);

private:
    static bool write_exact(int fd, const void* buffer, std::size_t size);
};

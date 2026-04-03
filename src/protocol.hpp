#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class Protocol {
public:
    static bool read_message(int fd, std::vector<uint8_t>& buffer);
    static bool write_message(int fd, const std::vector<uint8_t>& buffer);

private:
    static bool read_exact(int fd, void* buffer, std::size_t size);
    static bool write_exact(int fd, const void* buffer, std::size_t size);
};
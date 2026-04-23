#include "protocol.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstddef>
#include <unistd.h>

bool Protocol::read_exact(int fd, void* buffer, std::size_t size) {
    auto* ptr = static_cast<uint8_t*>(buffer);
    std::size_t total = 0;

    while (total < size) {
        const auto read_count = ::read(fd, ptr + total, size - total);

        if (read_count == 0) {
            return false;
        }
        if (read_count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }

        total += static_cast<std::size_t>(read_count);
    }

    return true;
}

bool Protocol::write_exact(int fd, const void* buffer, std::size_t size) {
    auto* ptr = static_cast<const uint8_t*>(buffer);
    std::size_t total = 0;

    while (total < size) {
        const auto write_count = ::write(fd, ptr + total, size - total);

        if (write_count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }

        total += static_cast<std::size_t>(write_count);
    }

    return true;
}

bool Protocol::read_message(int fd, std::vector<uint8_t>& buffer) {
    uint32_t length_be = 0;

    if (!read_exact(fd, &length_be, sizeof(length_be))) {
        return false;
    }

    const uint32_t length = ntohl(length_be);
    buffer.resize(length);

    if (length == 0) {
        return true;
    }

    return read_exact(fd, buffer.data(), length);
}

bool Protocol::write_message(int fd, const std::vector<uint8_t>& buffer) {
    const uint32_t length = static_cast<uint32_t>(buffer.size());
    const uint32_t length_be = htonl(length);

    if (!write_exact(fd, &length_be, sizeof(length_be))) {
        return false;
    }

    if (length == 0) {
        return true;
    }

    return write_exact(fd, buffer.data(), buffer.size());
}
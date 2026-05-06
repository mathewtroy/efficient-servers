#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

class FlatGrid {
public:
    static constexpr uint32_t INVALID_NODE = UINT32_MAX;

    void clear_and_reserve(std::size_t expected_entries) {
        size_ = 0;

        std::size_t capacity = 1;
        while (capacity < expected_entries * 2) {
            capacity <<= 1U;
        }

        table_.clear();
        table_.resize(std::max<std::size_t>(capacity, 16));
    }

    void clear() {
        for (Slot& slot : table_) {
            slot.used = false;
            slot.head = INVALID_NODE;
        }
        size_ = 0;
    }

    [[nodiscard]] const uint32_t* find(int64_t key) const {
        if (table_.empty()) return nullptr;

        const std::size_t mask = table_.size() - 1;
        std::size_t index = hash_key(key) & mask;

        while (true) {
            const Slot& slot = table_[index];
            if (!slot.used) return nullptr;
            if (slot.key == key) return &slot.head;
            index = (index + 1) & mask;
        }
    }

    [[nodiscard]] uint32_t add(int64_t key, uint32_t id) {
        if (table_.empty() || (size_ + 1) * 10 >= table_.size() * 7) {
            rehash(table_.empty() ? 16 : table_.size() * 2);
        }

        Slot& slot = find_or_insert(key);
        const uint32_t previous = slot.head;
        slot.head = id;
        return previous;
    }

private:
    struct Slot {
        int64_t key = 0;
        uint32_t head = INVALID_NODE;
        bool used = false;
    };

    static std::size_t hash_key(int64_t key) noexcept {
        uint64_t x = static_cast<uint64_t>(key);
        x = (x ^ (x >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27U)) * 0x94d049bb133111ebULL;
        return static_cast<std::size_t>(x ^ (x >> 31U));
    }

    Slot& find_or_insert(int64_t key) {
        const std::size_t mask = table_.size() - 1;
        std::size_t index = hash_key(key) & mask;

        while (true) {
            Slot& slot = table_[index];
            if (!slot.used) {
                slot.used = true;
                slot.key = key;
                slot.head = INVALID_NODE;
                ++size_;
                return slot;
            }
            if (slot.key == key) return slot;
            index = (index + 1) & mask;
        }
    }

    void rehash(std::size_t new_capacity) {
        std::size_t capacity = 1;
        while (capacity < new_capacity) {
            capacity <<= 1U;
        }

        std::vector<Slot> old;
        old.swap(table_);

        table_.clear();
        table_.resize(std::max<std::size_t>(capacity, 16));
        size_ = 0;

        for (Slot& slot : old) {
            if (!slot.used) continue;

            Slot& inserted = find_or_insert(slot.key);
            inserted.head = slot.head;
        }
    }

    std::vector<Slot> table_;
    std::size_t size_ = 0;
};

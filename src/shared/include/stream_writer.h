#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

/**
 * @brief Serializes data into a compact binary buffer readable by StreamReader.
 *
 * Layout: [data bytes][bit-packed bytes][uint16 bit-byte count].
 * The bit section grows independently; finish() appends it and the 2-byte footer.
 */
class StreamWriter
{
    std::vector<uint8_t> data_;
    std::vector<uint8_t> bits_;
    uint8_t cur_bit_byte_ = 0;
    uint8_t cur_bit_idx_  = 0;

public:
    explicit StreamWriter(size_t capacity = 256) { data_.reserve(capacity); }

    [[nodiscard]] std::vector<uint8_t> finish()
    {
        if (cur_bit_idx_ > 0)
            bits_.push_back(cur_bit_byte_);

        const auto bit_count = static_cast<uint16_t>(bits_.size());
        data_.insert(data_.end(), bits_.begin(), bits_.end());

        const uint8_t* footer = reinterpret_cast<const uint8_t*>(&bit_count);
        data_.insert(data_.end(), footer, footer + sizeof(uint16_t));
        return std::move(data_);
    }

    template<typename T>
    void write(T val)
    {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&val);
        data_.insert(data_.end(), p, p + sizeof(T));
    }

    void write_float(float val)
    {
        uint32_t bits;
        std::memcpy(&bits, &val, sizeof(bits));
        write<uint32_t>(bits);
    }

    void write_bool(bool val)
    {
        if (val) cur_bit_byte_ |= static_cast<uint8_t>(1U << cur_bit_idx_);
        if (++cur_bit_idx_ >= 8) {
            bits_.push_back(cur_bit_byte_);
            cur_bit_byte_ = 0;
            cur_bit_idx_  = 0;
        }
    }

    /// Writes [uint32 count][element...] using a per-element writer callable.
    template<typename Container, typename WriterFn>
    void write_array(const Container& container, WriterFn writer_fn)
    {
        write<uint32_t>(static_cast<uint32_t>(container.size()));
        for (const auto& item : container)
            writer_fn(*this, item);
    }

    template<typename T>
    void write_struct(const T& item) { item.serialize(*this); }
};

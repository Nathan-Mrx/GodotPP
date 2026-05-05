#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <vector>

/**
 * @brief Deserializes a binary buffer produced by StreamWriter.
 *
 * Layout expected: [data bytes][bit-packed bytes][uint16 bit-byte count].
 * The 2-byte footer is parsed on construction to split the byte and bit
 * regions. Read methods advance an internal cursor; nullopt signals
 * exhaustion or corruption.
 */
class StreamReader
{
    const uint8_t* data_;
    size_t         data_len_;
    const uint8_t* bits_;
    size_t         bits_len_;
    uint8_t cur_bit_byte_ = 0;
    uint8_t cur_bit_idx_  = 8; // 8 = no byte loaded

public:
    StreamReader(const uint8_t* buf, size_t len)
    {
        if (len < 2)
            throw std::runtime_error("[StreamReader] Buffer too short for footer");

        uint16_t bit_count = 0;
        std::memcpy(&bit_count, buf + len - 2, sizeof(uint16_t));

        if (len < static_cast<size_t>(2) + bit_count)
            throw std::runtime_error("[StreamReader] Corrupt footer");

        data_len_ = len - 2 - bit_count;
        data_     = buf;
        bits_     = buf + data_len_;
        bits_len_ = bit_count;
    }

    template<typename T>
    [[nodiscard]] std::optional<T> read()
    {
        if (data_len_ < sizeof(T)) return std::nullopt;
        T val;
        std::memcpy(&val, data_, sizeof(T));
        data_     += sizeof(T);
        data_len_ -= sizeof(T);
        return val;
    }

    [[nodiscard]] std::optional<float> read_float()
    {
        const auto raw = read<uint32_t>();
        if (!raw) return std::nullopt;
        float val;
        std::memcpy(&val, &*raw, sizeof(float));
        return val;
    }

    [[nodiscard]] std::optional<bool> read_bool()
    {
        if (cur_bit_idx_ >= 8) {
            if (bits_len_ == 0) return std::nullopt;
            cur_bit_byte_ = *bits_++;
            bits_len_--;
            cur_bit_idx_ = 0;
        }
        const bool val = (cur_bit_byte_ & (1U << cur_bit_idx_)) != 0;
        ++cur_bit_idx_;
        return val;
    }

    /// Reads a [uint32 count][element...] array using a per-element reader callable.
    template<typename T, typename ReaderFn>
    [[nodiscard]] std::optional<std::vector<T>> read_array(ReaderFn reader_fn)
    {
        const auto len = read<uint32_t>();
        if (!len) return std::nullopt;
        std::vector<T> vec;
        vec.reserve(*len);
        for (uint32_t i = 0; i < *len; ++i) {
            auto item = reader_fn(*this);
            if (!item) return std::nullopt;
            vec.push_back(std::move(*item));
        }
        return vec;
    }

    template<typename T>
    [[nodiscard]] std::optional<T> read_struct() { return T::deserialize(*this); }
};

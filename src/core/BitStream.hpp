#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/Math.hpp"

namespace coop
{
// Number of bits needed to store values in [0, aMaxValue].
constexpr int BitsRequired(uint64_t aMaxValue)
{
    int bits = 0;
    while (aMaxValue > 0)
    {
        ++bits;
        aMaxValue >>= 1;
    }
    return bits == 0 ? 1 : bits;
}

// Little-endian bit packer. Bits are appended LSB-first into 32-bit words.
class BitWriter
{
public:
    void WriteBits(uint32_t aValue, int aBits);
    void WriteU64(uint64_t aValue);
    void WriteBytes(const uint8_t* aData, size_t aSize);
    void AlignToByte();

    // Flushes the scratch word and returns the packed bytes (exact byte length).
    [[nodiscard]] std::vector<uint8_t> Finish();
    [[nodiscard]] size_t BitsWritten() const { return m_bitsWritten; }

private:
    std::vector<uint8_t> m_bytes;
    uint64_t m_scratch = 0;
    int m_scratchBits = 0;
    size_t m_bitsWritten = 0;
};

class BitReader
{
public:
    BitReader(const uint8_t* aData, size_t aSize);

    bool ReadBits(uint32_t& aValue, int aBits);
    bool ReadU64(uint64_t& aValue);
    bool ReadBytes(uint8_t* aOut, size_t aSize);
    bool AlignToByte();

    [[nodiscard]] size_t BitsRemaining() const { return m_totalBits - m_bitsRead; }
    [[nodiscard]] bool Failed() const { return m_failed; }

private:
    const uint8_t* m_data;
    size_t m_totalBits;
    size_t m_bitsRead = 0;
    bool m_failed = false;
};

// Quantization helpers shared by both stream directions.
uint32_t QuantizeFloat(float aValue, float aMin, float aMax, int aBits);
float DequantizeFloat(uint32_t aQuantized, float aMin, float aMax, int aBits);

// One Serialize() template per message works for both directions:
//   template<typename S> bool Serialize(S& s) { return s.U32(value) && s.Bool(flag); }
// Every method returns false on failure (overflow, out-of-range data), which aborts decoding.
class WriteStream
{
public:
    static constexpr bool kIsReading = false;

    bool Bits(uint32_t& aValue, int aBits);
    bool Bool(bool& aValue);
    bool U8(uint8_t& aValue);
    bool U16(uint16_t& aValue);
    bool U32(uint32_t& aValue);
    bool U64(uint64_t& aValue);
    bool I64(int64_t& aValue);
    bool IntRange(int32_t& aValue, int32_t aMin, int32_t aMax);
    bool Float(float& aValue);
    bool Quantized(float& aValue, float aMin, float aMax, int aBits);
    bool String(std::string& aValue, size_t aMaxLength);
    bool Blob(std::vector<uint8_t>& aValue, size_t aMaxLength);
    bool Vec(Vec3& aValue);

    template<size_t N>
    bool Array(std::array<uint8_t, N>& aValue)
    {
        m_writer.WriteBytes(aValue.data(), N);
        return true;
    }

    [[nodiscard]] std::vector<uint8_t> Finish() { return m_writer.Finish(); }

private:
    BitWriter m_writer;
};

class ReadStream
{
public:
    static constexpr bool kIsReading = true;

    ReadStream(const uint8_t* aData, size_t aSize);

    bool Bits(uint32_t& aValue, int aBits);
    bool Bool(bool& aValue);
    bool U8(uint8_t& aValue);
    bool U16(uint16_t& aValue);
    bool U32(uint32_t& aValue);
    bool U64(uint64_t& aValue);
    bool I64(int64_t& aValue);
    bool IntRange(int32_t& aValue, int32_t aMin, int32_t aMax);
    bool Float(float& aValue);
    bool Quantized(float& aValue, float aMin, float aMax, int aBits);
    bool String(std::string& aValue, size_t aMaxLength);
    bool Blob(std::vector<uint8_t>& aValue, size_t aMaxLength);
    bool Vec(Vec3& aValue);

    template<size_t N>
    bool Array(std::array<uint8_t, N>& aValue)
    {
        return m_reader.ReadBytes(aValue.data(), N);
    }

    [[nodiscard]] size_t BitsRemaining() const { return m_reader.BitsRemaining(); }

private:
    BitReader m_reader;
};
} // namespace coop

#include "core/BitStream.hpp"

#include <cmath>
#include <utility>

namespace coop
{
// ---------------------------------------------------------------------------------------------------------------------
// BitWriter

void BitWriter::WriteBits(uint32_t aValue, int aBits)
{
    if (aBits <= 0)
        return;
    if (aBits < 32)
        aValue &= (1u << aBits) - 1u;

    m_scratch |= static_cast<uint64_t>(aValue) << m_scratchBits;
    m_scratchBits += aBits;
    m_bitsWritten += static_cast<size_t>(aBits);

    while (m_scratchBits >= 8)
    {
        m_bytes.push_back(static_cast<uint8_t>(m_scratch & 0xFF));
        m_scratch >>= 8;
        m_scratchBits -= 8;
    }
}

void BitWriter::WriteU64(uint64_t aValue)
{
    WriteBits(static_cast<uint32_t>(aValue & 0xFFFFFFFFull), 32);
    WriteBits(static_cast<uint32_t>(aValue >> 32), 32);
}

void BitWriter::AlignToByte()
{
    const int pad = (8 - static_cast<int>(m_bitsWritten % 8)) % 8;
    WriteBits(0, pad);
}

void BitWriter::WriteBytes(const uint8_t* aData, size_t aSize)
{
    for (size_t i = 0; i < aSize; ++i)
        WriteBits(aData[i], 8);
}

std::vector<uint8_t> BitWriter::Finish()
{
    if (m_scratchBits > 0)
    {
        m_bytes.push_back(static_cast<uint8_t>(m_scratch & 0xFF));
        m_scratch = 0;
        m_scratchBits = 0;
    }
    return std::move(m_bytes);
}

// ---------------------------------------------------------------------------------------------------------------------
// BitReader

BitReader::BitReader(const uint8_t* aData, size_t aSize)
    : m_data(aData)
    , m_totalBits(aSize * 8)
{
}

bool BitReader::ReadBits(uint32_t& aValue, int aBits)
{
    aValue = 0;
    if (m_failed)
        return false;
    if (aBits <= 0)
        return true;
    if (aBits > 32 || m_bitsRead + static_cast<size_t>(aBits) > m_totalBits)
    {
        m_failed = true;
        return false;
    }

    uint64_t result = 0;
    for (int i = 0; i < aBits; ++i)
    {
        const size_t bitIndex = m_bitsRead + static_cast<size_t>(i);
        const uint8_t byte = m_data[bitIndex / 8];
        const uint64_t bit = (byte >> (bitIndex % 8)) & 1u;
        result |= bit << i;
    }
    m_bitsRead += static_cast<size_t>(aBits);
    aValue = static_cast<uint32_t>(result);
    return true;
}

bool BitReader::ReadU64(uint64_t& aValue)
{
    uint32_t low = 0;
    uint32_t high = 0;
    if (!ReadBits(low, 32) || !ReadBits(high, 32))
        return false;
    aValue = (static_cast<uint64_t>(high) << 32) | low;
    return true;
}

bool BitReader::ReadBytes(uint8_t* aOut, size_t aSize)
{
    for (size_t i = 0; i < aSize; ++i)
    {
        uint32_t value = 0;
        if (!ReadBits(value, 8))
            return false;
        aOut[i] = static_cast<uint8_t>(value);
    }
    return true;
}

bool BitReader::AlignToByte()
{
    const int pad = (8 - static_cast<int>(m_bitsRead % 8)) % 8;
    uint32_t ignored = 0;
    return ReadBits(ignored, pad);
}

// ---------------------------------------------------------------------------------------------------------------------
// Quantization

uint32_t QuantizeFloat(float aValue, float aMin, float aMax, int aBits)
{
    const uint32_t maxQ = aBits >= 32 ? 0xFFFFFFFFu : ((1u << aBits) - 1u);
    if (!std::isfinite(aValue))
        aValue = aMin;
    const float clamped = Clamp(aValue, aMin, aMax);
    const double normalized = (static_cast<double>(clamped) - aMin) / (static_cast<double>(aMax) - aMin);
    return static_cast<uint32_t>(std::llround(normalized * maxQ));
}

float DequantizeFloat(uint32_t aQuantized, float aMin, float aMax, int aBits)
{
    const uint32_t maxQ = aBits >= 32 ? 0xFFFFFFFFu : ((1u << aBits) - 1u);
    const double normalized = static_cast<double>(aQuantized) / maxQ;
    return static_cast<float>(aMin + normalized * (static_cast<double>(aMax) - aMin));
}

// ---------------------------------------------------------------------------------------------------------------------
// WriteStream

bool WriteStream::Bits(uint32_t& aValue, int aBits)
{
    m_writer.WriteBits(aValue, aBits);
    return true;
}

bool WriteStream::Bool(bool& aValue)
{
    m_writer.WriteBits(aValue ? 1u : 0u, 1);
    return true;
}

bool WriteStream::U8(uint8_t& aValue)
{
    m_writer.WriteBits(aValue, 8);
    return true;
}

bool WriteStream::U16(uint16_t& aValue)
{
    m_writer.WriteBits(aValue, 16);
    return true;
}

bool WriteStream::U32(uint32_t& aValue)
{
    m_writer.WriteBits(aValue, 32);
    return true;
}

bool WriteStream::U64(uint64_t& aValue)
{
    m_writer.WriteU64(aValue);
    return true;
}

bool WriteStream::I64(int64_t& aValue)
{
    m_writer.WriteU64(static_cast<uint64_t>(aValue));
    return true;
}

bool WriteStream::IntRange(int32_t& aValue, int32_t aMin, int32_t aMax)
{
    if (aValue < aMin || aValue > aMax)
        return false;
    const auto range = static_cast<uint64_t>(static_cast<int64_t>(aMax) - aMin);
    m_writer.WriteBits(static_cast<uint32_t>(static_cast<int64_t>(aValue) - aMin), BitsRequired(range));
    return true;
}

bool WriteStream::Float(float& aValue)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &aValue, sizeof(bits));
    m_writer.WriteBits(bits, 32);
    return true;
}

bool WriteStream::Quantized(float& aValue, float aMin, float aMax, int aBits)
{
    m_writer.WriteBits(QuantizeFloat(aValue, aMin, aMax, aBits), aBits);
    return true;
}

bool WriteStream::String(std::string& aValue, size_t aMaxLength)
{
    if (aValue.size() > aMaxLength)
        return false;
    m_writer.WriteBits(static_cast<uint32_t>(aValue.size()), BitsRequired(aMaxLength));
    m_writer.WriteBytes(reinterpret_cast<const uint8_t*>(aValue.data()), aValue.size());
    return true;
}

bool WriteStream::Blob(std::vector<uint8_t>& aValue, size_t aMaxLength)
{
    if (aValue.size() > aMaxLength)
        return false;
    m_writer.WriteBits(static_cast<uint32_t>(aValue.size()), BitsRequired(aMaxLength));
    m_writer.WriteBytes(aValue.data(), aValue.size());
    return true;
}

bool WriteStream::Vec(Vec3& aValue)
{
    return Float(aValue.x) && Float(aValue.y) && Float(aValue.z);
}

// ---------------------------------------------------------------------------------------------------------------------
// ReadStream

ReadStream::ReadStream(const uint8_t* aData, size_t aSize)
    : m_reader(aData, aSize)
{
}

bool ReadStream::Bits(uint32_t& aValue, int aBits)
{
    return m_reader.ReadBits(aValue, aBits);
}

bool ReadStream::Bool(bool& aValue)
{
    uint32_t value = 0;
    if (!m_reader.ReadBits(value, 1))
        return false;
    aValue = value != 0;
    return true;
}

bool ReadStream::U8(uint8_t& aValue)
{
    uint32_t value = 0;
    if (!m_reader.ReadBits(value, 8))
        return false;
    aValue = static_cast<uint8_t>(value);
    return true;
}

bool ReadStream::U16(uint16_t& aValue)
{
    uint32_t value = 0;
    if (!m_reader.ReadBits(value, 16))
        return false;
    aValue = static_cast<uint16_t>(value);
    return true;
}

bool ReadStream::U32(uint32_t& aValue)
{
    return m_reader.ReadBits(aValue, 32);
}

bool ReadStream::U64(uint64_t& aValue)
{
    return m_reader.ReadU64(aValue);
}

bool ReadStream::I64(int64_t& aValue)
{
    uint64_t value = 0;
    if (!m_reader.ReadU64(value))
        return false;
    aValue = static_cast<int64_t>(value);
    return true;
}

bool ReadStream::IntRange(int32_t& aValue, int32_t aMin, int32_t aMax)
{
    const auto range = static_cast<uint64_t>(static_cast<int64_t>(aMax) - aMin);
    uint32_t value = 0;
    if (!m_reader.ReadBits(value, BitsRequired(range)))
        return false;
    if (value > range)
        return false;
    aValue = static_cast<int32_t>(static_cast<int64_t>(value) + aMin);
    return true;
}

bool ReadStream::Float(float& aValue)
{
    uint32_t bits = 0;
    if (!m_reader.ReadBits(bits, 32))
        return false;
    std::memcpy(&aValue, &bits, sizeof(bits));
    return std::isfinite(aValue);
}

bool ReadStream::Quantized(float& aValue, float aMin, float aMax, int aBits)
{
    uint32_t value = 0;
    if (!m_reader.ReadBits(value, aBits))
        return false;
    aValue = DequantizeFloat(value, aMin, aMax, aBits);
    return true;
}

bool ReadStream::String(std::string& aValue, size_t aMaxLength)
{
    uint32_t length = 0;
    if (!m_reader.ReadBits(length, BitsRequired(aMaxLength)) || length > aMaxLength)
        return false;
    if (static_cast<size_t>(length) * 8 > m_reader.BitsRemaining())
        return false;
    aValue.resize(length);
    return m_reader.ReadBytes(reinterpret_cast<uint8_t*>(aValue.data()), length);
}

bool ReadStream::Blob(std::vector<uint8_t>& aValue, size_t aMaxLength)
{
    uint32_t length = 0;
    if (!m_reader.ReadBits(length, BitsRequired(aMaxLength)) || length > aMaxLength)
        return false;
    if (static_cast<size_t>(length) * 8 > m_reader.BitsRemaining())
        return false;
    aValue.resize(length);
    return m_reader.ReadBytes(aValue.data(), length);
}

bool ReadStream::Vec(Vec3& aValue)
{
    return Float(aValue.x) && Float(aValue.y) && Float(aValue.z);
}
} // namespace coop

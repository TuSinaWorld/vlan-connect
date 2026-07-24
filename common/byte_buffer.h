#ifndef VLAN_BYTE_BUFFER_H
#define VLAN_BYTE_BUFFER_H

#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace VLan {

class ByteBufferReadError : public std::runtime_error {
public:
    explicit ByteBufferReadError(const char* message)
        : std::runtime_error(message) {}
};

class ByteBuffer {
public:
    ByteBuffer() : m_readPos(0) {}

    ByteBuffer(const uint8_t* d, size_t len)
        : m_readPos(0) {
        if (len == 0) return;
        if (d == nullptr)
            throw ByteBufferReadError("ByteBuffer: null input");
        m_data.assign(d, d + len);
    }

    ByteBuffer(const char* d, size_t len)
        : m_readPos(0) {
        if (len == 0) return;
        if (d == nullptr)
            throw ByteBufferReadError("ByteBuffer: null input");
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(d);
        m_data.assign(bytes, bytes + len);
    }

    void writeU8(uint8_t v) { m_data.push_back(v); }

    void writeU16(uint16_t v) {
        m_data.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        m_data.push_back(static_cast<uint8_t>(v & 0xFF));
    }

    void writeU32(uint32_t v) {
        m_data.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
        m_data.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        m_data.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        m_data.push_back(static_cast<uint8_t>(v & 0xFF));
    }

    void writeString(const std::string& s) {
        writeU16(static_cast<uint16_t>(s.size()));
        m_data.insert(m_data.end(), s.begin(), s.end());
    }

    void writeBytes(const void* d, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(d);
        m_data.insert(m_data.end(), p, p + len);
    }

    uint8_t readU8() {
        ensureReadable(1);
        return m_data[m_readPos++];
    }

    uint16_t readU16() {
        ensureReadable(2);
        uint16_t v = (static_cast<uint16_t>(m_data[m_readPos]) << 8) |
                      m_data[m_readPos + 1];
        m_readPos += 2;
        return v;
    }

    uint32_t readU32() {
        ensureReadable(4);
        uint32_t v = (static_cast<uint32_t>(m_data[m_readPos])     << 24) |
                     (static_cast<uint32_t>(m_data[m_readPos + 1]) << 16) |
                     (static_cast<uint32_t>(m_data[m_readPos + 2]) << 8)  |
                      m_data[m_readPos + 3];
        m_readPos += 4;
        return v;
    }

    std::string readString() {
        uint16_t len = readU16();
        ensureReadable(len);
        std::string s(m_data.begin() + m_readPos,
                      m_data.begin() + m_readPos + len);
        m_readPos += len;
        return s;
    }

    void readBytes(void* out, size_t len) {
        ensureReadable(len);
        std::memcpy(out, m_data.data() + m_readPos, len);
        m_readPos += len;
    }

    const uint8_t* data()    const { return m_data.data(); }
    size_t         size()    const { return m_data.size(); }
    size_t     remaining()   const { return m_data.size() - m_readPos; }
    bool         atEnd()     const { return m_readPos >= m_data.size(); }
    void         clear()           { m_data.clear(); m_readPos = 0; }

private:
    void ensureReadable(size_t n) const {
        if (n > remaining())
            throw ByteBufferReadError("ByteBuffer: read beyond end");
    }

    std::vector<uint8_t> m_data;
    size_t               m_readPos;
};

} // namespace VLan
#endif // VLAN_BYTE_BUFFER_H

#ifndef VLAN_CLI_COMMON_H
#define VLAN_CLI_COMMON_H

#include "protocol.h"
#include "byte_buffer.h"
#include "net_common.h"
#include "payload_cipher.h"
#include "secure_frame.h"

#include <vector>
#include <string>
#include <map>
#include <set>
#include <functional>
#include <mutex>
#include <atomic>
#include <thread>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace VLan {

typedef std::vector<uint8_t> Buffer;

inline Buffer toBuffer(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    return Buffer(p, p + len);
}

inline Buffer toBuffer(const std::string& s) {
    return Buffer(s.begin(), s.end());
}

inline std::string virtualIPStr(uint32_t ip) {
    return ipToString(ip);
}

template<typename T>
class ThreadSafeQueue {
public:
    void push(const T& item) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back(item);
    }
    void push(T&& item) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back(std::move(item));
    }
    bool tryPop(T& out) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty()) return false;
        out = std::move(m_queue.front());
        m_queue.erase(m_queue.begin());
        return true;
    }
    std::vector<T> popAll() {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<T> result;
        result.swap(m_queue);
        return result;
    }
    bool empty() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.empty();
    }
private:
    mutable std::mutex m_mutex;
    std::vector<T> m_queue;
};

struct TcpConnection {
    socket_t   fd;
    Buffer     recvBuf;
    Buffer     sendBuf;
    bool       connected;
    bool       connecting;
    uint32_t   lastRecvTime;

    TcpConnection() : fd(SOCK_INVALID), connected(false), connecting(false), lastRecvTime(0) {}

    void reset() {
        if (fd != SOCK_INVALID) { sock_close(fd); fd = SOCK_INVALID; }
        recvBuf.clear();
        sendBuf.clear();
        connected = false;
        connecting = false;
        lastRecvTime = 0;
    }

    bool connectTo(const std::string& ip, uint16_t port) {
        reset();
        fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd == SOCK_INVALID) return false;
        setNonBlocking(fd);
        setTcpNoDelay(fd);

        struct sockaddr_in addr = makeAddr(ip, port);
        int rc = ::connect(fd, (struct sockaddr*)&addr, sizeof(addr));
#ifdef _WIN32
        if (rc == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
#else
        if (rc < 0 && errno != EINPROGRESS) {
#endif
            sock_close(fd);
            fd = SOCK_INVALID;
            return false;
        }
        connecting = true;
        return true;
    }

    bool flushSend() {
        if (fd == SOCK_INVALID || sendBuf.empty()) return true;
        int sent = ::send(fd, reinterpret_cast<const char*>(sendBuf.data()),
                          static_cast<int>(sendBuf.size()), 0);
        if (sent > 0) {
            sendBuf.erase(sendBuf.begin(), sendBuf.begin() + sent);
            return true;
        }
#ifdef _WIN32
        if (sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) return true;
#else
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
#endif
        return false;
    }

    void queueSend(const void* data, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        sendBuf.insert(sendBuf.end(), p, p + len);
    }

    bool sendTcpMsg(uint8_t msgType, const ByteBuffer& body) {
        if (body.size() > MAX_TCP_MSG_PAYLOAD) return false;
        TcpMsgHeader hdr;
        hdr.msgType = msgType;
        hdr.length  = htons(static_cast<uint16_t>(body.size()));
        queueSend(&hdr, sizeof(hdr));
        if (body.size() > 0)
            queueSend(body.data(), body.size());
        return true;
    }

    bool sendTcpMsg(uint8_t msgType) {
        TcpMsgHeader hdr;
        hdr.msgType = msgType;
        hdr.length  = 0;
        queueSend(&hdr, sizeof(hdr));
        return true;
    }

    int readData() {
        char tmp[65536];
        int n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n > 0) {
            recvBuf.insert(recvBuf.end(), tmp, tmp + n);
            lastRecvTime = currentTimeMs();
        }
        return n;
    }
};

} // namespace VLan
#endif // VLAN_CLI_COMMON_H

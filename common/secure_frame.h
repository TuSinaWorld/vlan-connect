#ifndef VLAN_SECURE_FRAME_H
#define VLAN_SECURE_FRAME_H

#include "protocol.h"
#include "byte_buffer.h"
#include "payload_cipher.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#ifdef _MSC_VER
#pragma comment(lib, "bcrypt.lib")
#endif
#else
#include <fcntl.h>
#include <unistd.h>
#endif

extern "C" {
#include "monocypher.h"
}

namespace VLan {

static const int SECURE_KEY_SIZE      = 32;
static const int SECURE_NONCE_SIZE    = 24;
static const int SECURE_MAC_SIZE      = 16;
static const int SECURE_COUNTER_SIZE  = 8;
static const int SECURE_SESSION_ID_SIZE = 4;
static const int SECURE_FRAME_OVERHEAD =
    SECURE_COUNTER_SIZE + SECURE_MAC_SIZE;
static const int SERVER_AUTH_TIMEOUT_SEC = 120;

inline bool secureRandomSystem(uint8_t* out, size_t len) {
    if (!out && len != 0) return false;
    uint8_t* const outputStart = out;
    const size_t outputLength = len;
#ifdef _WIN32
    while (len > 0) {
        ULONG chunk = len > 0x7fffffffUL ? 0x7fffffffUL : static_cast<ULONG>(len);
        if (BCryptGenRandom(nullptr, out, chunk,
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            if (outputStart && outputLength)
                std::memset(outputStart, 0, outputLength);
            return false;
        }
        out += chunk;
        len -= chunk;
    }
    return true;
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        if (outputStart && outputLength)
            std::memset(outputStart, 0, outputLength);
        return false;
    }
    while (len > 0) {
        ssize_t n = read(fd, out, len);
        if (n <= 0) {
            close(fd);
            if (outputStart && outputLength)
                std::memset(outputStart, 0, outputLength);
            return false;
        }
        out += n;
        len -= static_cast<size_t>(n);
    }
    close(fd);
    return true;
#endif
}

typedef bool (*SecureRandomProvider)(uint8_t*, size_t);

inline bool secureRandomBytesWithProvider(uint8_t* out, size_t len,
                                          SecureRandomProvider provider) {
    if (provider && provider(out, len)) return true;
    if (out && len) std::memset(out, 0, len);
    return false;
}

inline bool secureRandomBytes(uint8_t* out, size_t len) {
    return secureRandomBytesWithProvider(out, len, &secureRandomSystem);
}

inline uint32_t readU32BE(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
            static_cast<uint32_t>(p[3]);
}

inline uint64_t readU64BE(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | static_cast<uint64_t>(p[i]);
    return v;
}

inline void writeU64BE(uint8_t* p, uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        p[i] = static_cast<uint8_t>(v & 0xFF);
        v >>= 8;
    }
}

inline void writeU32BE(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(v & 0xFF);
}

inline std::vector<uint8_t> secureHashParts(const char* domain,
                                            const uint8_t* a, size_t aLen,
                                            const uint8_t* b = nullptr, size_t bLen = 0,
                                            const uint8_t* c = nullptr, size_t cLen = 0,
                                            const uint8_t* d = nullptr, size_t dLen = 0)
{
    std::vector<uint8_t> msg;
    size_t domainLen = std::strlen(domain);
    msg.insert(msg.end(), domain, domain + domainLen);
    if (a && aLen) msg.insert(msg.end(), a, a + aLen);
    if (b && bLen) msg.insert(msg.end(), b, b + bLen);
    if (c && cLen) msg.insert(msg.end(), c, c + cLen);
    if (d && dLen) msg.insert(msg.end(), d, d + dLen);
    std::vector<uint8_t> out(SECURE_KEY_SIZE);
    crypto_blake2b(out.data(), out.size(), msg.data(), msg.size());
    return out;
}

inline void deriveSecureKey(uint8_t out[SECURE_KEY_SIZE],
                            const uint8_t master[SECURE_KEY_SIZE],
                            const char* domain)
{
    crypto_blake2b_keyed(out, SECURE_KEY_SIZE,
                         master, SECURE_KEY_SIZE,
                         reinterpret_cast<const uint8_t*>(domain),
                         std::strlen(domain));
}

inline uint32_t deriveSessionId(const uint8_t master[SECURE_KEY_SIZE]) {
    uint8_t tmp[SECURE_KEY_SIZE];
    deriveSecureKey(tmp, master, "VLan-session-id");
    uint32_t sid = readU32BE(tmp);
    crypto_wipe(tmp, sizeof(tmp));
    if (sid == 0) sid = 1;
    return sid;
}

inline void deriveSecureMaster(uint8_t out[SECURE_KEY_SIZE],
                               const uint8_t sharedSecret[SECURE_KEY_SIZE],
                               const uint8_t authHash[SECURE_KEY_SIZE],
                               const uint8_t clientNonce[16],
                               const uint8_t serverNonce[16],
                               const uint8_t clientPub[32],
                               const uint8_t serverPub[32])
{
    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, SECURE_KEY_SIZE);
    const char domain[] = "VLan-secure-master-v1";
    crypto_blake2b_update(&ctx, reinterpret_cast<const uint8_t*>(domain), sizeof(domain) - 1);
    crypto_blake2b_update(&ctx, sharedSecret, SECURE_KEY_SIZE);
    crypto_blake2b_update(&ctx, authHash, SECURE_KEY_SIZE);
    crypto_blake2b_update(&ctx, clientNonce, 16);
    crypto_blake2b_update(&ctx, serverNonce, 16);
    crypto_blake2b_update(&ctx, clientPub, 32);
    crypto_blake2b_update(&ctx, serverPub, 32);
    crypto_blake2b_final(&ctx, out);
}

inline void computeClientAuthProof(uint8_t out[SECURE_KEY_SIZE],
                                   const uint8_t master[SECURE_KEY_SIZE],
                                   const uint8_t authHash[SECURE_KEY_SIZE])
{
    crypto_blake2b_keyed(out, SECURE_KEY_SIZE,
                         authHash, SECURE_KEY_SIZE,
                         master, SECURE_KEY_SIZE);
}

inline void computeServerAuthProof(uint8_t out[SECURE_KEY_SIZE],
                                   const uint8_t master[SECURE_KEY_SIZE],
                                   const uint8_t authHash[SECURE_KEY_SIZE])
{
    const char domain[] = "VLan-server-proof";
    crypto_blake2b_ctx ctx;
    crypto_blake2b_keyed_init(&ctx, SECURE_KEY_SIZE, authHash, SECURE_KEY_SIZE);
    crypto_blake2b_update(&ctx, reinterpret_cast<const uint8_t*>(domain), sizeof(domain) - 1);
    crypto_blake2b_update(&ctx, master, SECURE_KEY_SIZE);
    crypto_blake2b_final(&ctx, out);
}

class SecureFrameCipher {
public:
    SecureFrameCipher()
        : m_sendCounter(0), m_recvMaxCounter(0), m_recvStarted(false)
    {
        std::memset(m_sendKey, 0, sizeof(m_sendKey));
        std::memset(m_recvKey, 0, sizeof(m_recvKey));
        std::memset(m_sendNonceSeed, 0, sizeof(m_sendNonceSeed));
        std::memset(m_recvNonceSeed, 0, sizeof(m_recvNonceSeed));
        std::memset(m_replayBitmap, 0, sizeof(m_replayBitmap));
    }

    ~SecureFrameCipher() {
        reset();
    }

    void reset() {
        crypto_wipe(m_sendKey, sizeof(m_sendKey));
        crypto_wipe(m_recvKey, sizeof(m_recvKey));
        crypto_wipe(m_sendNonceSeed, sizeof(m_sendNonceSeed));
        crypto_wipe(m_recvNonceSeed, sizeof(m_recvNonceSeed));
        crypto_wipe(m_replayBitmap, sizeof(m_replayBitmap));
        m_sendCounter = 0;
        m_recvMaxCounter = 0;
        m_recvStarted = false;
    }

    void init(const uint8_t master[SECURE_KEY_SIZE], bool clientSide,
              const char* context = "signal") {
        reset();
        uint8_t c2s[SECURE_KEY_SIZE], s2c[SECURE_KEY_SIZE];
        uint8_t c2sNonce[SECURE_KEY_SIZE], s2cNonce[SECURE_KEY_SIZE];
        std::string prefix = std::string("VLan-") + (context ? context : "signal");
        std::string c2sKeyDomain = prefix + "-c2s-key";
        std::string s2cKeyDomain = prefix + "-s2c-key";
        std::string c2sNonceDomain = prefix + "-c2s-nonce";
        std::string s2cNonceDomain = prefix + "-s2c-nonce";
        deriveSecureKey(c2s, master, c2sKeyDomain.c_str());
        deriveSecureKey(s2c, master, s2cKeyDomain.c_str());
        deriveSecureKey(c2sNonce, master, c2sNonceDomain.c_str());
        deriveSecureKey(s2cNonce, master, s2cNonceDomain.c_str());
        if (clientSide) {
            std::memcpy(m_sendKey, c2s, SECURE_KEY_SIZE);
            std::memcpy(m_recvKey, s2c, SECURE_KEY_SIZE);
            std::memcpy(m_sendNonceSeed, c2sNonce, 16);
            std::memcpy(m_recvNonceSeed, s2cNonce, 16);
        } else {
            std::memcpy(m_sendKey, s2c, SECURE_KEY_SIZE);
            std::memcpy(m_recvKey, c2s, SECURE_KEY_SIZE);
            std::memcpy(m_sendNonceSeed, s2cNonce, 16);
            std::memcpy(m_recvNonceSeed, c2sNonce, 16);
        }
        m_sendCounter = 0;
        m_recvMaxCounter = 0;
        m_recvStarted = false;
        std::memset(m_replayBitmap, 0, sizeof(m_replayBitmap));
        crypto_wipe(c2s, sizeof(c2s));
        crypto_wipe(s2c, sizeof(s2c));
        crypto_wipe(c2sNonce, sizeof(c2sNonce));
        crypto_wipe(s2cNonce, sizeof(s2cNonce));
    }

    std::vector<uint8_t> encrypt(const uint8_t* plain, size_t plainLen,
                                 const uint8_t* ad = nullptr, size_t adLen = 0)
    {
        std::vector<uint8_t> out(SECURE_COUNTER_SIZE + plainLen + SECURE_MAC_SIZE);
        uint64_t ctr = m_sendCounter++;
        writeU64BE(out.data(), ctr);
        uint8_t nonce[SECURE_NONCE_SIZE];
        buildNonce(nonce, m_sendNonceSeed, ctr);
        uint8_t* cipher = out.data() + SECURE_COUNTER_SIZE;
        uint8_t* mac = cipher + plainLen;
        crypto_aead_lock(cipher, mac, m_sendKey, nonce, ad, adLen, plain, plainLen);
        return out;
    }

    bool decrypt(const uint8_t* frame, size_t frameLen,
                 std::vector<uint8_t>* plain,
                 const uint8_t* ad = nullptr, size_t adLen = 0)
    {
        if (frameLen < SECURE_FRAME_OVERHEAD) return false;
        uint64_t ctr = readU64BE(frame);
        if (!isCounterAllowed(ctr)) return false;
        size_t cipherLen = frameLen - SECURE_COUNTER_SIZE - SECURE_MAC_SIZE;
        const uint8_t* cipher = frame + SECURE_COUNTER_SIZE;
        const uint8_t* mac = cipher + cipherLen;
        plain->assign(cipherLen, 0);
        uint8_t nonce[SECURE_NONCE_SIZE];
        buildNonce(nonce, m_recvNonceSeed, ctr);
        int rc = crypto_aead_unlock(plain->data(), mac, m_recvKey, nonce,
                                    ad, adLen, cipher, cipherLen);
        if (rc != 0) {
            plain->clear();
            return false;
        }
        recordCounter(ctr);
        return true;
    }

private:
    static const uint64_t REPLAY_WINDOW = 2048;

    void buildNonce(uint8_t nonce[SECURE_NONCE_SIZE],
                    const uint8_t seed[16], uint64_t counter) const
    {
        writeU64BE(nonce, counter);
        std::memcpy(nonce + 8, seed, 16);
    }

    bool recordCounter(uint64_t ctr) {
        if (!m_recvStarted) {
            m_recvStarted = true;
            m_recvMaxCounter = ctr;
            std::memset(m_replayBitmap, 0, sizeof(m_replayBitmap));
        } else if (ctr > m_recvMaxCounter) {
            uint64_t shift = ctr - m_recvMaxCounter;
            if (shift >= REPLAY_WINDOW) {
                std::memset(m_replayBitmap, 0, sizeof(m_replayBitmap));
            } else {
                for (uint64_t i = 1; i <= shift; ++i) {
                    uint64_t pos = (m_recvMaxCounter + i) % REPLAY_WINDOW;
                    m_replayBitmap[pos / 8] &= static_cast<uint8_t>(~(1u << (pos % 8)));
                }
            }
            m_recvMaxCounter = ctr;
        } else if (m_recvMaxCounter - ctr >= REPLAY_WINDOW) {
            return false;
        }

        uint64_t pos = ctr % REPLAY_WINDOW;
        uint8_t bit = static_cast<uint8_t>(1u << (pos % 8));
        if (m_replayBitmap[pos / 8] & bit)
            return false;
        m_replayBitmap[pos / 8] |= bit;
        return true;
    }

    bool isCounterAllowed(uint64_t ctr) const {
        if (!m_recvStarted) return true;
        if (ctr > m_recvMaxCounter) return true;
        if (m_recvMaxCounter - ctr >= REPLAY_WINDOW) return false;
        uint64_t pos = ctr % REPLAY_WINDOW;
        uint8_t bit = static_cast<uint8_t>(1u << (pos % 8));
        return (m_replayBitmap[pos / 8] & bit) == 0;
    }

    uint8_t  m_sendKey[SECURE_KEY_SIZE];
    uint8_t  m_recvKey[SECURE_KEY_SIZE];
    uint8_t  m_sendNonceSeed[16];
    uint8_t  m_recvNonceSeed[16];
    uint64_t m_sendCounter;
    uint64_t m_recvMaxCounter;
    bool     m_recvStarted;
    uint8_t  m_replayBitmap[REPLAY_WINDOW / 8];
};

} // namespace VLan

#endif // VLAN_SECURE_FRAME_H

#ifndef VLAN_PAYLOAD_CIPHER_H
#define VLAN_PAYLOAD_CIPHER_H

#include <cstdint>
#include <cstring>
#include <cstdlib>

#ifdef QT_CORE_LIB
#include <QByteArray>
#include <QString>
#endif

extern "C" {
#include "monocypher.h"
}

namespace VLan {

static const int CIPHER_KEY_SIZE   = 32;
static const int CIPHER_SALT_SIZE  = 16;
static const int CIPHER_NONCE_SIZE = 24;
static const int CIPHER_MAC_SIZE   = 16;
static const int CIPHER_CTR_SIZE   = 4;
static const int CIPHER_OVERHEAD   = CIPHER_CTR_SIZE + CIPHER_MAC_SIZE; // 20 bytes
static const int CIPHER_SESSION_SEED_SIZE = 16;
static const int CIPHER_CHALLENGE_SIZE    = 32;
static const int REPLAY_WINDOW_SIZE       = 1024;
static const int MAX_REPLAY_PEERS         = 8;

static const uint32_t ARGON2_NB_BLOCKS = 4096;  // 4 MB work area
static const uint32_t ARGON2_NB_PASSES = 3;
static const uint32_t ARGON2_NB_LANES  = 1;

static const uint8_t ARGON2_AUTH_SALT[16] = {
    'V','L','a','n','-','a','u','t','h','-','s','a','l','t','v','1'
};

// ──── Free functions (usable from server without Qt) ────

// Argon2id: password → 32-byte intermediate (deliberately slow)
inline bool computeIntermediate(const uint8_t* password, size_t pwdLen,
                                uint8_t out[CIPHER_KEY_SIZE])
{
    void* work = malloc((size_t)ARGON2_NB_BLOCKS * 1024);
    if (!work) { memset(out, 0, CIPHER_KEY_SIZE); return false; }
    crypto_argon2_config cfg = {
        CRYPTO_ARGON2_ID, ARGON2_NB_BLOCKS, ARGON2_NB_PASSES, ARGON2_NB_LANES
    };
    crypto_argon2_inputs inp = {
        password, ARGON2_AUTH_SALT, (uint32_t)pwdLen, 16
    };
    crypto_argon2(out, CIPHER_KEY_SIZE, work, cfg, inp, crypto_argon2_no_extras);
    free(work);
    return true;
}

// intermediate → auth hash (fast, one-way; used for server-side verification)
inline void authHashFromIntermediate(const uint8_t intermediate[CIPHER_KEY_SIZE],
                                     uint8_t hash[CIPHER_KEY_SIZE])
{
    const uint8_t domain[] = "VLan-auth-hash";
    crypto_blake2b_keyed(hash, CIPHER_KEY_SIZE,
                         intermediate, CIPHER_KEY_SIZE,
                         domain, sizeof(domain) - 1);
}

// Convenience: password → auth hash (runs Argon2 internally)
inline void hashPassword(const uint8_t* password, size_t pwdLen,
                         uint8_t hash[CIPHER_KEY_SIZE])
{
    uint8_t intermediate[CIPHER_KEY_SIZE];
    computeIntermediate(password, pwdLen, intermediate);
    authHashFromIntermediate(intermediate, hash);
    crypto_wipe(intermediate, CIPHER_KEY_SIZE);
}

// intermediate + room salt → encryption key (fast, BLAKE2b-keyed)
inline void deriveKey(const uint8_t intermediate[CIPHER_KEY_SIZE],
                      const uint8_t salt[CIPHER_SALT_SIZE],
                      uint8_t key[CIPHER_KEY_SIZE])
{
    crypto_blake2b_keyed(key, CIPHER_KEY_SIZE,
                         intermediate, CIPHER_KEY_SIZE,
                         salt, CIPHER_SALT_SIZE);
}

// Challenge-response: auth_hash + challenge → response
inline void computeChallengeResponse(const uint8_t authHash[CIPHER_KEY_SIZE],
                                     const uint8_t challenge[CIPHER_CHALLENGE_SIZE],
                                     uint8_t response[CIPHER_KEY_SIZE])
{
    crypto_blake2b_keyed(response, CIPHER_KEY_SIZE,
                         authHash, CIPHER_KEY_SIZE,
                         challenge, CIPHER_CHALLENGE_SIZE);
}

// ──── Qt wrapper class (client-side only) ────

#ifdef QT_CORE_LIB

class PayloadCipher {
public:
    PayloadCipher(const uint8_t key[CIPHER_KEY_SIZE], uint32_t myPeerId,
                  const uint8_t sessionSeed[CIPHER_SESSION_SEED_SIZE])
        : m_myPeerId(myPeerId), m_sendCounter(0), m_replayPeerCount(0)
    {
        memcpy(m_key, key, CIPHER_KEY_SIZE);
        memcpy(m_sessionSeed, sessionSeed, CIPHER_SESSION_SEED_SIZE);
        memset(m_replayStates, 0, sizeof(m_replayStates));
    }

    ~PayloadCipher() {
        crypto_wipe(m_key, CIPHER_KEY_SIZE);
        crypto_wipe(m_sessionSeed, CIPHER_SESSION_SEED_SIZE);
        crypto_wipe(&m_sendCounter, sizeof(m_sendCounter));
    }

    // ── Static helpers for auth flow ──

    static QByteArray computeIntermediate(const QString& password)
    {
        QByteArray pwd = password.toUtf8();
        QByteArray inter(CIPHER_KEY_SIZE, '\0');
        VLan::computeIntermediate(
            reinterpret_cast<const uint8_t*>(pwd.constData()), pwd.size(),
            reinterpret_cast<uint8_t*>(inter.data()));
        return inter;
    }

    static QByteArray hashFromIntermediate(const QByteArray& intermediate)
    {
        QByteArray hash(CIPHER_KEY_SIZE, '\0');
        VLan::authHashFromIntermediate(
            reinterpret_cast<const uint8_t*>(intermediate.constData()),
            reinterpret_cast<uint8_t*>(hash.data()));
        return hash;
    }

    static QByteArray hashPassword(const QString& password)
    {
        QByteArray inter = computeIntermediate(password);
        QByteArray hash = hashFromIntermediate(inter);
        crypto_wipe(reinterpret_cast<uint8_t*>(inter.data()), inter.size());
        return hash;
    }

    static QByteArray challengeResponse(const QByteArray& authHash,
                                        const QByteArray& challenge)
    {
        QByteArray resp(CIPHER_KEY_SIZE, '\0');
        VLan::computeChallengeResponse(
            reinterpret_cast<const uint8_t*>(authHash.constData()),
            reinterpret_cast<const uint8_t*>(challenge.constData()),
            reinterpret_cast<uint8_t*>(resp.data()));
        return resp;
    }

    static QByteArray deriveKey(const QByteArray& intermediate,
                                const QByteArray& salt)
    {
        QByteArray key(CIPHER_KEY_SIZE, '\0');
        VLan::deriveKey(
            reinterpret_cast<const uint8_t*>(intermediate.constData()),
            reinterpret_cast<const uint8_t*>(salt.constData()),
            reinterpret_cast<uint8_t*>(key.data()));
        return key;
    }

    // ── Encrypt / Decrypt ──

    QByteArray encrypt(const QByteArray& ipPacket)
    {
        if (m_sendCounter >= 0xFFFFFFF0u) return QByteArray();
        if (ipPacket.size() < 20) return ipPacket;
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(ipPacket.constData());
        int ihl = (raw[0] & 0x0F) * 4;
        if (ihl < 20 || ihl > ipPacket.size()) return ipPacket;

        int payloadLen = ipPacket.size() - ihl;
        if (payloadLen <= 0) return ipPacket;

        uint32_t ctr = m_sendCounter++;

        uint8_t nonce[CIPHER_NONCE_SIZE];
        buildNonce(nonce, m_myPeerId, ctr);

        QByteArray out;
        out.resize(ihl + CIPHER_CTR_SIZE + payloadLen + CIPHER_MAC_SIZE);
        uint8_t* dst = reinterpret_cast<uint8_t*>(out.data());

        memcpy(dst, raw, ihl);
        memcpy(dst + ihl, &ctr, CIPHER_CTR_SIZE);

        uint8_t* cipherOut = dst + ihl + CIPHER_CTR_SIZE;
        uint8_t* macOut    = cipherOut + payloadLen;

        crypto_aead_lock(cipherOut, macOut,
                         m_key, nonce,
                         raw, ihl,                // AAD = IP header
                         raw + ihl, payloadLen);   // plaintext = IP payload
        return out;
    }

    QByteArray decrypt(const QByteArray& encPacket, uint32_t senderPeerId)
    {
        if (encPacket.size() < 20) return QByteArray();
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(encPacket.constData());
        int ihl = (raw[0] & 0x0F) * 4;
        if (ihl < 20 || encPacket.size() < ihl + CIPHER_OVERHEAD)
            return QByteArray();

        int cipherLen = encPacket.size() - ihl - CIPHER_CTR_SIZE - CIPHER_MAC_SIZE;
        if (cipherLen <= 0) return QByteArray();

        uint32_t ctr;
        memcpy(&ctr, raw + ihl, CIPHER_CTR_SIZE);

        if (!checkAndRecordCounter(senderPeerId, ctr))
            return QByteArray();

        uint8_t nonce[CIPHER_NONCE_SIZE];
        buildNonce(nonce, senderPeerId, ctr);

        const uint8_t* cipherData = raw + ihl + CIPHER_CTR_SIZE;
        const uint8_t* mac        = cipherData + cipherLen;

        QByteArray out;
        out.resize(ihl + cipherLen);
        uint8_t* dst = reinterpret_cast<uint8_t*>(out.data());

        memcpy(dst, raw, ihl);

        int rc = crypto_aead_unlock(dst + ihl, mac,
                                    m_key, nonce,
                                    raw, ihl,              // AAD = IP header
                                    cipherData, cipherLen); // ciphertext
        if (rc != 0) return QByteArray();
        return out;
    }

private:
    // nonce = peerId(4B) + counter(4B) + sessionSeed(16B)
    // sessionSeed guarantees uniqueness across sessions
    void buildNonce(uint8_t nonce[CIPHER_NONCE_SIZE],
                    uint32_t peerId, uint32_t counter) const
    {
        memcpy(nonce,      &peerId,       4);
        memcpy(nonce + 4,  &counter,      4);
        memcpy(nonce + 8,  m_sessionSeed, CIPHER_SESSION_SEED_SIZE);
    }

    // ── Per-sender anti-replay sliding window ──

    struct ReplayState {
        uint32_t peerId;
        uint32_t maxCounter;
        uint8_t  bitmap[REPLAY_WINDOW_SIZE / 8];
        bool     active;
    };

    bool checkAndRecordCounter(uint32_t peerId, uint32_t ctr)
    {
        ReplayState* rs = nullptr;
        ReplayState* freeSlot = nullptr;
        for (int i = 0; i < MAX_REPLAY_PEERS; ++i) {
            if (m_replayStates[i].active && m_replayStates[i].peerId == peerId) {
                rs = &m_replayStates[i];
                break;
            }
            if (!m_replayStates[i].active && !freeSlot)
                freeSlot = &m_replayStates[i];
        }
        if (!rs) {
            if (!freeSlot) return false;
            rs = freeSlot;
            rs->peerId = peerId;
            rs->maxCounter = 0;
            rs->active = true;
            memset(rs->bitmap, 0, sizeof(rs->bitmap));
            // first packet (ctr==0) accepted below
        }

        if (rs->maxCounter >= (uint32_t)REPLAY_WINDOW_SIZE &&
            ctr <= rs->maxCounter - (uint32_t)REPLAY_WINDOW_SIZE)
            return false;

        if (ctr > rs->maxCounter) {
            uint32_t shift = ctr - rs->maxCounter;
            if (shift >= (uint32_t)REPLAY_WINDOW_SIZE) {
                memset(rs->bitmap, 0, sizeof(rs->bitmap));
            } else {
                for (uint32_t i = 0; i < shift; ++i) {
                    uint32_t pos = (rs->maxCounter + i + 1) % REPLAY_WINDOW_SIZE;
                    rs->bitmap[pos / 8] &= ~(1u << (pos % 8));
                }
            }
            rs->maxCounter = ctr;
        }

        uint32_t pos = ctr % REPLAY_WINDOW_SIZE;
        uint8_t  bit = 1u << (pos % 8);
        if (rs->bitmap[pos / 8] & bit)
            return false;
        rs->bitmap[pos / 8] |= bit;
        return true;
    }

    uint8_t     m_key[CIPHER_KEY_SIZE];
    uint8_t     m_sessionSeed[CIPHER_SESSION_SEED_SIZE];
    uint32_t    m_myPeerId;
    uint32_t    m_sendCounter;
    ReplayState m_replayStates[MAX_REPLAY_PEERS];
    int         m_replayPeerCount;
};

#endif // QT_CORE_LIB

} // namespace VLan

#endif // VLAN_PAYLOAD_CIPHER_H

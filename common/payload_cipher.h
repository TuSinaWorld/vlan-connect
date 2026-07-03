#ifndef VLAN_PAYLOAD_CIPHER_H
#define VLAN_PAYLOAD_CIPHER_H

#include <cstdint>
#include <cstdlib>
#include <cstring>

#ifdef QT_CORE_LIB
#include <QByteArray>
#include <QString>
#endif

extern "C" {
#include "monocypher.h"
}

namespace VLan {

static const int CIPHER_KEY_SIZE        = 32;
static const int CIPHER_CHALLENGE_SIZE  = 32;

static const uint32_t ARGON2_NB_BLOCKS = 4096;  // 4 MB work area
static const uint32_t ARGON2_NB_PASSES = 3;
static const uint32_t ARGON2_NB_LANES  = 1;

static const uint8_t ARGON2_AUTH_SALT[16] = {
    'V','L','a','n','-','a','u','t','h','-','s','a','l','t','v','1'
};

inline bool computeIntermediate(const uint8_t* password, size_t pwdLen,
                                uint8_t out[CIPHER_KEY_SIZE])
{
    void* work = malloc(static_cast<size_t>(ARGON2_NB_BLOCKS) * 1024);
    if (!work) {
        memset(out, 0, CIPHER_KEY_SIZE);
        return false;
    }
    crypto_argon2_config cfg = {
        CRYPTO_ARGON2_ID, ARGON2_NB_BLOCKS, ARGON2_NB_PASSES, ARGON2_NB_LANES
    };
    crypto_argon2_inputs inp = {
        password, ARGON2_AUTH_SALT, static_cast<uint32_t>(pwdLen), 16
    };
    crypto_argon2(out, CIPHER_KEY_SIZE, work, cfg, inp, crypto_argon2_no_extras);
    free(work);
    return true;
}

inline void authHashFromIntermediate(const uint8_t intermediate[CIPHER_KEY_SIZE],
                                     uint8_t hash[CIPHER_KEY_SIZE])
{
    const uint8_t domain[] = "VLan-auth-hash";
    crypto_blake2b_keyed(hash, CIPHER_KEY_SIZE,
                         intermediate, CIPHER_KEY_SIZE,
                         domain, sizeof(domain) - 1);
}

inline void hashPassword(const uint8_t* password, size_t pwdLen,
                         uint8_t hash[CIPHER_KEY_SIZE])
{
    uint8_t intermediate[CIPHER_KEY_SIZE];
    computeIntermediate(password, pwdLen, intermediate);
    authHashFromIntermediate(intermediate, hash);
    crypto_wipe(intermediate, CIPHER_KEY_SIZE);
}

inline void computeChallengeResponse(const uint8_t authHash[CIPHER_KEY_SIZE],
                                     const uint8_t challenge[CIPHER_CHALLENGE_SIZE],
                                     uint8_t response[CIPHER_KEY_SIZE])
{
    crypto_blake2b_keyed(response, CIPHER_KEY_SIZE,
                         authHash, CIPHER_KEY_SIZE,
                         challenge, CIPHER_CHALLENGE_SIZE);
}

#ifdef QT_CORE_LIB

class PayloadCipher {
public:
    static QByteArray computeIntermediate(const QString& password)
    {
        QByteArray pwd = password.toUtf8();
        QByteArray inter(CIPHER_KEY_SIZE, '\0');
        VLan::computeIntermediate(
            reinterpret_cast<const uint8_t*>(pwd.constData()),
            static_cast<size_t>(pwd.size()),
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
        crypto_wipe(reinterpret_cast<uint8_t*>(inter.data()),
                    static_cast<size_t>(inter.size()));
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
};

#endif // QT_CORE_LIB

} // namespace VLan

#endif // VLAN_PAYLOAD_CIPHER_H

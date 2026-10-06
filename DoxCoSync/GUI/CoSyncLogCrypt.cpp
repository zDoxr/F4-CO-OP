#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <bcrypt.h>

#include "CoSyncLogCrypt.h"

#include <cstdio>
#include <cstring>

#pragma comment(lib, "bcrypt.lib")

namespace
{
    // RSA-2048 public key (exponent 65537). The private key is NOT in this
    // repository; it lives with the developers (see tools/cslog_decrypt.py).
    const unsigned char kModulus[256] = {
        0xCA, 0x57, 0x3A, 0x81, 0x3A, 0xE8, 0x82, 0x40, 0x95, 0x80, 0x72, 0x64, 0x52, 0x47, 0x17, 0x99,
        0x86, 0x66, 0x8F, 0x7E, 0xDF, 0x1B, 0xBE, 0xAA, 0xB8, 0x7D, 0x9A, 0x48, 0x16, 0x1C, 0x75, 0x92,
        0xCC, 0xB3, 0xD1, 0x4C, 0x1A, 0x6F, 0x4B, 0x3E, 0x23, 0xE6, 0x21, 0x07, 0x82, 0xAC, 0x72, 0x20,
        0x5C, 0x05, 0x84, 0x3B, 0x0F, 0x78, 0x40, 0xE6, 0xE2, 0x33, 0x53, 0x1A, 0xEE, 0x30, 0x6B, 0x84,
        0xDF, 0xB1, 0xF5, 0x78, 0x5D, 0xEF, 0x63, 0x01, 0x43, 0x98, 0xC7, 0x1E, 0x9B, 0x99, 0x2E, 0xAF,
        0x7D, 0x29, 0xD0, 0xD5, 0xB3, 0xEE, 0x6F, 0x55, 0x3F, 0xE6, 0xB6, 0xF1, 0x1C, 0x1D, 0xA5, 0xB3,
        0xFD, 0x37, 0x64, 0x06, 0x60, 0x76, 0xF6, 0xFE, 0x66, 0x94, 0x7A, 0x5B, 0x54, 0x20, 0x2A, 0x91,
        0x78, 0x8E, 0xD8, 0x88, 0x29, 0xE0, 0xD1, 0xB2, 0xD0, 0x4C, 0x91, 0x0D, 0x9D, 0x51, 0x4B, 0x09,
        0x63, 0x43, 0x3E, 0xBE, 0xC2, 0xF6, 0x75, 0x4E, 0x24, 0x6A, 0x79, 0xCC, 0x1E, 0x06, 0x0B, 0x48,
        0x04, 0x36, 0x7A, 0x6F, 0xA0, 0xBC, 0xC8, 0x1E, 0x1F, 0xD7, 0xC1, 0x9B, 0x37, 0x67, 0xD8, 0x8A,
        0x42, 0x9B, 0x0D, 0xD2, 0x31, 0xA8, 0x21, 0x23, 0xBF, 0x90, 0x7B, 0xCD, 0xEA, 0xA7, 0x8C, 0xEB,
        0x50, 0x15, 0x02, 0xCC, 0xE2, 0x08, 0x4A, 0xD7, 0x21, 0x8D, 0x5C, 0xC4, 0x9F, 0xBA, 0x7F, 0xF3,
        0xF7, 0x09, 0xF0, 0x52, 0x04, 0x55, 0x40, 0xBB, 0x7C, 0x43, 0x93, 0x63, 0xCF, 0xDF, 0x6B, 0x02,
        0xF8, 0x6C, 0x35, 0xB8, 0xE6, 0xE7, 0x45, 0x8F, 0xD8, 0x81, 0xEF, 0xFA, 0xF5, 0x67, 0xE8, 0x07,
        0xC0, 0xA9, 0x5D, 0x2B, 0x7E, 0x04, 0x71, 0xB1, 0x2C, 0x1E, 0xA8, 0xF4, 0xDF, 0xEA, 0x76, 0x46,
        0x3E, 0x3D, 0xC5, 0xD4, 0x54, 0x26, 0x85, 0x51, 0x6D, 0x43, 0x96, 0x56, 0x89, 0xB8, 0xFB, 0xA7,
    };
    const unsigned char kExponent[3] = { 0x01, 0x00, 0x01 };

    bool Ok(NTSTATUS s) { return s >= 0; }

    // key[32] || iv[16], RSA-OAEP(SHA-256) under kModulus. 256 bytes out.
    bool WrapKey(const unsigned char* secret, ULONG secretLen, unsigned char* out, ULONG outCap, ULONG& outLen)
    {
        BCRYPT_ALG_HANDLE alg = nullptr;
        BCRYPT_KEY_HANDLE key = nullptr;
        bool ok = false;
        unsigned char blob[sizeof(BCRYPT_RSAKEY_BLOB) + sizeof(kExponent) + sizeof(kModulus)];
        BCRYPT_RSAKEY_BLOB* hdr = reinterpret_cast<BCRYPT_RSAKEY_BLOB*>(blob);
        hdr->Magic = BCRYPT_RSAPUBLIC_MAGIC;
        hdr->BitLength = 2048;
        hdr->cbPublicExp = sizeof(kExponent);
        hdr->cbModulus = sizeof(kModulus);
        hdr->cbPrime1 = 0;
        hdr->cbPrime2 = 0;
        std::memcpy(blob + sizeof(BCRYPT_RSAKEY_BLOB), kExponent, sizeof(kExponent));
        std::memcpy(blob + sizeof(BCRYPT_RSAKEY_BLOB) + sizeof(kExponent), kModulus, sizeof(kModulus));
        if (Ok(BCryptOpenAlgorithmProvider(&alg, BCRYPT_RSA_ALGORITHM, nullptr, 0)) &&
            Ok(BCryptImportKeyPair(alg, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key, blob, sizeof(blob), 0)))
        {
            BCRYPT_OAEP_PADDING_INFO pad{ BCRYPT_SHA256_ALGORITHM, nullptr, 0 };
            ok = Ok(BCryptEncrypt(key, const_cast<PUCHAR>(secret), secretLen, &pad, nullptr, 0,
                                  out, outCap, &outLen, BCRYPT_PAD_OAEP));
        }
        if (key) BCryptDestroyKey(key);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        return ok;
    }
}

struct CoSyncCryptLog
{
    FILE*             file = nullptr;
    BCRYPT_ALG_HANDLE alg  = nullptr;
    BCRYPT_KEY_HANDLE key  = nullptr;
    unsigned char     counter[16] = {};   // next counter block
    unsigned char     stream[16]  = {};   // keystream of the current block
    unsigned          used = 16;          // bytes of `stream` already used

    bool NextBlock()
    {
        ULONG got = 0;
        if (!Ok(BCryptEncrypt(key, counter, 16, nullptr, nullptr, 0, stream, 16, &got, 0)) || got != 16)
            return false;
        for (int i = 15; i >= 0; --i)
            if (++counter[i] != 0)
                break;
        used = 0;
        return true;
    }
};

CoSyncCryptLog* CoSyncCryptLog_Open(const char* path)
{
    unsigned char secret[48];   // AES key 32 || iv 16
    if (!Ok(BCryptGenRandom(nullptr, secret, sizeof(secret), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        return nullptr;
    unsigned char wrapped[512];
    ULONG wrappedLen = 0;
    if (!WrapKey(secret, sizeof(secret), wrapped, sizeof(wrapped), wrappedLen) || wrappedLen == 0 || wrappedLen > 0xFFFF)
    {
        SecureZeroMemory(secret, sizeof(secret));
        return nullptr;
    }

    CoSyncCryptLog* log = new CoSyncCryptLog();
    bool ok = Ok(BCryptOpenAlgorithmProvider(&log->alg, BCRYPT_AES_ALGORITHM, nullptr, 0)) &&
              Ok(BCryptSetProperty(log->alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB,
                                   sizeof(BCRYPT_CHAIN_MODE_ECB), 0)) &&
              Ok(BCryptGenerateSymmetricKey(log->alg, &log->key, nullptr, 0, secret, 32, 0));
    std::memcpy(log->counter, secret + 32, 16);
    SecureZeroMemory(secret, sizeof(secret));
    if (ok)
        ok = fopen_s(&log->file, path, "wb") == 0 && log->file;
    if (!ok)
    {
        if (log->key) BCryptDestroyKey(log->key);
        if (log->alg) BCryptCloseAlgorithmProvider(log->alg, 0);
        delete log;
        return nullptr;
    }
    static const unsigned char kMagic[8] = { 'C', 'S', 'L', 'O', 'G', 0, 1, 0 };
    const unsigned char len[2] = { static_cast<unsigned char>(wrappedLen & 0xFF), static_cast<unsigned char>(wrappedLen >> 8) };
    fwrite(kMagic, 1, sizeof(kMagic), log->file);
    fwrite(len, 1, 2, log->file);
    fwrite(wrapped, 1, wrappedLen, log->file);
    fflush(log->file);
    return log;
}

void CoSyncCryptLog_Write(CoSyncCryptLog* log, const char* data, size_t len)
{
    if (!log || !log->file || !data)
        return;
    unsigned char buf[1024];
    while (len > 0)
    {
        const size_t n = len < sizeof(buf) ? len : sizeof(buf);
        for (size_t i = 0; i < n; ++i)
        {
            if (log->used == 16 && !log->NextBlock())
                return;   // never fall back to plain text
            buf[i] = static_cast<unsigned char>(data[i]) ^ log->stream[log->used++];
        }
        fwrite(buf, 1, n, log->file);
        data += n;
        len -= n;
    }
    fflush(log->file);
}

// The operating system's hashes. See os_crypto.hpp.
#include "os_crypto.hpp"

#include <algorithm>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonKeyDerivation.h>
#else
#include <dlfcn.h>

#include <mutex>
#endif

namespace kerfkite {
namespace server {

std::string hex(const std::string &bytes) {
    static const char *digits = "0123456789abcdef";
    std::string out;
    for (const unsigned char c : bytes) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}

#ifdef _WIN32

namespace {

struct Provider {
    BCRYPT_ALG_HANDLE handle = nullptr;
    explicit Provider(ULONG flags) {
        if (BCryptOpenAlgorithmProvider(&handle, BCRYPT_SHA256_ALGORITHM, nullptr, flags) != 0) handle = nullptr;
    }
    ~Provider() {
        if (handle) BCryptCloseAlgorithmProvider(handle, 0);
    }
};

}  // namespace

std::string sha256(const std::string &data) {
    static Provider provider(0);
    if (!provider.handle) throw std::runtime_error("Windows CNG has no SHA-256 here");
    std::string out(32, '\0');
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptCreateHash(provider.handle, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    // In pieces of at most 2^31 bytes: CNG counts in ULONG.
    for (size_t at = 0; ok && at < data.size(); at += 0x40000000)
        ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char *>(data.data() + at)),
                            static_cast<ULONG>(std::min<size_t>(0x40000000, data.size() - at)), 0) == 0;
    ok = ok && BCryptFinishHash(hash, reinterpret_cast<PUCHAR>(&out[0]), 32, 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    if (!ok) throw std::runtime_error("Windows CNG could not compute SHA-256");
    return out;
}

std::string pbkdf2_sha256(const std::string &password, const std::string &salt, uint64_t iterations, size_t length) {
    static Provider provider(BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!provider.handle) throw std::runtime_error("Windows CNG has no HMAC-SHA256 here");
    std::string out(length, '\0');
    if (BCryptDeriveKeyPBKDF2(provider.handle, reinterpret_cast<PUCHAR>(const_cast<char *>(password.data())), static_cast<ULONG>(password.size()),
                              reinterpret_cast<PUCHAR>(const_cast<char *>(salt.data())), static_cast<ULONG>(salt.size()), iterations,
                              reinterpret_cast<PUCHAR>(&out[0]), static_cast<ULONG>(length), 0) != 0)
        throw std::runtime_error("Windows CNG could not compute PBKDF2");
    return out;
}

bool scrypt(const std::string &, const std::string &, uint64_t, uint64_t, uint64_t, size_t, std::string &) { return false; }

std::string crypto_source() { return "Windows CNG"; }

#elif defined(__APPLE__)

std::string sha256(const std::string &data) {
    std::string out(CC_SHA256_DIGEST_LENGTH, '\0');
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    for (size_t at = 0; at < data.size(); at += 0x40000000)
        CC_SHA256_Update(&context, data.data() + at, static_cast<CC_LONG>(std::min<size_t>(0x40000000, data.size() - at)));
    CC_SHA256_Final(reinterpret_cast<unsigned char *>(&out[0]), &context);
    return out;
}

std::string pbkdf2_sha256(const std::string &password, const std::string &salt, uint64_t iterations, size_t length) {
    if (iterations > 0xffffffffu) throw std::runtime_error("CommonCrypto takes at most 2^32 - 1 PBKDF2 rounds");
    std::string out(length, '\0');
    if (CCKeyDerivationPBKDF(kCCPBKDF2, password.data(), password.size(), reinterpret_cast<const uint8_t *>(salt.data()), salt.size(),
                             kCCPRFHmacAlgSHA256, static_cast<unsigned>(iterations), reinterpret_cast<uint8_t *>(&out[0]), length) != 0)
        throw std::runtime_error("CommonCrypto could not compute PBKDF2");
    return out;
}

bool scrypt(const std::string &, const std::string &, uint64_t, uint64_t, uint64_t, size_t, std::string &) { return false; }

std::string crypto_source() { return "CommonCrypto"; }

#else

namespace {

// OpenSSL's libcrypto, by its stable EVP functions, which 1.1 and 3 share.
struct LibCrypto {
    void *library = nullptr;
    std::string problem;
    const void *(*evp_sha256)() = nullptr;
    int (*evp_digest)(const void *, size_t, unsigned char *, unsigned int *, const void *, void *) = nullptr;
    int (*pbkdf2)(const char *, int, const unsigned char *, int, int, const void *, int, unsigned char *) = nullptr;
    int (*scrypt)(const char *, size_t, const unsigned char *, size_t, uint64_t, uint64_t, uint64_t, uint64_t, unsigned char *, size_t) = nullptr;
};

const LibCrypto &libcrypto() {
    static LibCrypto found;
    static std::once_flag once;
    std::call_once(once, [] {
        for (const char *name : {"libcrypto.so.3", "libcrypto.so.1.1", "libcrypto.so"}) {
            found.library = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (found.library) break;
        }
        if (!found.library) {
            found.problem = "OpenSSL's libcrypto (libssl3 or libssl1.1) is not installed";
            return;
        }
        found.evp_sha256 = reinterpret_cast<const void *(*)()>(dlsym(found.library, "EVP_sha256"));
        found.evp_digest = reinterpret_cast<decltype(found.evp_digest)>(dlsym(found.library, "EVP_Digest"));
        found.pbkdf2 = reinterpret_cast<decltype(found.pbkdf2)>(dlsym(found.library, "PKCS5_PBKDF2_HMAC"));
        found.scrypt = reinterpret_cast<decltype(found.scrypt)>(dlsym(found.library, "EVP_PBE_scrypt"));
        if (!found.evp_sha256 || !found.evp_digest || !found.pbkdf2) found.problem = "the installed libcrypto lacks SHA-256 or PBKDF2";
    });
    return found;
}

const LibCrypto &usable() {
    const LibCrypto &c = libcrypto();
    if (!c.problem.empty()) throw std::runtime_error(c.problem);
    return c;
}

}  // namespace

std::string sha256(const std::string &data) {
    const LibCrypto &c = usable();
    std::string out(32, '\0');
    unsigned int size = 0;
    if (c.evp_digest(data.data(), data.size(), reinterpret_cast<unsigned char *>(&out[0]), &size, c.evp_sha256(), nullptr) != 1 || size != 32)
        throw std::runtime_error("libcrypto could not compute SHA-256");
    return out;
}

std::string pbkdf2_sha256(const std::string &password, const std::string &salt, uint64_t iterations, size_t length) {
    const LibCrypto &c = usable();
    if (iterations > 0x7fffffff || password.size() > 0x7fffffff || salt.size() > 0x7fffffff || length > 0x7fffffff)
        throw std::runtime_error("PBKDF2 parameters beyond libcrypto's range");
    std::string out(length, '\0');
    if (c.pbkdf2(password.data(), static_cast<int>(password.size()), reinterpret_cast<const unsigned char *>(salt.data()), static_cast<int>(salt.size()),
                 static_cast<int>(iterations), c.evp_sha256(), static_cast<int>(length), reinterpret_cast<unsigned char *>(&out[0])) != 1)
        throw std::runtime_error("libcrypto could not compute PBKDF2");
    return out;
}

bool scrypt(const std::string &password, const std::string &salt, uint64_t n, uint64_t r, uint64_t p, size_t length, std::string &out) {
    const LibCrypto &c = libcrypto();
    if (!c.problem.empty() || !c.scrypt) return false;
    out.assign(length, '\0');
    // Memory as hashlib.scrypt allows it here (132 * n * r * p bytes): 128 * n * r and the rest.
    const uint64_t memory = 132 * n * r * p + (1u << 20);
    return c.scrypt(password.data(), password.size(), reinterpret_cast<const unsigned char *>(salt.data()), salt.size(), n, r, p, memory,
                    reinterpret_cast<unsigned char *>(&out[0]), length) == 1;
}

std::string crypto_source() {
    const LibCrypto &c = libcrypto();
    return c.problem.empty() ? std::string("OpenSSL's libcrypto") : c.problem;
}

#endif

std::string sha256_hex(const std::string &data) { return hex(sha256(data)); }

}  // namespace server
}  // namespace kerfkite

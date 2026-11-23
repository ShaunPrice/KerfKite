// The operating system's hashes. See os_crypto.hpp.
#include "os_crypto.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <type_traits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonKeyDerivation.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
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

bool rsa_sha256_verify(const std::string &modulus, const std::string &exponent, const std::string &message, const std::string &signature) {
    if (modulus.empty() || signature.size() != modulus.size() || modulus.size() > 2048 || exponent.size() > 2048) return false;
    static struct Rsa {
        BCRYPT_ALG_HANDLE handle = nullptr;
        Rsa() {
            if (BCryptOpenAlgorithmProvider(&handle, BCRYPT_RSA_ALGORITHM, nullptr, 0) != 0) handle = nullptr;
        }
        ~Rsa() {
            if (handle) BCryptCloseAlgorithmProvider(handle, 0);
        }
    } rsa;
    if (!rsa.handle) throw std::runtime_error("Windows CNG has no RSA here");
    // BCRYPT_RSAKEY_BLOB, then the exponent and the modulus, big-endian.
    std::string blob(sizeof(BCRYPT_RSAKEY_BLOB), '\0');
    BCRYPT_RSAKEY_BLOB header{};
    header.Magic = BCRYPT_RSAPUBLIC_MAGIC;
    unsigned top = static_cast<unsigned char>(modulus[0]), bits = 0;
    while (top) ++bits, top >>= 1;
    header.BitLength = static_cast<ULONG>((modulus.size() - 1) * 8 + bits);
    header.cbPublicExp = static_cast<ULONG>(exponent.size());
    header.cbModulus = static_cast<ULONG>(modulus.size());
    std::memcpy(&blob[0], &header, sizeof header);
    blob += exponent;
    blob += modulus;
    BCRYPT_KEY_HANDLE key = nullptr;
    if (BCryptImportKeyPair(rsa.handle, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key, reinterpret_cast<PUCHAR>(&blob[0]), static_cast<ULONG>(blob.size()), 0) != 0)
        return false;
    std::string digest = sha256(message);
    BCRYPT_PKCS1_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM};
    const NTSTATUS status = BCryptVerifySignature(key, &padding, reinterpret_cast<PUCHAR>(&digest[0]), static_cast<ULONG>(digest.size()),
                                                  reinterpret_cast<PUCHAR>(const_cast<char *>(signature.data())), static_cast<ULONG>(signature.size()),
                                                  BCRYPT_PAD_PKCS1);
    BCryptDestroyKey(key);
    return status == 0;
}

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

namespace {

// A DER length and an unsigned INTEGER, for the PKCS #1 RSAPublicKey the Security framework imports.
std::string der_length(size_t size) {
    if (size < 0x80) return std::string(1, static_cast<char>(size));
    std::string digits;
    for (; size; size >>= 8) digits.insert(digits.begin(), static_cast<char>(size & 0xff));
    return std::string(1, static_cast<char>(0x80 | digits.size())) + digits;
}

std::string der_integer(const std::string &value) {
    const std::string body = (static_cast<unsigned char>(value[0]) & 0x80) ? std::string(1, '\0') + value : value;
    return "\x02" + der_length(body.size()) + body;
}

}  // namespace

bool rsa_sha256_verify(const std::string &modulus, const std::string &exponent, const std::string &message, const std::string &signature) {
    if (modulus.empty() || exponent.empty() || signature.size() != modulus.size()) return false;
    const std::string integers = der_integer(modulus) + der_integer(exponent);
    const std::string der = "\x30" + der_length(integers.size()) + integers;
    const auto data = [](const std::string &bytes) {
        return CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(bytes.data()), static_cast<CFIndex>(bytes.size()));
    };
    CFDataRef key_data = data(der), message_data = data(message), signature_data = data(signature);
    const void *names[] = {kSecAttrKeyType, kSecAttrKeyClass};
    const void *values[] = {kSecAttrKeyTypeRSA, kSecAttrKeyClassPublic};
    CFDictionaryRef attributes = CFDictionaryCreate(kCFAllocatorDefault, names, values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    bool ok = false;
    if (key_data && message_data && signature_data && attributes) {
        SecKeyRef key = SecKeyCreateWithData(key_data, attributes, nullptr);
        if (key) {
            ok = SecKeyVerifySignature(key, kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA256, message_data, signature_data, nullptr);
            CFRelease(key);
        }
    }
    for (CFTypeRef held : {static_cast<CFTypeRef>(key_data), static_cast<CFTypeRef>(message_data), static_cast<CFTypeRef>(signature_data),
                           static_cast<CFTypeRef>(attributes)})
        if (held) CFRelease(held);
    return ok;
}

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
    // RSA signature checks, by functions 1.1 and 3 both export.
    void *(*bn_bin2bn)(const unsigned char *, int, void *) = nullptr;
    void (*bn_free)(void *) = nullptr;
    void *(*rsa_new)() = nullptr;
    int (*rsa_set0_key)(void *, void *, void *, void *) = nullptr;
    void (*rsa_free)(void *) = nullptr;
    void *(*pkey_new)() = nullptr;
    int (*pkey_assign)(void *, int, void *) = nullptr;
    void (*pkey_free)(void *) = nullptr;
    void *(*md_ctx_new)() = nullptr;
    void (*md_ctx_free)(void *) = nullptr;
    int (*digest_verify_init)(void *, void **, const void *, void *, void *) = nullptr;
    int (*digest_verify)(void *, const unsigned char *, size_t, const unsigned char *, size_t) = nullptr;
    void (*err_clear)() = nullptr;
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
        const auto symbol = [&](auto &slot, const char *name) { slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(dlsym(found.library, name)); };
        symbol(found.bn_bin2bn, "BN_bin2bn");
        symbol(found.bn_free, "BN_free");
        symbol(found.rsa_new, "RSA_new");
        symbol(found.rsa_set0_key, "RSA_set0_key");
        symbol(found.rsa_free, "RSA_free");
        symbol(found.pkey_new, "EVP_PKEY_new");
        symbol(found.pkey_assign, "EVP_PKEY_assign");
        symbol(found.pkey_free, "EVP_PKEY_free");
        symbol(found.md_ctx_new, "EVP_MD_CTX_new");
        symbol(found.md_ctx_free, "EVP_MD_CTX_free");
        symbol(found.digest_verify_init, "EVP_DigestVerifyInit");
        symbol(found.digest_verify, "EVP_DigestVerify");
        symbol(found.err_clear, "ERR_clear_error");
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

bool rsa_sha256_verify(const std::string &modulus, const std::string &exponent, const std::string &message, const std::string &signature) {
    const LibCrypto &c = usable();
    if (!c.bn_bin2bn || !c.bn_free || !c.rsa_new || !c.rsa_set0_key || !c.rsa_free || !c.pkey_new || !c.pkey_assign || !c.pkey_free || !c.md_ctx_new ||
        !c.md_ctx_free || !c.digest_verify_init || !c.digest_verify)
        throw std::runtime_error("the installed libcrypto lacks RSA signature checks");
    if (modulus.empty() || exponent.empty() || signature.size() != modulus.size() || modulus.size() > 0x7fffffff || exponent.size() > 0x7fffffff)
        return false;
    const auto number = [&](const std::string &bytes) {
        return c.bn_bin2bn(reinterpret_cast<const unsigned char *>(bytes.data()), static_cast<int>(bytes.size()), nullptr);
    };
    void *n = number(modulus), *e = number(exponent), *rsa = c.rsa_new(), *pkey = c.pkey_new(), *context = c.md_ctx_new();
    bool ok = false;
    if (n && e && rsa && c.rsa_set0_key(rsa, n, e, nullptr) == 1) {
        n = e = nullptr;  // the key owns them
        if (pkey && c.pkey_assign(pkey, 6 /* EVP_PKEY_RSA */, rsa) == 1) {
            rsa = nullptr;  // the EVP_PKEY owns it
            ok = context && c.digest_verify_init(context, nullptr, c.evp_sha256(), nullptr, pkey) == 1 &&
                 c.digest_verify(context, reinterpret_cast<const unsigned char *>(signature.data()), signature.size(),
                                 reinterpret_cast<const unsigned char *>(message.data()), message.size()) == 1;
        }
    }
    if (context) c.md_ctx_free(context);
    if (pkey) c.pkey_free(pkey);
    if (rsa) c.rsa_free(rsa);
    if (n) c.bn_free(n);
    if (e) c.bn_free(e);
    if (c.err_clear) c.err_clear();
    return ok;
}

std::string crypto_source() {
    const LibCrypto &c = libcrypto();
    return c.problem.empty() ? std::string("OpenSSL's libcrypto") : c.problem;
}

#endif

std::string sha256_hex(const std::string &data) { return hex(sha256(data)); }

}  // namespace server
}  // namespace kerfkite

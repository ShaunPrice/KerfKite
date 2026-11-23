// The operating system's random bytes. See os_random.hpp.
#include "os_random.hpp"

#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <sys/random.h>
#include <unistd.h>
#else
#include <fcntl.h>
#include <sys/random.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace kerfkite {
namespace server {

std::string random_bytes(size_t count) {
    std::string out(count, '\0');
#ifdef _WIN32
    if (count && BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&out[0]), static_cast<ULONG>(count), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("The system random generator is unavailable");
#elif defined(__APPLE__)
    for (size_t at = 0; at < count; at += 256)
        if (getentropy(&out[at], count - at < 256 ? count - at : 256) != 0) throw std::runtime_error("The system random generator is unavailable");
#else
    size_t at = 0;
    while (at < count) {
        const ssize_t got = getrandom(&out[at], count - at, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("The system random generator is unavailable");
        }
        at += static_cast<size_t>(got);
    }
#endif
    return out;
}

std::string token_urlsafe(size_t count) {
    static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const std::string bytes = random_bytes(count);
    std::string out;
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const unsigned a = static_cast<unsigned char>(bytes[i]), b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0,
                       c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
        const unsigned group = (a << 16) | (b << 8) | c;
        out += alphabet[(group >> 18) & 63];
        out += alphabet[(group >> 12) & 63];
        if (i + 1 < bytes.size()) out += alphabet[(group >> 6) & 63];
        if (i + 2 < bytes.size()) out += alphabet[group & 63];
    }
    return out;
}

bool same_secret(const std::string &a, const std::string &b) {
    // As compare_digest: the work depends on the lengths, not on where the texts differ.
    unsigned difference = a.size() == b.size() ? 0u : 1u;
    const std::string &longer = a.size() >= b.size() ? a : b;
    for (size_t i = 0; i < longer.size(); ++i) {
        const unsigned x = i < a.size() ? static_cast<unsigned char>(a[i]) : 0u, y = i < b.size() ? static_cast<unsigned char>(b[i]) : 0u;
        difference |= x ^ y;
    }
    return difference == 0;
}

}  // namespace server
}  // namespace kerfkite

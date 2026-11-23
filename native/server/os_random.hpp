// Random bytes from the operating system's generator (getrandom or /dev/urandom, getentropy, BCryptGenRandom), for
// the session token, pairing tokens and challenges. Nothing here is a generator of its own.
#ifndef KERFKITE_SERVER_OS_RANDOM_HPP
#define KERFKITE_SERVER_OS_RANDOM_HPP

#include <cstddef>
#include <stdexcept>
#include <string>

namespace kerfkite {
namespace server {

// count bytes from the system's cryptographic generator; throws std::runtime_error when it cannot.
std::string random_bytes(size_t count);

// secrets.token_urlsafe(count): count random bytes in URL-safe base64 without padding.
std::string token_urlsafe(size_t count);

// secrets.compare_digest for text: equal in time that depends on the length only.
bool same_secret(const std::string &a, const std::string &b);

}  // namespace server
}  // namespace kerfkite

#endif

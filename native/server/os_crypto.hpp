// Hashes from the operating system's own cryptography, for the hosted editor's accounts, sessions and files:
// SHA-256, PBKDF2-HMAC-SHA256 and, where the system has it, scrypt. Nothing here computes a hash of its own: on
// Windows they are CNG's (bcrypt.dll), on macOS CommonCrypto's, and elsewhere OpenSSL's libcrypto, found when
// first needed (libcrypto.so.3, then 1.1), as the system's TLS and SSH already install it.
#ifndef KERFKITE_SERVER_OS_CRYPTO_HPP
#define KERFKITE_SERVER_OS_CRYPTO_HPP

#include <cstdint>
#include <string>

namespace kerfkite {
namespace server {

// The 32-byte SHA-256 digest of data. Throws std::runtime_error where the system has no SHA-256 (no libcrypto).
std::string sha256(const std::string &data);
// sha256 in lower-case hexadecimal, as hashlib.sha256(data).hexdigest() writes it.
std::string sha256_hex(const std::string &data);

// PBKDF2-HMAC-SHA256 of password and salt, length bytes, as hashlib.pbkdf2_hmac('sha256', ...) computes it.
std::string pbkdf2_sha256(const std::string &password, const std::string &salt, uint64_t iterations, size_t length);

// scrypt(password, salt, N, r, p) to length bytes, as hashlib.scrypt computes it, when the system provides scrypt
// (OpenSSL 1.1 and later; neither CNG nor CommonCrypto has it). False when it does not.
bool scrypt(const std::string &password, const std::string &salt, uint64_t n, uint64_t r, uint64_t p, size_t length, std::string &out);

// What the system's cryptography is, or why it cannot be used: for messages and the setup check.
std::string crypto_source();

// Lower-case hexadecimal of bytes.
std::string hex(const std::string &bytes);

}  // namespace server
}  // namespace kerfkite

#endif

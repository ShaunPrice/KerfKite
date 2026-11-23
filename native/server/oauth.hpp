// The hosted assistant server's single-operator OAuth (studio/mcp_transport.py over FastMCP's JWTVerifier and
// RemoteAuthProvider): which bearer tokens the HTTP transport takes. A token is an RS256 JSON Web Token from one
// issuer, for the server's /mcp resource, with the workshop scope and the operator's subject, signed by a key of
// the issuer's key set (JWKS), which is fetched from a public HTTPS address. What FastMCP, joserfc and
// cryptography accept is reproduced: the token's segments and their encodings, its header and claims, the key set's
// cache (an hour) and its refreshes (one at most every 30 seconds, also when it fails), the keys it takes and passes
// over, and the address checks made before it is fetched. The signature itself is checked by the system's
// cryptography (os_crypto.hpp) and the key set fetched by KerfKite's TLS relay. docs/NATIVE.md lists where the
// server's own rules differ.
#ifndef KERFKITE_SERVER_OAUTH_HPP
#define KERFKITE_SERVER_OAUTH_HPP

#include <map>
#include <mutex>
#include <string>

#include "kerfkite/json.hpp"

namespace kerfkite {
namespace server {

struct RemoteAuth {
    std::string base_url;    // the public origin as AnyHttpUrl writes it, without a final '/': "https://host[:port]"
    std::string host;        // its host as AnyHttpUrl gives it, "[...]" for IPv6: the Host the guard allows
    std::string issuer;      // as configured: a token's "iss" must be this text
    std::string issuer_url;  // the issuer as the protected-resource metadata lists it (an empty path stays empty)
    std::string jwks_url;    // as configured
    std::string subject;     // the operator: a token's "sub"

    std::string audience() const { return base_url + "/mcp"; }
    std::string metadata_url() const { return base_url + "/.well-known/oauth-protected-resource/mcp"; }
};

// KERFKITE_MCP_PUBLIC_ORIGIN, _ISSUER, _SUBJECT and _JWKS_URL, checked in that order, as remote_auth_from_env()
// checks them. Throws std::runtime_error with the reference's message for the first that is wrong; an address the
// server cannot read as plainly as AnyHttpUrl writes it is refused too (own rule; docs/NATIVE.md).
RemoteAuth remote_auth_from_environment();

class TokenVerifier {
public:
    // relay: the TLS relay that fetches the key set.
    TokenVerifier(RemoteAuth auth, std::string relay);

    // Whether the bearer token (the Authorization header's value after "Bearer ", each byte a character as
    // Starlette reads headers) is one the reference took. May fetch the key set.
    bool verify(const std::string &token);

    const RemoteAuth &auth() const { return auth_; }

private:
    struct Key {
        bool rsa = false;
        std::string modulus, exponent;  // big-endian, without leading zeros
    };
    enum class Look { found, missing, refused };

    Look lookup(const Json *kid, Key &key) const;  // in the cache, data_ held
    bool key_for(const Json *kid, Key &key);
    bool refresh();  // replaces the cache from a fresh key set; false, with the cache as it was, when it fails
    bool fetch(std::string &body) const;

    RemoteAuth auth_;
    std::string relay_;
    std::mutex data_, refreshing_;
    std::map<std::string, Key> cache_;
    double cache_time_ = 0;  // time.monotonic() of the last refresh; 0 before any, as in JWTVerifier
    bool attempted_ = false;
    double last_attempt_ = 0;
};

// The pieces, for the server's tests.
namespace oauth_detail {
// binascii.a2b_base64 as base64.urlsafe_b64decode calls it (not strict): ASCII text, '-' and '_' for '+' and '/',
// other characters passed over, decoding ended by padding; false where it raises.
bool lenient_base64url(const std::string &text, std::string &out);
// joserfc's urlsafe_b64decode: only the URL-safe alphabet, no padding, and the bits past the last byte zero.
bool strict_base64url(const std::string &text, std::string &out);
// json.loads of bytes: UTF-8 (with or without a signature), UTF-16 or UTF-32 by the first bytes, as
// json.detect_encoding tells them; false where it raises or (own rule) where the core's JSON reader refuses.
bool json_from_bytes(const std::string &bytes, Json &out);
// Whether json.loads decodes the bytes to text (its 'surrogatepass' decoding), whatever the text then is.
bool decodes(const std::string &bytes);
// Whether (x mod p, y mod p) lies on the curve named as joserfc names it (P-256, P-384, P-521, secp256k1); false
// for another name.
bool on_curve(const std::string &curve, const std::string &x, const std::string &y);
// "RSA" or "EC" for a key of a key set that FastMCP keeps (_jwk_to_pem succeeds), empty for one it passes over.
std::string key_kind(const Json &jwk);
}  // namespace oauth_detail

}  // namespace server
}  // namespace kerfkite

#endif

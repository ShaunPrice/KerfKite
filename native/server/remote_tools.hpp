// The assistant server's optional tools for what lies outside the workshop: a paired phone monitor (pendant_*,
// as studio/mcp_pendant.py) and an account on a hosted editor (hosted_*, as studio/mcp_files.py). The operator
// configures each through the environment; with nothing configured its tools are not offered, and a wrong
// configuration stops the server at its start, with the reference's message. Their requests go through
// KerfKite's TLS relay (kerfkite-pendant-tls -fetch), as TLS is not written here: no proxy, no redirect
// followed, the operator's certificate authority when one is set and the system's otherwise. What the reference's
// http.client, json and http.cookies made of an answer is reproduced; docs/NATIVE.md lists where it is not.
#ifndef KERFKITE_SERVER_REMOTE_TOOLS_HPP
#define KERFKITE_SERVER_REMOTE_TOOLS_HPP

#include <memory>
#include <mutex>
#include <string>

#include "kerfkite/json.hpp"
#include "mcp.hpp"
#include "mcp_tools.hpp"

namespace kerfkite {
namespace server {

// The paired monitor: one base address and its pairing token.
class PendantClient {
public:
    // PendantClient.from_environment(): KERFKITE_PENDANT_URL with KERFKITE_PENDANT_TOKEN or _TOKEN_FILE, and
    // KERFKITE_PENDANT_CA_FILE; null when none is set, ValueError when the configuration is wrong.
    static std::unique_ptr<PendantClient> from_environment(const std::string &relay);
    PendantClient(const std::string &url, const std::string &token, const std::string &ca_file, const std::string &relay);

    // GET /api/status: the controller and arm state, with a new request_id.
    Json status() const;
    // A job, jog or reference action, after the operator's confirmation where the reference asks for one.
    Json action(mcp::Exchange &exchange, const std::string &action, const std::string &request_id, const std::string &control_epoch,
                const Json &values) const;

private:
    Json request(const std::string &path, const Json *payload) const;
    std::string base_, token_, ca_file_, relay_;
    std::string refused_;  // what every request fails with, where http.client could not have made one
};

// The hosted editor account: a fixed HTTPS origin, a username and its password.
class HostedFiles {
public:
    // HostedFiles.from_environment(): KERFKITE_FILES_URL, _USERNAME, _PASSWORD or _PASSWORD_FILE, _CA_FILE.
    static std::unique_ptr<HostedFiles> from_environment(const std::string &relay);
    HostedFiles(const std::string &url, const std::string &username, const std::string &password, const std::string &ca_file,
                const std::string &relay);

    // api(path, payload): the account's JSON answer (a GET, or a POST with payload), logging in again once
    // when the session has expired; the raw bytes for a download.
    Json api(const std::string &path, const Json *payload = nullptr) const;
    std::string download_raw(const std::string &path) const;
    // The account's entry for a file id, or ValueError.
    Json lookup(const std::string &identifier) const;
    // The file's checked name and bytes.
    std::pair<std::string, std::string> download(const std::string &identifier) const;
    // The file stored in the account (destination empty) or sent to a receiver, with its verified receipt.
    Json send(const std::string &name, const std::string &data, const std::string *destination) const;

private:
    struct Answer {
        long long status = 0;
        Json headers;  // name -> [values], Go's canonical names
        std::string body;
    };
    Answer exchange(const std::string &method, const std::string &path, const Json &headers, const std::string &body, size_t limit) const;
    std::string call(const std::string &path, const Json *payload, bool download) const;
    void login() const;
    std::string base_, origin_, user_, password_, ca_file_, relay_;
    std::string refused_;  // as PendantClient's
    mutable std::mutex lock_;
    mutable std::string session_, csrf_;
};

void add_pendant_tools(mcp::Server &server, std::shared_ptr<const PendantClient> client);
void add_hosted_tools(mcp::Server &server, std::shared_ptr<const HostedFiles> hosted, const Artifacts &artifacts);

}  // namespace server
}  // namespace kerfkite

#endif

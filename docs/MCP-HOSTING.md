# Connect a hosted workshop to an AI assistant

A remote assistant connects to the KerfKite workshop at an HTTPS address such as `https://workshop.example.org/mcp`. The host runs the MCP process and holds its design files. Your OAuth provider signs the access tokens. Each process accepts **one operator** from one issuer. Use a separate hostname, process, filesystem account and private output directory for each additional operator.

For a desktop assistant on the workshop computer, the [local stdio setup](MCP.md) uses the signed-in operating system account and needs no published endpoint.

## Prepare the host

Install KerfKite Studio on the host and check `kerfkite-mcp --help`, following the [MCP installation guide](MCP.md). The workshop tools also need Google Chrome, Chromium or Microsoft Edge. Run the service under an ordinary account, not as root. Keep `kerfkite-pendant-tls` beside `kerfkite-mcp`, as the packages install it: it fetches the issuer's signing keys over HTTPS. Use a dedicated DNS hostname you control with a publicly trusted TLS certificate. The examples below use reserved domains. Supply your own addresses and operator identifier when configuring your service environment.

1. Register the **complete** public MCP URL as an API resource with your OAuth provider. Its audience must match `https://workshop.example.org/mcp`, including the path and any non-default port. Assign the API scope `kerfkite:workshop` to the operator.
2. Configure the authorization-code flow with S256 PKCE. The issuer must publish OAuth or OpenID discovery metadata and an HTTPS JWKS URL. Configure RS256-signed JWT access tokens with `iss`, `aud`, an expiry `exp`, the operator's stable `sub` claim and the required scope. Use the subject and issuer from the access token the provider issues for this resource. The provider must honour the MCP client's requested `resource` when issuing the token.
3. Configure a client-registration method supported by both the assistant and your identity provider. Options include an administrator-registered OAuth client, client ID metadata documents and dynamic client registration. If registering a confidential client yourself, generate its secret at the identity provider and enter it in the assistant's OAuth client settings.
4. Grant the service account write access to a private output directory. A given process shares its working data with clients signed in as the configured operator. Configure the following four variables in that process's environment.

| Variable | Example | Purpose |
| --- | --- | --- |
| `KERFKITE_MCP_PUBLIC_ORIGIN` | `https://workshop.example.org` | Public HTTPS origin of this process. The endpoint path is always `/mcp`. |
| `KERFKITE_MCP_ISSUER` | `https://identity.example.org/realms/workshop` | Exact value of the issuer's `iss` claim, including its trailing slash if present. |
| `KERFKITE_MCP_JWKS_URL` | `https://identity.example.org/realms/workshop/keys` | Public HTTPS signing key set from the issuer's discovery metadata. |
| `KERFKITE_MCP_SUBJECT` | `operator-id-from-your-provider` | Exact allowed `sub`. Use the provider's stable subject identifier. |

The host needs outbound HTTPS access to the configured key set. The server connects to it directly, without a proxy, and only when every address of its host name is public. Its clock must be accurate to evaluate the token expiry and any `nbf` claim. KerfKite uses the configured external provider for login and has no built-in remote password.

## Serve the HTTPS endpoint

Start KerfKite on the same host as the reverse proxy. This example uses the Debian package's program.

```sh
/opt/kerfkite/kerfkite-mcp --transport http --port 8766 --output-dir /srv/kerfkite/operator/artifacts
```

The service listens on `127.0.0.1` and reports its local endpoint and the public URL it serves. Configure the TLS reverse proxy to forward the MCP path and the protected resource metadata path, preserving the public Host header and Authorization header. Disable proxy response buffering for the MCP stream. The following Caddy site uses its automatic HTTPS support for a hostname with working public DNS. [Caddy reverse proxy](https://caddyserver.com/docs/caddyfile/directives/reverse_proxy) · [Caddy automatic HTTPS](https://caddyserver.com/docs/automatic-https)

```caddyfile
workshop.example.org {
    @workshop path /mcp /.well-known/oauth-protected-resource/mcp
    handle @workshop {
        reverse_proxy 127.0.0.1:8766 {
            flush_interval -1
        }
    }
    handle {
        respond 404
    }
}
```

Only these two paths are required on this virtual host. OAuth login, authorization-server metadata and key distribution remain at the identity provider. A private or self-signed Studio certificate can still serve local tools. A hosted assistant must trust the public endpoint's certificate.

## Connect an assistant

**ChatGPT.** Open the custom MCP/plugin settings available to your account. Enable developer mode if your workspace requires it. Enter the complete `/mcp` URL, select OAuth and use a client registration supported by your issuer. If configuring a client manually, copy the redirect URI shown for this connection into the identity provider's redirect allowlist. Sign in as the configured operator and review the discovered workshop tools. [OpenAI custom MCP setup](https://developers.openai.com/plugins/deploy/connect-chatgpt) · [OpenAI OAuth requirements and callback modes](https://developers.openai.com/plugins/build/auth)

**Claude.** In **Customize → Connectors**, add a custom remote connector using the complete `/mcp` URL. Select the sign-in option and choose a supported OAuth client identity. When using a manually registered client, configure the redirect URI that Claude documents and enter the issued client ID and secret if applicable. Complete sign-in, then enable the connector in the conversation. Organization settings can restrict who can register a connector. [Claude custom remote connector setup](https://support.claude.com/en/articles/11175166-get-started-with-custom-connectors-using-remote-mcp)

Use a read-only workshop query first and inspect the reported tool list. Registration and permissions belong to the assistant account and identity provider. The repository's tests cover the MCP protocol and authorization using an ephemeral signing key. Connecting a particular live account still requires its sign-in.

## Check discovery and diagnose access

```sh
curl -i https://workshop.example.org/.well-known/oauth-protected-resource/mcp
curl -i -X POST -H 'Content-Type: application/json' -H 'Accept: application/json, text/event-stream' --data '{}' https://workshop.example.org/mcp
```

The first request returns HTTP 200 and metadata describing the resource URL, configured issuer and `kerfkite:workshop` scope. The second, without a bearer token, returns HTTP 401 and a `WWW-Authenticate` challenge pointing back to the metadata. Discovery is public so the assistant can find the login provider. MCP tool discovery and tool calls require an authorized bearer token.

| Symptom | Check |
| --- | --- |
| Startup reports an unset variable | Configure all four values in the process environment and restart it. |
| Metadata returns 404 or the wrong resource | Forward the exact discovery path. Check the dedicated public origin and the proxy's virtual host. |
| Sign-in never starts | Check certificate trust, OAuth discovery, S256 support, client registration and the assistant workspace's connector policy. |
| Token receives 401 | Check RS256, signing key ID, exact issuer, full resource audience, operator subject, scope, token expiry, `nbf` and the host clock. Check JWKS reachability from the KerfKite host. |
| Unexpected Host or Origin response | Preserve the configured public Host in the proxy. A browser request with an Origin header must come from the allowed origin. |
| Tools work locally but the remote model operation fails | Check the hosted service account's browser installation and write permissions on its output directory. |

From a source checkout on Linux, `validation/mcp_http_parity.py` compares this transport with the reference implementation, using signing keys and key servers made for the run. It covers discovery, sign-in challenges, rejected requests and tokens, authenticated initialization, tool listing and authorized tool calls. [Native notes](NATIVE.md#the-assistant-server) [MCP authorization specification](https://modelcontextprotocol.io/specification/2025-11-25/basic/authorization) · [FastMCP remote OAuth](https://gofastmcp.com/docs/servers/auth/remote-oauth)

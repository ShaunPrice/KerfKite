"""Streamable HTTP protocol and OAuth checks using a local, ephemeral issuer key."""

from contextlib import ExitStack
import importlib.util
import os
import time
import unittest
from unittest.mock import AsyncMock, patch


ENV = {
    "KERFKITE_MCP_PUBLIC_ORIGIN": "https://workshop.example.org",
    "KERFKITE_MCP_ISSUER": "https://identity.example.org/realms/workshop",
    "KERFKITE_MCP_JWKS_URL": "https://identity.example.org/realms/workshop/keys",
    "KERFKITE_MCP_SUBJECT": "example-operator",
}
RESOURCE = ENV["KERFKITE_MCP_PUBLIC_ORIGIN"] + "/mcp"
METADATA = "/.well-known/oauth-protected-resource/mcp"
ACCEPT = "application/json, text/event-stream"


@unittest.skipUnless(importlib.util.find_spec("fastmcp"), "install the mcp optional dependency")
class RemoteWorkshopTests(unittest.TestCase):
    def setUp(self):
        from studio.mcp_transport import remote_auth_from_env
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.stack.enter_context(patch.dict(os.environ, ENV))
        self.provider = remote_auth_from_env()

    def test_configuration_is_required_and_urls_are_public_https(self):
        from studio.mcp_transport import remote_auth_from_env
        for name in ENV:
            with self.subTest(missing=name), patch.dict(os.environ, {name: ""}), self.assertRaisesRegex(ValueError, name):
                remote_auth_from_env()
        for key, value in [
            ("PUBLIC_ORIGIN", "http://workshop.example.org"),
            ("PUBLIC_ORIGIN", "https://localhost"),
            ("PUBLIC_ORIGIN", "https://127.0.0.1"),
            ("PUBLIC_ORIGIN", "https://workshop.example.org/mcp"),
            ("PUBLIC_ORIGIN", "https://user@workshop.example.org"),
            ("ISSUER", "https://identity.example.org/realm?key=example"),
            ("JWKS_URL", "https://[::1]/jwks"),
        ]:
            name = "KERFKITE_MCP_" + key
            with self.subTest(key=key, value=value), patch.dict(os.environ, {name: value}), self.assertRaisesRegex(ValueError, name):
                remote_auth_from_env()

    def test_root_issuer_identifier_keeps_an_empty_path(self):
        from studio.mcp_transport import remote_auth_from_env
        for issuer in ["https://identity.example.org", "https://identity.example.org/"]:
            with self.subTest(issuer=issuer), patch.dict(os.environ, {"KERFKITE_MCP_ISSUER": issuer}):
                provider = remote_auth_from_env()
                self.assertEqual(str(provider.authorization_servers[0]), issuer)
                self.assertEqual(provider.token_verifier.issuer, issuer)

    def test_discovery_and_authenticated_protocol(self):
        from fastmcp import FastMCP
        from joserfc import jwt
        from joserfc.jwk import RSAKey
        from starlette.testclient import TestClient

        key = RSAKey.generate_key(parameters={"kid": "workshop-test"})
        # Replace only the external issuer fetch; the stock JWKS parser, RSA
        # signature checks, OAuth middleware and MCP request handling run normally.
        fetch = self.stack.enter_context(patch.object(self.provider.token_verifier, "_fetch_jwks", new_callable=AsyncMock))
        fetch.return_value = {"keys": [key.as_dict()]}
        server = FastMCP("Test workshop", auth=self.provider)
        calls = []

        @server.tool
        def workshop_status() -> str:
            calls.append(True)
            return "ready"

        app = server.http_app(path="/mcp", stateless_http=True, json_response=True,
                              host_origin_protection=True, allowed_hosts=[self.provider.base_url.host],
                              allowed_origins=[ENV["KERFKITE_MCP_PUBLIC_ORIGIN"]])
        client = self.stack.enter_context(TestClient(app, base_url=ENV["KERFKITE_MCP_PUBLIC_ORIGIN"]))
        metadata = client.get(METADATA)
        self.assertEqual(metadata.status_code, 200)
        self.assertEqual(metadata.json()["resource"], RESOURCE)
        self.assertEqual(metadata.json()["authorization_servers"], [ENV["KERFKITE_MCP_ISSUER"]])
        self.assertEqual(metadata.json()["scopes_supported"], ["kerfkite:workshop"])

        now = int(time.time())
        claims = {"iss": ENV["KERFKITE_MCP_ISSUER"], "aud": RESOURCE, "sub": ENV["KERFKITE_MCP_SUBJECT"],
                  "scope": "kerfkite:workshop", "exp": now + 300}
        def request(method, params, token=None, headers=None):
            h = {"Accept": ACCEPT, **(headers or {})}
            if token:
                h["Authorization"] = "Bearer " + token
            return client.post("/mcp", headers=h, json={"jsonrpc": "2.0", "id": 1, "method": method, "params": params})

        def sign(payload, signer=key):
            return jwt.encode({"alg": "RS256", "kid": "workshop-test"}, payload, signer)

        call = {"name": "workshop_status", "arguments": {}}
        anon = request("tools/call", call)
        self.assertEqual(anon.status_code, 401)
        self.assertIn(ENV["KERFKITE_MCP_PUBLIC_ORIGIN"] + METADATA, anon.headers["WWW-Authenticate"])
        for change in [{"sub": "other"}, {"aud": "wrong-api"}, {"iss": "another-issuer"},
                       {"scope": "some-other-scope"}, {"exp": now - 1}, {"nbf": now + 60},
                       {"exp": None}, {"sub": None}, {"aud": None}]:
            modified = {k: v for k, v in (claims | change).items() if v is not None}
            with self.subTest(change=change):
                self.assertEqual(request("tools/call", call, sign(modified)).status_code, 401)
        self.assertEqual(request("tools/call", call, sign(claims, RSAKey.generate_key())).status_code, 401)
        token = sign(claims)
        self.assertGreaterEqual(request("tools/call", call, token, {"Host": "other.example.org"}).status_code, 400)
        self.assertGreaterEqual(request("tools/call", call, token, {"Origin": "https://other.example.org"}).status_code, 400)
        self.assertEqual(calls, [])
        init = request("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                     "clientInfo": {"name": "transport-test", "version": "1"}}, token)
        self.assertEqual(init.status_code, 200)
        self.assertEqual(init.json()["result"]["serverInfo"]["name"], "Test workshop")
        listed = request("tools/list", {}, token)
        self.assertEqual(listed.json()["result"]["tools"][0]["name"], "workshop_status")
        result = request("tools/call", call, token)
        self.assertEqual(result.json()["result"]["content"][0]["text"], "ready")
        self.assertEqual(calls, [True])
        fetch.assert_awaited_once()

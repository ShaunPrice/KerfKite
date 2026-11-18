"""Single-operator OAuth configuration for the optional remote artwork server."""

import ipaddress
import math
import os
import time
from typing import Annotated

from fastmcp.server.auth import RemoteAuthProvider
from fastmcp.server.auth.providers.jwt import JWTVerifier
from pydantic import AnyHttpUrl, TypeAdapter, UrlConstraints, ValidationError


WORKSHOP_SCOPE = "kerfkite:workshop"


def _setting(suffix):
    name = "KERFKITE_MCP_" + suffix
    value = os.environ.get(name, "")
    if not value or value != value.strip() or not value.isprintable() or len(value) > 2048:
        raise ValueError(f"Set {name} to a nonempty value for remote OAuth.")
    return value


def _https_url(suffix, *, origin=False):
    value = _setting(suffix)
    error = f"KERFKITE_MCP_{suffix} must be a public HTTPS {'origin' if origin else 'URL'}."
    if any(c in value for c in ("\\", "?", "#")) or any(c.isspace() for c in value):
        raise ValueError(error)
    try:
        url = AnyHttpUrl(value)
    except ValidationError:
        raise ValueError(error) from None
    if (url.scheme != "https" or not url.host or url.username is not None or url.password is not None
            or (origin and url.path not in (None, "", "/"))):
        raise ValueError(error)
    host = url.host.strip("[]").lower().rstrip(".")
    try:
        ip = ipaddress.ip_address(host)
    except ValueError:
        if "." not in host or host.endswith((".local", ".localhost", ".internal", ".lan", ".home.arpa")):
            raise ValueError(error) from None
    else:
        if not ip.is_global or ip.is_multicast:
            raise ValueError(error)
    return value


class _WorkshopVerifier(JWTVerifier):
    def __init__(self, *, operator, **kwargs):
        super().__init__(**kwargs)
        self.operator = operator

    async def verify_token(self, token):
        # The SDK verifies the signature against the administrator's JWKS as well
        # as issuer, resource audience and required scopes before claims are used.
        try:
            verified = await super().verify_token(token)
        except OverflowError:
            return None
        if verified is None or verified.claims.get("sub") != self.operator:
            return None
        now = time.time()
        exp, nbf = verified.claims.get("exp"), verified.claims.get("nbf", 0)
        if any(type(t) is not int and (type(t) is not float or not math.isfinite(t)) for t in (exp, nbf)):
            return None
        if exp <= now or nbf > now:
            return None
        return verified


def remote_auth_from_env():
    """Build a protected /mcp endpoint for one issuer, resource and operator."""
    base_url = str(AnyHttpUrl(_https_url("PUBLIC_ORIGIN", origin=True))).rstrip("/")
    issuer = _https_url("ISSUER")
    verifier = _WorkshopVerifier(
        operator=_setting("SUBJECT"),
        jwks_uri=_https_url("JWKS_URL"),
        issuer=issuer,
        audience=base_url + "/mcp",
        algorithm="RS256",
        required_scopes=[WORKSHOP_SCOPE],
        ssrf_safe=True,
    )
    # Issuer identifiers use exact string matching. Preserve a root issuer's
    # empty path rather than introducing a trailing slash in discovery.
    issuer_url = TypeAdapter(Annotated[AnyHttpUrl, UrlConstraints(preserve_empty_path=True)]).validate_python(issuer)
    return RemoteAuthProvider(
        token_verifier=verifier,
        authorization_servers=[issuer_url],
        base_url=base_url,
        resource_name="KerfKite Workshop",
    )

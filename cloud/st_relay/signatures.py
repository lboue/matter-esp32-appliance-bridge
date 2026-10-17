"""Verification of SmartThings' signed webhook requests.

SmartThings signs every request to an app's target URL with HTTP Signatures
(draft-cavage), RSA-SHA256. The Authorization header names the signed headers
and a keyId; the key is an X.509 certificate served from key.smartthings.com
at that path. The body is bound to the signature through the Digest header.
"""

from __future__ import annotations

import base64
import hashlib
import logging
import re
import time
from collections.abc import Awaitable, Callable, Mapping
from email.utils import parsedate_to_datetime

import httpx
from cryptography import x509
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import padding, rsa

log = logging.getLogger(__name__)

MAX_CLOCK_SKEW_S = 300

KeyFetcher = Callable[[str], Awaitable[rsa.RSAPublicKey]]


class SignatureError(Exception):
    pass


_PARAM = re.compile(r'(\w+)="([^"]*)"')


def parse_signature_header(value: str) -> dict[str, str]:
    if not value.lower().startswith("signature "):
        raise SignatureError("Authorization is not an HTTP Signature")
    params = dict(_PARAM.findall(value[len("signature ") :]))
    for required in ("keyId", "signature"):
        if required not in params:
            raise SignatureError(f"Signature is missing {required}")
    return params


def signing_string(method: str, path: str, header_names: list[str], headers: Mapping[str, str]) -> str:
    lines = []
    for name in header_names:
        if name == "(request-target)":
            lines.append(f"(request-target): {method.lower()} {path}")
        else:
            value = headers.get(name)
            if value is None:
                raise SignatureError(f"Signed header {name!r} is missing")
            lines.append(f"{name}: {value}")
    return "\n".join(lines)


def check_digest(body: bytes, digest_header: str | None) -> None:
    if not digest_header:
        raise SignatureError("Digest header is missing")
    algorithm, _, value = digest_header.partition("=")
    if algorithm.upper() != "SHA-256":
        raise SignatureError(f"Unsupported digest algorithm {algorithm}")
    expected = base64.b64encode(hashlib.sha256(body).digest()).decode()
    if value != expected:
        raise SignatureError("Digest does not match the body")


def check_date(date_header: str | None, now: float | None = None) -> None:
    if not date_header:
        raise SignatureError("Date header is missing")
    try:
        sent = parsedate_to_datetime(date_header).timestamp()
    except (TypeError, ValueError) as exc:
        raise SignatureError("Date header is unreadable") from exc
    if abs((now if now is not None else time.time()) - sent) > MAX_CLOCK_SKEW_S:
        raise SignatureError("Request is too old (or the clock is wrong)")


async def verify_request(
    method: str,
    path: str,
    headers: Mapping[str, str],
    body: bytes,
    fetch_key: KeyFetcher,
) -> None:
    """Raise SignatureError unless the request carries a valid SmartThings signature.

    `headers` must be case-insensitive (Starlette's Headers is).
    """
    params = parse_signature_header(headers.get("authorization", ""))
    if params.get("algorithm", "rsa-sha256").lower() != "rsa-sha256":
        raise SignatureError(f"Unsupported algorithm {params.get('algorithm')}")

    header_names = params.get("headers", "date").lower().split()
    # Without these the signature would not cover the body or the time.
    for required in ("digest", "date"):
        if required not in header_names:
            raise SignatureError(f"Signature does not cover {required}")

    check_digest(body, headers.get("digest"))
    check_date(headers.get("date"))

    message = signing_string(method, path, header_names, headers).encode()
    try:
        signature = base64.b64decode(params["signature"])
    except ValueError as exc:
        raise SignatureError("Signature is not base64") from exc

    key = await fetch_key(params["keyId"])
    try:
        key.verify(signature, message, padding.PKCS1v15(), hashes.SHA256())
    except InvalidSignature as exc:
        raise SignatureError("Signature does not verify") from exc


class SmartThingsKeyCache:
    """Fetches and caches SmartThings' signing certificates by keyId."""

    def __init__(self, http: httpx.AsyncClient, key_base: str) -> None:
        self._http = http
        self._key_base = key_base.rstrip("/")
        self._keys: dict[str, rsa.RSAPublicKey] = {}

    async def __call__(self, key_id: str) -> rsa.RSAPublicKey:
        cached = self._keys.get(key_id)
        if cached is not None:
            return cached
        # keyId is a path on key.smartthings.com; never let it point elsewhere.
        if not key_id.startswith("/") or ".." in key_id or "//" in key_id:
            raise SignatureError(f"Unexpected keyId {key_id!r}")

        response = await self._http.get(f"{self._key_base}{key_id}", timeout=10)
        if response.status_code != 200:
            raise SignatureError(f"Could not fetch key {key_id}: HTTP {response.status_code}")
        try:
            certificate = x509.load_pem_x509_certificate(response.content)
        except ValueError as exc:
            raise SignatureError("Signing key is not a PEM certificate") from exc
        key = certificate.public_key()
        if not isinstance(key, rsa.RSAPublicKey):
            raise SignatureError("Signing key is not RSA")
        self._keys[key_id] = key
        log.info("Cached SmartThings signing key %s", key_id)
        return key

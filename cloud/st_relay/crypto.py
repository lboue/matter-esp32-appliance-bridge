"""Secrets handling: bridge secrets are stored hashed, OAuth tokens encrypted."""

from __future__ import annotations

import hashlib
import hmac
import secrets

from cryptography.fernet import Fernet


def new_bridge_secret() -> str:
    return secrets.token_urlsafe(32)


def new_code() -> str:
    return secrets.token_urlsafe(24)


def hash_secret(secret: str) -> str:
    # Bridge secrets are 256-bit random values, so a plain hash is enough; a
    # slow KDF would only add latency to every WebSocket connect.
    return hashlib.sha256(secret.encode()).hexdigest()


def secrets_equal(a: str, b: str) -> bool:
    return hmac.compare_digest(a.encode(), b.encode())


class TokenCipher:
    """Encrypts OAuth tokens at rest so a leaked database is not a leaked account."""

    def __init__(self, key: str) -> None:
        self._fernet = Fernet(key.encode())

    def encrypt(self, value: str) -> str:
        return self._fernet.encrypt(value.encode()).decode()

    def decrypt(self, value: str) -> str:
        return self._fernet.decrypt(value.encode()).decode()

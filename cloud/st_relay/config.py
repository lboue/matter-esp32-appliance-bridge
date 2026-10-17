"""Settings, read from the environment (see .env.example)."""

from __future__ import annotations

import os
from dataclasses import dataclass


def _bool(value: str | None, default: bool) -> bool:
    if value is None or value == "":
        return default
    return value.strip().lower() in ("1", "true", "yes", "on")


@dataclass(frozen=True)
class Settings:
    public_url: str
    client_id: str
    client_secret: str
    scope: str
    database_path: str
    token_encryption_key: str
    enroll_token: str | None
    verify_signatures: bool
    api_base: str = "https://api.smartthings.com"
    key_base: str = "https://key.smartthings.com"

    @property
    def redirect_uri(self) -> str:
        return f"{self.public_url}/oauth/callback"

    @property
    def webhook_url(self) -> str:
        return f"{self.public_url}/smartthings/webhook"

    @classmethod
    def from_env(cls) -> "Settings":
        missing = [
            name
            for name in ("PUBLIC_URL", "ST_CLIENT_ID", "ST_CLIENT_SECRET", "TOKEN_ENCRYPTION_KEY")
            if not os.environ.get(name)
        ]
        if missing:
            raise RuntimeError(f"Missing required environment variables: {', '.join(missing)}")

        return cls(
            public_url=os.environ["PUBLIC_URL"].rstrip("/"),
            client_id=os.environ["ST_CLIENT_ID"],
            client_secret=os.environ["ST_CLIENT_SECRET"],
            # r: read devices; x: execute commands. The bridge reads status and
            # sends washer commands with the token this service hands it.
            scope=os.environ.get("ST_SCOPE", "r:devices:* x:devices:*"),
            database_path=os.environ.get("DATABASE_PATH", "st-relay.db"),
            token_encryption_key=os.environ["TOKEN_ENCRYPTION_KEY"],
            enroll_token=os.environ.get("ENROLL_TOKEN") or None,
            # Only ever disable this for local testing with hand-made requests.
            verify_signatures=_bool(os.environ.get("ST_VERIFY_SIGNATURES"), True),
        )

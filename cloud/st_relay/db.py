"""SQLite persistence.

The load is a handful of bridges, so this uses the stdlib sqlite3 module
directly with one connection guarded by a lock. Calls are short and made from
the event loop; none of them do network I/O.
"""

from __future__ import annotations

import sqlite3
import threading
import time
from dataclasses import dataclass

SCHEMA = """
CREATE TABLE IF NOT EXISTS bridges (
    id            TEXT PRIMARY KEY,
    secret_hash   TEXT NOT NULL UNIQUE,
    name          TEXT NOT NULL DEFAULT '',
    created_at    INTEGER NOT NULL,
    last_seen_at  INTEGER
);

CREATE TABLE IF NOT EXISTS links (
    bridge_id          TEXT PRIMARY KEY REFERENCES bridges(id) ON DELETE CASCADE,
    installed_app_id   TEXT NOT NULL UNIQUE,
    location_id        TEXT,
    access_token_enc   TEXT NOT NULL,
    refresh_token_enc  TEXT NOT NULL,
    expires_at         INTEGER NOT NULL,
    scopes             TEXT NOT NULL DEFAULT ''
);

CREATE TABLE IF NOT EXISTS link_codes (
    code        TEXT PRIMARY KEY,
    bridge_id   TEXT NOT NULL REFERENCES bridges(id) ON DELETE CASCADE,
    expires_at  INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS oauth_states (
    state       TEXT PRIMARY KEY,
    bridge_id   TEXT NOT NULL REFERENCES bridges(id) ON DELETE CASCADE,
    expires_at  INTEGER NOT NULL
);

-- The SmartThings devices a bridge has bridged, as last reported by the bridge.
CREATE TABLE IF NOT EXISTS bridge_devices (
    bridge_id   TEXT NOT NULL REFERENCES bridges(id) ON DELETE CASCADE,
    device_id   TEXT NOT NULL,
    PRIMARY KEY (bridge_id, device_id)
);

-- Subscriptions created on the bridge's installed app, one per device.
CREATE TABLE IF NOT EXISTS subscriptions (
    bridge_id        TEXT NOT NULL REFERENCES bridges(id) ON DELETE CASCADE,
    device_id        TEXT NOT NULL,
    subscription_id  TEXT NOT NULL,
    PRIMARY KEY (bridge_id, device_id)
);
"""


def now() -> int:
    return int(time.time())


@dataclass
class Link:
    bridge_id: str
    installed_app_id: str
    location_id: str | None
    access_token_enc: str
    refresh_token_enc: str
    expires_at: int
    scopes: str


class Database:
    def __init__(self, path: str) -> None:
        self._conn = sqlite3.connect(path, check_same_thread=False, isolation_level=None)
        self._conn.row_factory = sqlite3.Row
        self._lock = threading.Lock()
        with self._lock:
            self._conn.execute("PRAGMA foreign_keys = ON")
            self._conn.execute("PRAGMA journal_mode = WAL")
            self._conn.executescript(SCHEMA)

    def close(self) -> None:
        self._conn.close()

    def _exec(self, sql: str, params: tuple = ()) -> sqlite3.Cursor:
        with self._lock:
            return self._conn.execute(sql, params)

    def _one(self, sql: str, params: tuple = ()) -> sqlite3.Row | None:
        with self._lock:
            return self._conn.execute(sql, params).fetchone()

    def _all(self, sql: str, params: tuple = ()) -> list[sqlite3.Row]:
        with self._lock:
            return self._conn.execute(sql, params).fetchall()

    # ---- Bridges ----

    def create_bridge(self, bridge_id: str, secret_hash: str, name: str) -> None:
        self._exec(
            "INSERT INTO bridges (id, secret_hash, name, created_at) VALUES (?, ?, ?, ?)",
            (bridge_id, secret_hash, name, now()),
        )

    def bridge_by_secret_hash(self, secret_hash: str) -> sqlite3.Row | None:
        return self._one("SELECT * FROM bridges WHERE secret_hash = ?", (secret_hash,))

    def touch_bridge(self, bridge_id: str) -> None:
        self._exec("UPDATE bridges SET last_seen_at = ? WHERE id = ?", (now(), bridge_id))

    def delete_bridge(self, bridge_id: str) -> None:
        self._exec("DELETE FROM bridges WHERE id = ?", (bridge_id,))

    # ---- One-time link codes and OAuth state ----

    def add_link_code(self, code: str, bridge_id: str, ttl_s: int) -> None:
        self._exec("DELETE FROM link_codes WHERE expires_at < ?", (now(),))
        self._exec(
            "INSERT INTO link_codes (code, bridge_id, expires_at) VALUES (?, ?, ?)",
            (code, bridge_id, now() + ttl_s),
        )

    def consume_link_code(self, code: str) -> str | None:
        with self._lock:
            row = self._conn.execute(
                "SELECT bridge_id, expires_at FROM link_codes WHERE code = ?", (code,)
            ).fetchone()
            self._conn.execute("DELETE FROM link_codes WHERE code = ?", (code,))
        if row is None or row["expires_at"] < now():
            return None
        return row["bridge_id"]

    def add_oauth_state(self, state: str, bridge_id: str, ttl_s: int) -> None:
        self._exec("DELETE FROM oauth_states WHERE expires_at < ?", (now(),))
        self._exec(
            "INSERT INTO oauth_states (state, bridge_id, expires_at) VALUES (?, ?, ?)",
            (state, bridge_id, now() + ttl_s),
        )

    def consume_oauth_state(self, state: str) -> str | None:
        with self._lock:
            row = self._conn.execute(
                "SELECT bridge_id, expires_at FROM oauth_states WHERE state = ?", (state,)
            ).fetchone()
            self._conn.execute("DELETE FROM oauth_states WHERE state = ?", (state,))
        if row is None or row["expires_at"] < now():
            return None
        return row["bridge_id"]

    # ---- Links (OAuth tokens per bridge) ----

    def upsert_link(self, link: Link) -> None:
        with self._lock:
            # An installed app belongs to one bridge: re-linking the same
            # SmartThings account to another bridge moves it.
            self._conn.execute(
                "DELETE FROM links WHERE installed_app_id = ? AND bridge_id != ?",
                (link.installed_app_id, link.bridge_id),
            )
            self._conn.execute(
                """
                INSERT INTO links (bridge_id, installed_app_id, location_id, access_token_enc,
                                   refresh_token_enc, expires_at, scopes)
                VALUES (?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT(bridge_id) DO UPDATE SET
                    installed_app_id = excluded.installed_app_id,
                    location_id = excluded.location_id,
                    access_token_enc = excluded.access_token_enc,
                    refresh_token_enc = excluded.refresh_token_enc,
                    expires_at = excluded.expires_at,
                    scopes = excluded.scopes
                """,
                (
                    link.bridge_id,
                    link.installed_app_id,
                    link.location_id,
                    link.access_token_enc,
                    link.refresh_token_enc,
                    link.expires_at,
                    link.scopes,
                ),
            )

    def _link(self, row: sqlite3.Row | None) -> Link | None:
        return Link(**dict(row)) if row is not None else None

    def link_for_bridge(self, bridge_id: str) -> Link | None:
        return self._link(self._one("SELECT * FROM links WHERE bridge_id = ?", (bridge_id,)))

    def link_for_installed_app(self, installed_app_id: str) -> Link | None:
        return self._link(
            self._one("SELECT * FROM links WHERE installed_app_id = ?", (installed_app_id,))
        )

    def links_expiring_before(self, timestamp: int) -> list[Link]:
        return [
            Link(**dict(r))
            for r in self._all("SELECT * FROM links WHERE expires_at < ?", (timestamp,))
        ]

    def delete_link(self, bridge_id: str) -> None:
        with self._lock:
            self._conn.execute("DELETE FROM links WHERE bridge_id = ?", (bridge_id,))
            self._conn.execute("DELETE FROM subscriptions WHERE bridge_id = ?", (bridge_id,))

    # ---- Devices and subscriptions ----

    def set_bridge_devices(self, bridge_id: str, device_ids: list[str]) -> None:
        with self._lock:
            self._conn.execute("DELETE FROM bridge_devices WHERE bridge_id = ?", (bridge_id,))
            self._conn.executemany(
                "INSERT INTO bridge_devices (bridge_id, device_id) VALUES (?, ?)",
                [(bridge_id, d) for d in sorted(set(device_ids))],
            )

    def bridge_devices(self, bridge_id: str) -> set[str]:
        return {
            r["device_id"]
            for r in self._all("SELECT device_id FROM bridge_devices WHERE bridge_id = ?", (bridge_id,))
        }

    def subscriptions(self, bridge_id: str) -> dict[str, str]:
        return {
            r["device_id"]: r["subscription_id"]
            for r in self._all(
                "SELECT device_id, subscription_id FROM subscriptions WHERE bridge_id = ?", (bridge_id,)
            )
        }

    def add_subscription(self, bridge_id: str, device_id: str, subscription_id: str) -> None:
        self._exec(
            "INSERT OR REPLACE INTO subscriptions (bridge_id, device_id, subscription_id) VALUES (?, ?, ?)",
            (bridge_id, device_id, subscription_id),
        )

    def remove_subscription(self, bridge_id: str, device_id: str) -> None:
        self._exec(
            "DELETE FROM subscriptions WHERE bridge_id = ? AND device_id = ?", (bridge_id, device_id)
        )

"""Core relay logic shared by the HTTP, webhook and WebSocket routes."""

from __future__ import annotations

import asyncio
import logging
from typing import Any

from .config import Settings
from .crypto import TokenCipher
from .db import Database, Link, now
from .hub import Hub
from .smartthings import SmartThings, SmartThingsError, TokenSet

log = logging.getLogger(__name__)

# Refresh tokens this long before they expire. SmartThings access tokens last
# 24h; the refresh token rotates on every use and lasts ~30 days.
REFRESH_MARGIN_S = 2 * 3600
REFRESH_CHECK_INTERVAL_S = 600


class Relay:
    def __init__(self, settings: Settings, db: Database, cipher: TokenCipher, st: SmartThings, hub: Hub) -> None:
        self.settings = settings
        self.db = db
        self.cipher = cipher
        self.st = st
        self.hub = hub
        # Serialises token refreshes and subscription changes per bridge.
        self._locks: dict[str, asyncio.Lock] = {}

    def _lock(self, bridge_id: str) -> asyncio.Lock:
        return self._locks.setdefault(bridge_id, asyncio.Lock())

    # ---- Tokens ----

    def _store_tokens(self, bridge_id: str, tokens: TokenSet, installed_app_id: str, location_id: str | None) -> Link:
        link = Link(
            bridge_id=bridge_id,
            installed_app_id=installed_app_id,
            location_id=location_id,
            access_token_enc=self.cipher.encrypt(tokens.access_token),
            refresh_token_enc=self.cipher.encrypt(tokens.refresh_token),
            expires_at=now() + tokens.expires_in,
            scopes=tokens.scope,
        )
        self.db.upsert_link(link)
        return link

    async def complete_link(self, bridge_id: str, tokens: TokenSet) -> None:
        """Store the tokens from an OAuth callback and bring the bridge up to date."""
        if not tokens.installed_app_id:
            raise SmartThingsError(200, "Token response did not include installed_app_id")
        location_id = await self.st.installed_app_location(tokens.installed_app_id, tokens.access_token)
        async with self._lock(bridge_id):
            # Subscriptions from a previous link belong to another installed
            # app and are gone with it.
            self.db.delete_link(bridge_id)
            self._store_tokens(bridge_id, tokens, tokens.installed_app_id, location_id)
        log.info("Bridge %s linked to installed app %s", bridge_id, tokens.installed_app_id)

        await self.hub.send(bridge_id, {"type": "linked", "locationId": location_id})
        await self.push_token(bridge_id)
        await self.reconcile_subscriptions(bridge_id)

    async def access_token(self, bridge_id: str) -> tuple[str, int] | None:
        """A valid access token and its remaining lifetime, refreshing if due."""
        link = self.db.link_for_bridge(bridge_id)
        if link is None:
            return None
        if link.expires_at - now() < REFRESH_MARGIN_S:
            link = await self.refresh(bridge_id)
            if link is None:
                return None
        return self.cipher.decrypt(link.access_token_enc), link.expires_at - now()

    async def refresh(self, bridge_id: str) -> Link | None:
        async with self._lock(bridge_id):
            link = self.db.link_for_bridge(bridge_id)
            if link is None:
                return None
            if link.expires_at - now() >= REFRESH_MARGIN_S:
                return link  # someone else refreshed while we waited

            try:
                tokens = await self.st.refresh(self.cipher.decrypt(link.refresh_token_enc))
            except SmartThingsError as exc:
                if exc.status in (400, 401):
                    log.warning("Refresh token for bridge %s rejected; unlinking", bridge_id)
                    self.db.delete_link(bridge_id)
                    await self.hub.send(
                        bridge_id,
                        {"type": "unlinked", "reason": "SmartThings rejected the refresh token. Link again."},
                    )
                    return None
                log.error("Token refresh for bridge %s failed: %s", bridge_id, exc)
                return link if link.expires_at > now() else None

            link = self._store_tokens(bridge_id, tokens, link.installed_app_id, link.location_id)
            log.info("Refreshed tokens for bridge %s", bridge_id)

        await self.push_token(bridge_id)
        return link

    async def push_token(self, bridge_id: str) -> None:
        token = await self.access_token(bridge_id)
        if token is not None:
            access_token, expires_in = token
            await self.hub.send(bridge_id, {"type": "token", "accessToken": access_token, "expiresIn": expires_in})

    async def refresh_loop(self) -> None:
        while True:
            try:
                for link in self.db.links_expiring_before(now() + REFRESH_MARGIN_S):
                    await self.refresh(link.bridge_id)
            except Exception:  # noqa: BLE001 - keep the loop alive
                log.exception("Token refresh pass failed")
            await asyncio.sleep(REFRESH_CHECK_INTERVAL_S)

    # ---- Subscriptions ----

    async def set_devices(self, bridge_id: str, device_ids: list[str]) -> None:
        self.db.set_bridge_devices(bridge_id, [d for d in device_ids if isinstance(d, str) and d])
        await self.reconcile_subscriptions(bridge_id)

    async def reconcile_subscriptions(self, bridge_id: str) -> None:
        """Make the installed app's subscriptions match the bridge's devices."""
        token = await self.access_token(bridge_id)
        link = self.db.link_for_bridge(bridge_id)
        if token is None or link is None:
            return
        access_token, _ = token

        async with self._lock(bridge_id):
            wanted = self.db.bridge_devices(bridge_id)
            existing = self.db.subscriptions(bridge_id)

            for device_id in sorted(wanted - existing.keys()):
                try:
                    sub_id = await self.st.create_device_subscription(
                        link.installed_app_id, access_token, device_id
                    )
                    self.db.add_subscription(bridge_id, device_id, sub_id)
                    log.info("Subscribed bridge %s to device %s", bridge_id, device_id)
                except SmartThingsError as exc:
                    log.error("Subscribing to %s failed: %s", device_id, exc)

            for device_id in sorted(existing.keys() - wanted):
                try:
                    await self.st.delete_subscription(link.installed_app_id, access_token, existing[device_id])
                    self.db.remove_subscription(bridge_id, device_id)
                    log.info("Unsubscribed bridge %s from device %s", bridge_id, device_id)
                except SmartThingsError as exc:
                    log.error("Unsubscribing from %s failed: %s", device_id, exc)

    async def unlink(self, bridge_id: str) -> None:
        token = await self.access_token(bridge_id)
        link = self.db.link_for_bridge(bridge_id)
        if token is not None and link is not None:
            for sub_id in self.db.subscriptions(bridge_id).values():
                try:
                    await self.st.delete_subscription(link.installed_app_id, token[0], sub_id)
                except SmartThingsError as exc:
                    log.warning("Could not delete subscription %s: %s", sub_id, exc)
        self.db.delete_link(bridge_id)
        await self.hub.send(bridge_id, {"type": "unlinked", "reason": "Unlinked"})

    # ---- Events ----

    async def forward_events(self, installed_app_id: str, events: list[dict[str, Any]]) -> int:
        link = self.db.link_for_installed_app(installed_app_id)
        if link is None:
            log.info("Event for unknown installed app %s ignored", installed_app_id)
            return 0

        forwarded = 0
        for event in events:
            if event.get("eventType") != "DEVICE_EVENT":
                continue
            device_event = event.get("deviceEvent") or {}
            message = {
                "type": "event",
                "deviceId": device_event.get("deviceId"),
                "componentId": device_event.get("componentId"),
                "capability": device_event.get("capability"),
                "attribute": device_event.get("attribute"),
                "value": device_event.get("value"),
            }
            if await self.hub.send(link.bridge_id, message):
                forwarded += 1
        return forwarded

"""Live WebSocket connections from bridges."""

from __future__ import annotations

import asyncio
import logging
from typing import Any

from fastapi import WebSocket

log = logging.getLogger(__name__)


class Hub:
    def __init__(self) -> None:
        self._sockets: dict[str, WebSocket] = {}
        self._lock = asyncio.Lock()

    async def register(self, bridge_id: str, ws: WebSocket) -> None:
        async with self._lock:
            old = self._sockets.get(bridge_id)
            self._sockets[bridge_id] = ws
        if old is not None:
            # A bridge that reconnects before its old socket times out
            # replaces it; only one connection per bridge is kept.
            log.info("Bridge %s reconnected; closing its previous socket", bridge_id)
            try:
                await old.close(code=4000, reason="replaced")
            except Exception:  # noqa: BLE001 - already dead
                pass

    async def unregister(self, bridge_id: str, ws: WebSocket) -> None:
        async with self._lock:
            if self._sockets.get(bridge_id) is ws:
                del self._sockets[bridge_id]

    def is_connected(self, bridge_id: str) -> bool:
        return bridge_id in self._sockets

    async def send(self, bridge_id: str, message: dict[str, Any]) -> bool:
        """Send to a bridge if it is connected. Returns False if it is not."""
        ws = self._sockets.get(bridge_id)
        if ws is None:
            return False
        try:
            await ws.send_json(message)
            return True
        except Exception as exc:  # noqa: BLE001 - socket died mid-send
            log.info("Send to bridge %s failed: %s", bridge_id, exc)
            await self.unregister(bridge_id, ws)
            return False

    async def close(self, bridge_id: str, code: int = 1000, reason: str = "") -> None:
        ws = self._sockets.get(bridge_id)
        if ws is not None:
            await self.unregister(bridge_id, ws)
            try:
                await ws.close(code=code, reason=reason)
            except Exception:  # noqa: BLE001
                pass

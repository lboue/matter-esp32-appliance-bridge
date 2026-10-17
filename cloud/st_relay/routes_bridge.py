"""Endpoints the bridge firmware calls, and its WebSocket."""

from __future__ import annotations

import json
import logging
import time
import uuid
from collections import defaultdict, deque

from fastapi import APIRouter, Depends, Header, HTTPException, Request, WebSocket, WebSocketDisconnect
from pydantic import BaseModel, Field

from .crypto import hash_secret, new_bridge_secret, new_code, secrets_equal
from .relay import Relay

log = logging.getLogger(__name__)

router = APIRouter()

LINK_CODE_TTL_S = 600
ENROLL_LIMIT = 5
ENROLL_WINDOW_S = 3600

_enroll_attempts: dict[str, deque[float]] = defaultdict(deque)


def relay_from(request: Request) -> Relay:
    return request.app.state.relay


def _check_enroll_rate(ip: str) -> None:
    window = _enroll_attempts[ip]
    cutoff = time.monotonic() - ENROLL_WINDOW_S
    while window and window[0] < cutoff:
        window.popleft()
    if len(window) >= ENROLL_LIMIT:
        raise HTTPException(429, "Too many enrolments from this address; try again later")
    window.append(time.monotonic())


def _bridge_id_for_secret(relay: Relay, authorization: str | None) -> str | None:
    if not authorization or not authorization.startswith("Bearer "):
        return None
    row = relay.db.bridge_by_secret_hash(hash_secret(authorization[len("Bearer ") :].strip()))
    return row["id"] if row is not None else None


def authenticated_bridge(request: Request, authorization: str | None = Header(default=None)) -> str:
    bridge_id = _bridge_id_for_secret(relay_from(request), authorization)
    if bridge_id is None:
        raise HTTPException(401, "Unknown bridge credentials")
    return bridge_id


class EnrollRequest(BaseModel):
    name: str = Field(default="", max_length=64)
    enrollToken: str | None = Field(default=None, max_length=256)


@router.post("/api/bridges")
async def enroll(body: EnrollRequest, request: Request) -> dict:
    relay = relay_from(request)
    _check_enroll_rate(request.client.host if request.client else "unknown")

    expected = relay.settings.enroll_token
    if expected is not None and not secrets_equal(body.enrollToken or "", expected):
        raise HTTPException(403, "Enrolment token is missing or wrong")

    bridge_id = str(uuid.uuid4())
    secret = new_bridge_secret()
    relay.db.create_bridge(bridge_id, hash_secret(secret), body.name)
    log.info("Enrolled bridge %s (%s)", bridge_id, body.name)
    # The secret is only ever returned here.
    return {"bridgeId": bridge_id, "secret": secret}


@router.get("/api/bridges/me")
async def bridge_status(request: Request, bridge_id: str = Depends(authenticated_bridge)) -> dict:
    relay = relay_from(request)
    link = relay.db.link_for_bridge(bridge_id)
    return {
        "bridgeId": bridge_id,
        "linked": link is not None,
        "locationId": link.location_id if link else None,
        "devices": sorted(relay.db.bridge_devices(bridge_id)),
        "subscribed": sorted(relay.db.subscriptions(bridge_id).keys()),
    }


@router.post("/api/bridges/me/link-code")
async def link_code(request: Request, bridge_id: str = Depends(authenticated_bridge)) -> dict:
    relay = relay_from(request)
    code = new_code()
    relay.db.add_link_code(code, bridge_id, LINK_CODE_TTL_S)
    return {"url": f"{relay.settings.public_url}/link/{code}", "expiresIn": LINK_CODE_TTL_S}


@router.delete("/api/bridges/me/link")
async def unlink(request: Request, bridge_id: str = Depends(authenticated_bridge)) -> dict:
    await relay_from(request).unlink(bridge_id)
    return {"ok": True}


@router.delete("/api/bridges/me")
async def delete_bridge(request: Request, bridge_id: str = Depends(authenticated_bridge)) -> dict:
    relay = relay_from(request)
    await relay.unlink(bridge_id)
    await relay.hub.close(bridge_id, code=4001, reason="deleted")
    relay.db.delete_bridge(bridge_id)
    return {"ok": True}


@router.websocket("/ws")
async def bridge_socket(ws: WebSocket) -> None:
    relay: Relay = ws.app.state.relay
    bridge_id = _bridge_id_for_secret(relay, ws.headers.get("authorization"))
    if bridge_id is None:
        await ws.close(code=4401, reason="unauthorized")
        return

    await ws.accept()
    await relay.hub.register(bridge_id, ws)
    relay.db.touch_bridge(bridge_id)
    log.info("Bridge %s connected", bridge_id)

    try:
        link = relay.db.link_for_bridge(bridge_id)
        await ws.send_json({"type": "state", "linked": link is not None})
        if link is not None:
            await relay.push_token(bridge_id)

        while True:
            raw = await ws.receive_text()
            try:
                message = json.loads(raw)
            except json.JSONDecodeError:
                continue
            kind = message.get("type") if isinstance(message, dict) else None

            if kind == "devices" and isinstance(message.get("deviceIds"), list):
                await relay.set_devices(bridge_id, message["deviceIds"][:50])
            elif kind == "token":
                # The bridge asks for a token, e.g. after a 401 from SmartThings.
                await relay.push_token(bridge_id)
            elif kind == "hello":
                log.info("Bridge %s says hello (fw %s)", bridge_id, message.get("fw"))
            relay.db.touch_bridge(bridge_id)
    except WebSocketDisconnect:
        pass
    finally:
        await relay.hub.unregister(bridge_id, ws)
        log.info("Bridge %s disconnected", bridge_id)

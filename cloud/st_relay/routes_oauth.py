"""The browser half of linking: SmartThings' OAuth authorization code flow."""

from __future__ import annotations

import logging

from fastapi import APIRouter, Request
from fastapi.responses import HTMLResponse, RedirectResponse

from .crypto import new_code
from .relay import Relay
from .smartthings import SmartThingsError
from .templates import page

log = logging.getLogger(__name__)

router = APIRouter()

OAUTH_STATE_TTL_S = 600


@router.get("/link/{code}")
async def start_link(code: str, request: Request):
    relay: Relay = request.app.state.relay
    bridge_id = relay.db.consume_link_code(code)
    if bridge_id is None:
        return HTMLResponse(
            page("Link expired", "This link has expired or was already used. Start again from the bridge.", ok=False),
            status_code=410,
        )
    state = new_code()
    relay.db.add_oauth_state(state, bridge_id, OAUTH_STATE_TTL_S)
    return RedirectResponse(relay.st.authorize_url(state), status_code=302)


@router.get("/oauth/callback")
async def oauth_callback(request: Request, code: str | None = None, state: str | None = None, error: str | None = None):
    relay: Relay = request.app.state.relay

    bridge_id = relay.db.consume_oauth_state(state) if state else None
    if bridge_id is None:
        return HTMLResponse(
            page("Link expired", "This sign-in has expired. Start again from the bridge.", ok=False), status_code=400
        )
    if error or not code:
        return HTMLResponse(
            page("Not linked", f"SmartThings did not authorise the bridge ({error or 'no code'}).", ok=False),
            status_code=400,
        )

    try:
        tokens = await relay.st.exchange_code(code)
        await relay.complete_link(bridge_id, tokens)
    except SmartThingsError as exc:
        log.error("Linking bridge %s failed: %s", bridge_id, exc)
        return HTMLResponse(
            page("Not linked", "SmartThings refused the sign-in. Check the app registration and try again.", ok=False),
            status_code=502,
        )

    return HTMLResponse(
        page("SmartThings linked", "The bridge can now see your SmartThings devices. You can close this tab.")
    )

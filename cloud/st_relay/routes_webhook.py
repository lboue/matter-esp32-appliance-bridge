"""The SmartThings target URL: lifecycle requests and device events."""

from __future__ import annotations

import json
import logging

from fastapi import APIRouter, HTTPException, Request

from .relay import Relay
from .signatures import SignatureError, verify_request

log = logging.getLogger(__name__)

router = APIRouter()


@router.post("/smartthings/webhook")
async def webhook(request: Request) -> dict:
    relay: Relay = request.app.state.relay
    body = await request.body()

    if relay.settings.verify_signatures:
        try:
            await verify_request("POST", request.url.path, request.headers, body, request.app.state.key_fetcher)
        except SignatureError as exc:
            log.warning("Rejected webhook: %s", exc)
            raise HTTPException(401, "Invalid signature") from exc

    try:
        payload = json.loads(body)
    except json.JSONDecodeError as exc:
        raise HTTPException(400, "Body is not JSON") from exc

    lifecycle = payload.get("lifecycle")

    if lifecycle == "PING":
        return {"pingData": {"challenge": payload.get("pingData", {}).get("challenge")}}

    if lifecycle == "CONFIRMATION":
        url = payload.get("confirmationData", {}).get("confirmationUrl", "")
        log.info("Confirming target URL for app %s", payload.get("confirmationData", {}).get("appId"))
        try:
            await relay.st.confirm_target(url)
        except Exception as exc:  # noqa: BLE001
            log.error("Target URL confirmation failed: %s", exc)
            raise HTTPException(502, "Confirmation failed") from exc
        return {"targetUrl": relay.settings.webhook_url}

    if lifecycle == "EVENT":
        data = payload.get("eventData", {})
        installed_app_id = data.get("installedApp", {}).get("installedAppId", "")
        forwarded = await relay.forward_events(installed_app_id, data.get("events", []))
        log.debug("Forwarded %d event(s) for %s", forwarded, installed_app_id)
        return {"eventData": {}}

    # UNINSTALL and anything newer: acknowledge.
    if lifecycle == "UNINSTALL":
        installed_app_id = payload.get("uninstallData", {}).get("installedApp", {}).get("installedAppId", "")
        link = relay.db.link_for_installed_app(installed_app_id)
        if link is not None:
            log.info("Installed app %s uninstalled; unlinking bridge %s", installed_app_id, link.bridge_id)
            relay.db.delete_link(link.bridge_id)
            await relay.hub.send(link.bridge_id, {"type": "unlinked", "reason": "Removed in SmartThings"})
        return {"uninstallData": {}}

    log.info("Ignoring lifecycle %s", lifecycle)
    return {}

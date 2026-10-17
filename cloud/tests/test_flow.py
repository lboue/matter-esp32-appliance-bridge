"""End to end: enrol, link, subscribe, relay events, refresh, unlink."""

from __future__ import annotations

import json
from urllib.parse import parse_qs, urlparse

import pytest

from .conftest import sign_request

WEBHOOK = "/smartthings/webhook"


def enroll(client) -> tuple[str, dict[str, str]]:
    response = client.post("/api/bridges", json={"name": "test bridge"})
    assert response.status_code == 200
    body = response.json()
    return body["bridgeId"], {"Authorization": f"Bearer {body['secret']}"}


def link(client, auth) -> None:
    url = client.post("/api/bridges/me/link-code", headers=auth).json()["url"]
    path = urlparse(url).path
    redirect = client.get(path, follow_redirects=False)
    assert redirect.status_code == 302
    location = urlparse(redirect.headers["location"])
    assert location.netloc == "api.smartthings.com"
    query = parse_qs(location.query)
    assert query["redirect_uri"] == ["https://relay.example.com/oauth/callback"]
    assert query["scope"] == ["r:devices:* x:devices:*"]

    callback = client.get("/oauth/callback", params={"code": "auth-code", "state": query["state"][0]})
    assert callback.status_code == 200
    assert "linked" in callback.text


def device_event(device_id: str, capability: str, attribute: str, value) -> dict:
    return {
        "eventType": "DEVICE_EVENT",
        "deviceEvent": {
            "deviceId": device_id,
            "componentId": "main",
            "capability": capability,
            "attribute": attribute,
            "value": value,
            "stateChange": True,
        },
    }


def post_event(client, signing_key, installed_app_id: str, events: list[dict]):
    body = json.dumps(
        {
            "lifecycle": "EVENT",
            "eventData": {"installedApp": {"installedAppId": installed_app_id}, "events": events},
        }
    ).encode()
    return client.post(WEBHOOK, content=body, headers=sign_request(signing_key, WEBHOOK, body))


def test_bad_credentials_are_refused(client):
    assert client.get("/api/bridges/me", headers={"Authorization": "Bearer nope"}).status_code == 401
    with pytest.raises(Exception):
        with client.websocket_connect("/ws", headers={"Authorization": "Bearer nope"}) as ws:
            ws.receive_json()


def test_link_codes_are_single_use(client):
    _, auth = enroll(client)
    path = urlparse(client.post("/api/bridges/me/link-code", headers=auth).json()["url"]).path
    assert client.get(path, follow_redirects=False).status_code == 302
    assert client.get(path, follow_redirects=False).status_code == 410


def test_full_flow(client, fake_st, signing_key):
    bridge_id, auth = enroll(client)

    with client.websocket_connect("/ws", headers=auth) as ws:
        assert ws.receive_json() == {"type": "state", "linked": False}

        # Devices reported before linking are remembered...
        ws.send_json({"type": "devices", "deviceIds": ["washer-1"]})

        link(client, auth)
        assert ws.receive_json() == {"type": "linked", "locationId": "loc-1"}
        token = ws.receive_json()
        assert token["type"] == "token" and token["accessToken"] == "access-1"

        # ...and subscribed to once the link exists.
        status = client.get("/api/bridges/me", headers=auth).json()
        assert status["linked"] is True
        assert status["subscribed"] == ["washer-1"]
        (subscription,) = fake_st.subscriptions.values()
        assert subscription["device"]["deviceId"] == "washer-1"
        assert subscription["device"]["capability"] == "*"

        # Events for the installed app are relayed to the bridge.
        response = post_event(
            client,
            signing_key,
            "ia-1",
            [device_event("washer-1", "washerOperatingState", "machineState", "run")],
        )
        assert response.status_code == 200
        assert ws.receive_json() == {
            "type": "event",
            "deviceId": "washer-1",
            "componentId": "main",
            "capability": "washerOperatingState",
            "attribute": "machineState",
            "value": "run",
        }

        # Removing the device from the bridge removes the subscription.
        ws.send_json({"type": "devices", "deviceIds": []})
        ws.send_json({"type": "token"})  # round-trip so the previous message has been handled
        assert ws.receive_json()["type"] == "token"
        assert fake_st.subscriptions == {}

    # Unlinking forgets the tokens.
    assert client.delete("/api/bridges/me/link", headers=auth).json() == {"ok": True}
    assert client.get("/api/bridges/me", headers=auth).json()["linked"] is False


def test_tokens_are_encrypted_at_rest(client):
    bridge_id, auth = enroll(client)
    link(client, auth)
    row = client.app.state.relay.db.link_for_bridge(bridge_id)
    assert "access-1" not in row.access_token_enc
    assert "refresh-1" not in row.refresh_token_enc


def test_token_is_refreshed_when_close_to_expiry(client, fake_st):
    bridge_id, auth = enroll(client)
    link(client, auth)
    db = client.app.state.relay.db
    db._exec("UPDATE links SET expires_at = 0 WHERE bridge_id = ?", (bridge_id,))

    with client.websocket_connect("/ws", headers=auth) as ws:
        assert ws.receive_json() == {"type": "state", "linked": True}
        token = ws.receive_json()
        # Rotated: the refresh produced the second token set.
        assert token["accessToken"] == "access-2"

    assert db.link_for_bridge(bridge_id).expires_at > 0
    refreshes = [r for r in fake_st.requests if r.url.path == "/oauth/token" and b"refresh_token" in r.content]
    assert refreshes and b"refresh-1" in refreshes[0].content


def test_rejected_refresh_unlinks(client, fake_st):
    bridge_id, auth = enroll(client)
    link(client, auth)
    db = client.app.state.relay.db
    db._exec("UPDATE links SET expires_at = 0 WHERE bridge_id = ?", (bridge_id,))
    fake_st.refresh_status = 401

    with client.websocket_connect("/ws", headers=auth) as ws:
        assert ws.receive_json() == {"type": "state", "linked": True}
        message = ws.receive_json()
        assert message["type"] == "unlinked"

    assert db.link_for_bridge(bridge_id) is None


def test_events_for_unknown_apps_are_acknowledged(client, signing_key):
    response = post_event(client, signing_key, "someone-else", [device_event("d", "switch", "switch", "on")])
    assert response.status_code == 200
    assert response.json() == {"eventData": {}}


def test_enroll_token_is_enforced(tmp_path, fake_st):
    import httpx
    from fastapi.testclient import TestClient

    from st_relay.main import create_app

    from .conftest import make_settings

    app = create_app(
        settings=make_settings(tmp_path, enroll_token="s3cret"),
        http=httpx.AsyncClient(transport=httpx.MockTransport(fake_st)),
        run_refresh_loop=False,
    )
    with TestClient(app) as c:
        assert c.post("/api/bridges", json={"name": "x"}).status_code == 403
        assert c.post("/api/bridges", json={"name": "x", "enrollToken": "wrong"}).status_code == 403
        assert c.post("/api/bridges", json={"name": "x", "enrollToken": "s3cret"}).status_code == 200

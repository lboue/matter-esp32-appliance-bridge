from __future__ import annotations

import json
from email.utils import formatdate

import pytest

from st_relay.signatures import SignatureError, verify_request

from .conftest import sign_request

PATH = "/smartthings/webhook"


class Headers(dict):
    """Case-insensitive enough for verify_request (it looks up lower-case names)."""

    def __init__(self, raw: dict[str, str]) -> None:
        super().__init__({k.lower(): v for k, v in raw.items()})


def key_fetcher(signing_key):
    async def fetch(key_id: str):
        return signing_key.public_key()

    return fetch


async def test_valid_signature_verifies(signing_key):
    body = json.dumps({"lifecycle": "PING"}).encode()
    headers = Headers(sign_request(signing_key, PATH, body))
    await verify_request("POST", PATH, headers, body, key_fetcher(signing_key))


async def test_tampered_body_is_rejected(signing_key):
    body = json.dumps({"lifecycle": "PING"}).encode()
    headers = Headers(sign_request(signing_key, PATH, body))
    with pytest.raises(SignatureError, match="Digest"):
        await verify_request("POST", PATH, headers, body + b" ", key_fetcher(signing_key))


async def test_wrong_path_is_rejected(signing_key):
    body = b"{}"
    headers = Headers(sign_request(signing_key, "/elsewhere", body))
    with pytest.raises(SignatureError, match="does not verify"):
        await verify_request("POST", PATH, headers, body, key_fetcher(signing_key))


async def test_stale_date_is_rejected(signing_key):
    body = b"{}"
    old = formatdate(timeval=0, usegmt=True)
    headers = Headers(sign_request(signing_key, PATH, body, date=old))
    with pytest.raises(SignatureError, match="too old"):
        await verify_request("POST", PATH, headers, body, key_fetcher(signing_key))


async def test_missing_signature_is_rejected(signing_key):
    with pytest.raises(SignatureError):
        await verify_request("POST", PATH, Headers({}), b"{}", key_fetcher(signing_key))


def test_webhook_rejects_unsigned_request(client):
    response = client.post(PATH, json={"lifecycle": "PING", "pingData": {"challenge": "x"}})
    assert response.status_code == 401


def test_webhook_accepts_signed_ping(client, signing_key):
    body = json.dumps({"lifecycle": "PING", "pingData": {"challenge": "abc"}}).encode()
    response = client.post(PATH, content=body, headers=sign_request(signing_key, PATH, body))
    assert response.status_code == 200
    assert response.json() == {"pingData": {"challenge": "abc"}}


def test_webhook_confirmation_follows_smartthings_url(client, signing_key, fake_st):
    url = "https://api.smartthings.com/apps/app-1/confirm-registration?token=t"
    body = json.dumps(
        {"lifecycle": "CONFIRMATION", "confirmationData": {"appId": "app-1", "confirmationUrl": url}}
    ).encode()
    response = client.post(PATH, content=body, headers=sign_request(signing_key, PATH, body))
    assert response.status_code == 200
    assert response.json() == {"targetUrl": "https://relay.example.com/smartthings/webhook"}
    assert any(str(r.url) == url for r in fake_st.requests)


def test_webhook_confirmation_refuses_foreign_url(client, signing_key):
    body = json.dumps(
        {"lifecycle": "CONFIRMATION", "confirmationData": {"confirmationUrl": "https://evil.example/confirm"}}
    ).encode()
    response = client.post(PATH, content=body, headers=sign_request(signing_key, PATH, body))
    assert response.status_code == 502

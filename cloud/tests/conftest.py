from __future__ import annotations

import base64
import datetime
import hashlib
import json
from email.utils import formatdate

import httpx
import pytest
from cryptography import x509
from cryptography.fernet import Fernet
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa
from cryptography.x509.oid import NameOID
from fastapi.testclient import TestClient

from st_relay.config import Settings
from st_relay.main import create_app

KEY_ID = "/pl/useast2/test-key"


@pytest.fixture(scope="session")
def signing_key() -> rsa.RSAPrivateKey:
    return rsa.generate_private_key(public_exponent=65537, key_size=2048)


@pytest.fixture(scope="session")
def signing_cert_pem(signing_key) -> bytes:
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "test.smartthings.com")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(signing_key.public_key())
        .serial_number(1)
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=1))
        .sign(signing_key, hashes.SHA256())
    )
    return cert.public_bytes(serialization.Encoding.PEM)


def sign_request(key: rsa.RSAPrivateKey, path: str, body: bytes, date: str | None = None) -> dict[str, str]:
    """Headers for a request signed the way SmartThings signs webhook calls."""
    digest = "SHA-256=" + base64.b64encode(hashlib.sha256(body).digest()).decode()
    date = date or formatdate(usegmt=True)
    message = f"(request-target): post {path}\ndigest: {digest}\ndate: {date}".encode()
    signature = base64.b64encode(key.sign(message, padding.PKCS1v15(), hashes.SHA256())).decode()
    return {
        "Content-Type": "application/json",
        "Digest": digest,
        "Date": date,
        "Authorization": (
            f'Signature keyId="{KEY_ID}",signature="{signature}",'
            'headers="(request-target) digest date",algorithm="rsa-sha256"'
        ),
    }


class FakeSmartThings:
    """Answers the SmartThings API calls the relay makes, and records them."""

    def __init__(self, cert_pem: bytes) -> None:
        self.cert_pem = cert_pem
        self.requests: list[httpx.Request] = []
        self.refresh_status = 200
        self.token_counter = 0
        self.subscriptions: dict[str, dict] = {}

    def tokens(self) -> dict:
        self.token_counter += 1
        return {
            "access_token": f"access-{self.token_counter}",
            "refresh_token": f"refresh-{self.token_counter}",
            "expires_in": 86399,
            "installed_app_id": "ia-1",
            "scope": "r:devices:* x:devices:*",
        }

    def __call__(self, request: httpx.Request) -> httpx.Response:
        self.requests.append(request)
        path = request.url.path

        if request.url.host == "key.smartthings.com":
            return httpx.Response(200, content=self.cert_pem)

        if path == "/oauth/token":
            form = dict(x.split("=", 1) for x in request.content.decode().split("&"))
            if form["grant_type"] == "refresh_token" and self.refresh_status != 200:
                return httpx.Response(self.refresh_status, json={"error": "invalid_grant"})
            return httpx.Response(200, json=self.tokens())

        if path == "/v1/installedapps/ia-1":
            return httpx.Response(200, json={"installedAppId": "ia-1", "locationId": "loc-1"})

        if path == "/v1/installedapps/ia-1/subscriptions" and request.method == "POST":
            body = json.loads(request.content)
            sub_id = f"sub-{len(self.subscriptions) + 1}"
            self.subscriptions[sub_id] = body
            return httpx.Response(200, json={"id": sub_id, **body})

        if path.startswith("/v1/installedapps/ia-1/subscriptions/") and request.method == "DELETE":
            self.subscriptions.pop(path.rsplit("/", 1)[1], None)
            return httpx.Response(200, json={"count": 1})

        if request.url.host.endswith("smartthings.com") and "confirm" in path:
            return httpx.Response(200, json={})

        return httpx.Response(404, json={"error": "unexpected", "path": path})


def make_settings(tmp_path, **overrides) -> Settings:
    values = dict(
        public_url="https://relay.example.com",
        client_id="client-id",
        client_secret="client-secret",
        scope="r:devices:* x:devices:*",
        database_path=str(tmp_path / "test.db"),
        token_encryption_key=Fernet.generate_key().decode(),
        enroll_token=None,
        verify_signatures=True,
    )
    values.update(overrides)
    return Settings(**values)


@pytest.fixture
def fake_st(signing_cert_pem) -> FakeSmartThings:
    return FakeSmartThings(signing_cert_pem)


@pytest.fixture
def client(tmp_path, fake_st):
    settings = make_settings(tmp_path)
    http = httpx.AsyncClient(transport=httpx.MockTransport(fake_st))
    app = create_app(settings=settings, http=http, run_refresh_loop=False)
    with TestClient(app) as test_client:
        yield test_client


@pytest.fixture(autouse=True)
def reset_rate_limits():
    from st_relay import routes_bridge

    routes_bridge._enroll_attempts.clear()

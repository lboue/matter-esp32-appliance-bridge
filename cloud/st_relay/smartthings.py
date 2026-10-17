"""SmartThings OAuth and subscription API calls."""

from __future__ import annotations

import logging
import re
from dataclasses import dataclass
from urllib.parse import urlencode

import httpx

from .config import Settings

log = logging.getLogger(__name__)


class SmartThingsError(Exception):
    def __init__(self, status: int, body: str) -> None:
        super().__init__(f"SmartThings returned HTTP {status}: {body[:200]}")
        self.status = status
        self.body = body


@dataclass
class TokenSet:
    access_token: str
    refresh_token: str
    expires_in: int
    installed_app_id: str | None
    scope: str


class SmartThings:
    def __init__(self, settings: Settings, http: httpx.AsyncClient) -> None:
        self._settings = settings
        self._http = http

    # ---- OAuth ----

    def authorize_url(self, state: str) -> str:
        query = urlencode(
            {
                "client_id": self._settings.client_id,
                "response_type": "code",
                "redirect_uri": self._settings.redirect_uri,
                "scope": self._settings.scope,
                "state": state,
            }
        )
        return f"{self._settings.api_base}/oauth/authorize?{query}"

    async def _token_request(self, form: dict[str, str]) -> TokenSet:
        # SmartThings wants the client credentials as HTTP Basic auth.
        response = await self._http.post(
            f"{self._settings.api_base}/oauth/token",
            data=form,
            auth=(self._settings.client_id, self._settings.client_secret),
            timeout=15,
        )
        if response.status_code != 200:
            raise SmartThingsError(response.status_code, response.text)
        body = response.json()
        return TokenSet(
            access_token=body["access_token"],
            refresh_token=body["refresh_token"],
            expires_in=int(body.get("expires_in", 86400)),
            installed_app_id=body.get("installed_app_id"),
            scope=body.get("scope", ""),
        )

    async def exchange_code(self, code: str) -> TokenSet:
        return await self._token_request(
            {
                "grant_type": "authorization_code",
                "code": code,
                "redirect_uri": self._settings.redirect_uri,
                "client_id": self._settings.client_id,
            }
        )

    async def refresh(self, refresh_token: str) -> TokenSet:
        return await self._token_request(
            {
                "grant_type": "refresh_token",
                "refresh_token": refresh_token,
                "client_id": self._settings.client_id,
            }
        )

    # ---- Installed app ----

    async def installed_app_location(self, installed_app_id: str, access_token: str) -> str | None:
        response = await self._http.get(
            f"{self._settings.api_base}/v1/installedapps/{installed_app_id}",
            headers={"Authorization": f"Bearer {access_token}"},
            timeout=15,
        )
        if response.status_code != 200:
            log.warning("Could not read installed app %s: HTTP %s", installed_app_id, response.status_code)
            return None
        return response.json().get("locationId")

    # ---- Subscriptions ----

    @staticmethod
    def subscription_name(device_id: str) -> str:
        # Names must be unique per installed app and are limited in length and
        # character set, so derive one from the device ID.
        return "dev_" + re.sub(r"[^A-Za-z0-9]", "", device_id)[:32]

    async def create_device_subscription(
        self, installed_app_id: str, access_token: str, device_id: str
    ) -> str:
        response = await self._http.post(
            f"{self._settings.api_base}/v1/installedapps/{installed_app_id}/subscriptions",
            headers={"Authorization": f"Bearer {access_token}"},
            json={
                "sourceType": "DEVICE",
                "device": {
                    "deviceId": device_id,
                    "componentId": "main",
                    "capability": "*",
                    "attribute": "*",
                    "stateChangeOnly": True,
                    "subscriptionName": self.subscription_name(device_id),
                },
            },
            timeout=15,
        )
        if response.status_code != 200:
            raise SmartThingsError(response.status_code, response.text)
        return response.json()["id"]

    async def delete_subscription(
        self, installed_app_id: str, access_token: str, subscription_id: str
    ) -> None:
        response = await self._http.delete(
            f"{self._settings.api_base}/v1/installedapps/{installed_app_id}/subscriptions/{subscription_id}",
            headers={"Authorization": f"Bearer {access_token}"},
            timeout=15,
        )
        # Already gone is fine.
        if response.status_code not in (200, 204, 404):
            raise SmartThingsError(response.status_code, response.text)

    async def confirm_target(self, confirmation_url: str) -> None:
        # Only follow confirmation links that point at SmartThings.
        if not re.match(r"^https://[A-Za-z0-9.-]+\.smartthings\.com/", confirmation_url):
            raise ValueError(f"Refusing to confirm non-SmartThings URL {confirmation_url!r}")
        response = await self._http.get(confirmation_url, timeout=15)
        if response.status_code >= 300:
            raise SmartThingsError(response.status_code, response.text)

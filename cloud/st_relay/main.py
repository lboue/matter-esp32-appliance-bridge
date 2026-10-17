"""Application factory and entry point."""

from __future__ import annotations

import asyncio
import logging
import os
from contextlib import asynccontextmanager

import httpx
from fastapi import FastAPI

from . import routes_bridge, routes_oauth, routes_webhook
from .config import Settings
from .crypto import TokenCipher
from .db import Database
from .hub import Hub
from .relay import Relay
from .signatures import KeyFetcher, SmartThingsKeyCache
from .smartthings import SmartThings

log = logging.getLogger("st_relay")


def create_app(
    settings: Settings | None = None,
    http: httpx.AsyncClient | None = None,
    key_fetcher: KeyFetcher | None = None,
    run_refresh_loop: bool = True,
) -> FastAPI:
    """Build the app. Tests pass their own settings, HTTP client and key fetcher."""

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        cfg = settings or Settings.from_env()
        client = http or httpx.AsyncClient(headers={"User-Agent": "st-relay/0.1"})
        db = Database(cfg.database_path)
        relay = Relay(cfg, db, TokenCipher(cfg.token_encryption_key), SmartThings(cfg, client), Hub())
        app.state.relay = relay
        app.state.key_fetcher = key_fetcher or SmartThingsKeyCache(client, cfg.key_base)

        task = asyncio.create_task(relay.refresh_loop()) if run_refresh_loop else None
        log.info("st-relay ready at %s", cfg.public_url)
        try:
            yield
        finally:
            if task is not None:
                task.cancel()
            if http is None:
                await client.aclose()
            db.close()

    app = FastAPI(title="st-relay", lifespan=lifespan, docs_url=None, redoc_url=None)
    app.include_router(routes_bridge.router)
    app.include_router(routes_oauth.router)
    app.include_router(routes_webhook.router)

    @app.get("/healthz")
    async def healthz() -> dict:
        return {"ok": True}

    return app


def run() -> None:
    import uvicorn

    logging.basicConfig(level=os.environ.get("LOG_LEVEL", "INFO"), format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    uvicorn.run(
        create_app(),
        host=os.environ.get("HOST", "0.0.0.0"),
        port=int(os.environ.get("PORT", "8000")),
        # Behind Caddy: trust its X-Forwarded-* so rate limiting sees real IPs.
        proxy_headers=True,
        forwarded_allow_ips=os.environ.get("FORWARDED_ALLOW_IPS", "*"),
        # Keep idle bridge sockets alive through NAT and the proxy.
        ws_ping_interval=30,
        ws_ping_timeout=30,
    )


if __name__ == "__main__":
    run()

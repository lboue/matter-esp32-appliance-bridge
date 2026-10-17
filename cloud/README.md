# st-relay

A small, self-hostable service that connects the Matter appliance bridge to
SmartThings. The bridge can't do these two things itself:

- **Sign-in.** SmartThings only offers the OAuth authorization code flow. It
  needs an HTTPS redirect, and refreshing a token needs the client secret.
- **Real-time events.** SmartThings delivers device events as signed webhook
  POSTs to a public URL.

st-relay keeps each bridge's SmartThings tokens (encrypted) and refreshes them
before they expire. It subscribes to the devices each bridge has bridged and
relays their events over a WebSocket the bridge keeps open. It also pushes the
bridge a fresh access token, so the bridge calls `api.smartthings.com` directly
for status and commands.

```
SmartThings ──webhook──▶ st-relay ──WebSocket──▶ bridge (ESP32-S3)
     ▲                      │                        │
     └──── OAuth, subscriptions                      │
     └──────────────── REST: status, commands ───────┘
```

One instance serves any number of bridges, each linked to its own SmartThings
account.

## Deploying (e.g. an AWS Lightsail VM)

You need a DNS name pointing at the VM, with ports 80 and 443 open. Caddy gets
the TLS certificate automatically.

1. Install Docker and the compose plugin, then copy this `cloud/` folder to the VM.
2. `cp .env.example .env`, then set `RELAY_DOMAIN`, `PUBLIC_URL` and
   `TOKEN_ENCRYPTION_KEY`. Set `ENROLL_TOKEN` too if the instance is reachable
   by others. Leave the SmartThings fields empty for now.
3. `docker compose up -d --build`, then check `https://<domain>/healthz`.
   The service refuses to start without `ST_CLIENT_ID` and `ST_CLIENT_SECRET`,
   so fill in placeholders until step 4 gives you the real values. It must be
   running before step 4, because SmartThings confirms the target URL as the
   app is created.
4. Register the SmartThings app (next section), put its client ID and secret
   in `.env`, and run `docker compose up -d` again.

The SQLite database lives in the `relay-data` volume. Back it up together with
`TOKEN_ENCRYPTION_KEY`: without the key, the stored tokens can't be read.

## Registering the SmartThings app

Use the [SmartThings CLI](https://github.com/SmartThingsCommunity/smartthings-cli):

```sh
smartthings apps:create
```

- **App type:** OAuth-In App
- **Target URL:** `https://<domain>/smartthings/webhook`. Events and lifecycle
  calls are delivered here.
- **Scopes:** `r:devices:*` and `x:devices:*`
- **Redirect URI:** `https://<domain>/oauth/callback`

The CLI prints an OAuth client ID and secret; they're only shown once.
SmartThings then sends a `CONFIRMATION` request to the target URL, and st-relay
confirms it automatically. If that failed (for example because the service
wasn't running yet), re-send it with `smartthings apps:register <appId>`.

## How a bridge connects

All of this is driven from the bridge's web UI (SmartThings tab). Under the
hood:

1. **Enrol.** `POST /api/bridges {"name", "enrollToken"?}` returns
   `{"bridgeId", "secret"}`. The bridge stores the secret, and only its hash
   is kept here.
2. **Link.** `POST /api/bridges/me/link-code` (with `Authorization: Bearer
   <secret>`) returns a one-time URL, valid for 10 minutes. Opening it in a
   browser goes through SmartThings sign-in and comes back to `/oauth/callback`.
3. **Stay connected.** The bridge holds `GET /ws` (WebSocket, same bearer
   secret) open.

Messages from st-relay to the bridge:

| `type`     | Fields                                                   | When |
|------------|----------------------------------------------------------|------|
| `state`    | `linked`                                                 | on connect |
| `linked`   | `locationId`                                             | after sign-in completes |
| `token`    | `accessToken`, `expiresIn`                               | on connect, after each refresh, on request |
| `event`    | `deviceId`, `componentId`, `capability`, `attribute`, `value` | each SmartThings device event |
| `unlinked` | `reason`                                                 | refresh rejected, app removed, or unlinked |

Messages from the bridge to st-relay:

| `type`    | Fields      | Meaning |
|-----------|-------------|---------|
| `hello`   | `fw`        | informational |
| `devices` | `deviceIds` | the SmartThings devices the bridge has bridged. st-relay keeps one subscription per device. |
| `token`   |             | send me a token now (e.g. after a 401) |

Events are not queued. If a bridge is offline, it re-reads device status from
SmartThings when it reconnects.

Other endpoints: `GET /api/bridges/me` (link and subscription status),
`DELETE /api/bridges/me/link` (unlink), `DELETE /api/bridges/me` (forget the
bridge).

## Security notes

- Every webhook request's HTTP Signature is verified against SmartThings'
  published key. The `Digest` must match the body, and the `Date` must be
  within 5 minutes.
- Tokens are Fernet-encrypted at rest. Bridge secrets are stored as SHA-256
  hashes.
- Link codes and OAuth `state` values are single-use and expire after 10 minutes.
- Enrolment is rate-limited per IP. Set `ENROLL_TOKEN` to stop strangers
  enrolling on your instance.

## Development

```sh
uv venv && uv pip install -e '.[dev]'
.venv/bin/pytest
```

To run it locally against hand-made webhook requests, set
`ST_VERIFY_SIGNATURES=false` (never in production), then run
`PUBLIC_URL=http://localhost:8000 ... .venv/bin/st-relay`.

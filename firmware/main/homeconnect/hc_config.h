#pragma once

// HomeConnect API endpoints. The simulator is a separate host that accepts the
// same client registrations (with simulator-specific client IDs).
#define HC_API_BASE_PRODUCTION "https://api.home-connect.com"
#define HC_API_BASE_SIMULATOR "https://simulator.home-connect.com"

// IdentifyAppliance: list appliances. Monitor: status + events. Control: start,
// stop and select programs. Settings: power state.
#define HC_OAUTH_SCOPE "IdentifyAppliance Monitor Control Settings"

// HomeConnect's media type for API requests/responses.
#define HC_MEDIA_TYPE "application/vnd.bsh.sdk.v1+json"

// Access and refresh tokens are JWTs of roughly 1-1.5KB.
#define HC_MAX_TOKEN_LEN 2048
#define HC_MAX_CLIENT_ID_LEN 128
#define HC_MAX_CLIENT_SECRET_LEN 128

// haIds look like "BOSCH-SMV68TX06E-68A40E000000".
#define HC_HAID_LEN 48
#define HC_NAME_LEN 64
#define HC_SHORT_LEN 40

#define HC_MAX_APPLIANCES 8
#define HC_MAX_PROGRAMS 24
#define HC_PROGRAM_KEY_LEN 80

// Refresh the access token this long before it expires.
#define HC_TOKEN_REFRESH_MARGIN_S 300

// The event stream sends KEEP-ALIVE every 55s. Reconnect if nothing arrives
// for this long.
#define HC_SSE_IDLE_TIMEOUT_S 75
#define HC_SSE_BACKOFF_MIN_S 5
#define HC_SSE_BACKOFF_MAX_S 300

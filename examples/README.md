# C++20 MCP examples

These examples use **cpp-httplib + jsoncons + OpenSSL** (no Boost). They use the public `mymcp` C++ SDK, and are built by default with CMake. All HTTP servers bind to `127.0.0.1`.

> **Scope:** The sample programs exercise the SDK's currently implemented MCP `2026-07-28` subset. They do not imply full specification conformance. `oauth_resource_server` intentionally uses a **development-only** shared token, not a production OAuth verifier.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

On Windows (Visual Studio), add `--config Debug` to `cmake --build` and `-C Debug` to `ctest`, and use executables from `build/Debug/`. Dependencies are downloaded via CMake's pinned FetchContent revisions on the first configure.

## Feature coverage

| Feature | Example |
| --- | --- |
| Tools with automatic input Schema, typed argument extraction | `showcase_server`, `showcase_client` (`add`, `hello`) |
| Explicit JSON Schema input constraints | `showcase_server` (`rate`) |
| Tool discovery with paginated listing and caching hints | `showcase_client` (`list_all`) |
| Resources, URI templates | `showcase_server` (`demo://status`, `demo://notes/{id}`) |
| Prompts with arguments, completions | `showcase_server` (`review`) |
| Progress notifications and cooperative cancellation | `showcase_server` (`count`), `showcase_client` (stdio) |
| MRTR `input_required` + client-supplied responses | `showcase_server` (`confirm_release`), `showcase_client` |
| Background Tasks extension, task polling | `showcase_server` (`generate_report`), `showcase_client` |
| `subscriptions/listen` and SSE resource-update notifications | `showcase_subscribe` + `emit_update` tool |
| Custom methods and extension capability advertisement | `showcase_server` (`example/echo`) |
| OAuth discovery, PKCE S256, dynamic registration, browser callback, token exchange and refresh | `oauth_login` (requires a real provider) |
| OAuth Protected Resource Metadata, Bearer enforcement callback | `oauth_resource_server` (DEVELOPMENT ONLY) |

## 1. All-in-one stdio example

Run from the build output directory (Windows: use `.exe`):

```sh
./mymcp_showcase_client --stdio ./mymcp_showcase_server
```

The client launches the server itself, discovers capabilities, enumerates all paginated tools, calls typed tools, reads resources, gets prompts and completions, handles a non-destructive MRTR approval request, reports stdio progress, and polls a Task until completed. It prints `SHOWCASE_OK` when finished.

`showcase_client` automatically approves its **example-only** `confirm_release` tool. In real applications, get explicit human approval rather than automatically accepting elicitation requests.

## 2. HTTP and SSE examples

In terminal A:

```sh
./mymcp_showcase_server --http 3000
```

In terminal B:

```sh
./mymcp_showcase_client --http http://127.0.0.1:3000/mcp
./mymcp_showcase_subscribe http://127.0.0.1:3000/mcp
```

The SSE example subscribes to `demo://status`, receives the acknowledgement, invokes `emit_update` through a separate client, receives `notifications/resources/updated`, and closes the stream. `showcase_subscribe` exits with `SUBSCRIPTION_OK` on success.

**Current limitation:** progress notifications are shown through stdio only; HTTP request-level SSE progress support is not complete. Subscription SSE and regular HTTP RPC are supported by this example.

## 3. OAuth client: authorization code + PKCE (real provider)

This example is for a **real OAuth 2.1 authorization server**, not the fixed-token demo resource server. The endpoint must expose OAuth Protected Resource Metadata (RFC 9728), authorization-server discovery (RFC 8414 or OIDC), and authorize/token endpoints.

```sh
# Option A: pre-registered native client ID
./mymcp_oauth_login https://mcp.example.com/mcp --client-id YOUR_PUBLIC_CLIENT_ID --scope tools:read

# Option B: dynamic client registration (only if provider advertises RFC 7591)
./mymcp_oauth_login https://mcp.example.com/mcp --register --scope tools:read
```

The example performs:

1. Protected Resource Metadata and authorization-server metadata discovery, validating the expected issuer.
2. Loopback callback listener on an OS-allocated `127.0.0.1` port and, if selected, dynamic native-client registration.
3. PKCE S256 + CSRF `state` + OAuth `resource` indicator construction.
4. Displays the authorization URL for you to open in a browser; receives the browser redirect locally for up to 180 seconds.
5. Validates redirect, `state` and `iss`, exchanges the code, and obtains an in-memory Token.
6. Uses `Client::http_oauth(...)` to discover tools; the session attaches and refreshes Bearer tokens as needed.

**Important:** For a *pre-registered* client ID, your authorization server must permit the OS-selected local loopback redirect port; otherwise use an appropriate pre-registered redirect strategy or the dynamic registration option. This sample does not persist credentials, does not automate scope escalation, and intentionally does not print tokens or PKCE verifiers. The browser still needs to be opened by the user.

### OAuth helper API (when your app has its own callback UI)

```cpp
#include "mymcp/mcp.hpp"
#include "mymcp/oauth_client.hpp"

auto session = std::make_shared<mymcp::oauth::Session>("https://mcp.example.com/mcp");
session->discover();
session->client_id("your-public-client-id");
auto flow = session->begin_authorization("http://127.0.0.1:8765/callback", {"tools:read"});
// Open flow.url using your application's UI. Receive the full callback URL.
session->exchange_callback_url(flow, callback_url, "http://127.0.0.1:8765/callback");
auto client = mymcp::Client::http_oauth("https://mcp.example.com/mcp", session);
auto tools = client.tools();
```

The snippet assumes `callback_url` is supplied by your host application.

## 4. Protected MCP resource server (DEVELOPMENT ONLY)

This tests SDK Protected Resource Metadata + Bearer challenge/verification hook:

```sh
# Do not use a real password or production OAuth token for this test.
export MYMCP_DEMO_TOKEN=local-example-secret
./mymcp_oauth_resource_server https://auth.example.com 3100
```

Windows PowerShell:

```powershell
$env:MYMCP_DEMO_TOKEN = "local-example-secret"
.\mymcp_oauth_resource_server.exe https://auth.example.com 3100
```

Protected resource metadata is available at `http://127.0.0.1:3100/.well-known/oauth-protected-resource`. Requests to `/mcp` require `Authorization: Bearer local-example-secret`; wrong or missing tokens receive HTTP 401. The issuer URL here is illustrative; **this server will not issue OAuth authorization codes or validate JWTs**. It is not intended to work as the authorization server for the `oauth_login` example.

**For production:** Replace the fixed-token comparison with trusted JWT/JWKS verification or OAuth introspection: validate signature, `iss`, resource/audience, expiration, token purpose and operation-specific scopes. Serve behind HTTPS, and provide a real identity provider. The verifier callback receives `(token, method, name)` for authorization decisions; see `SECURITY.md`.

## 5. Repeatable tests

`ctest` includes `showcase_examples` (stdio, HTTP, SSE) and `oauth_resource_example` (metadata, 401, permitted Bearer request). `oauth_session` tests a local mock authorization server for code exchange, callback checks and token refresh. No live external OAuth provider or secrets are required for CI.

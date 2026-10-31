# Standalone C++20 MCP demos (no command-line arguments)

Every file `run_xxxx_demo.cpp` defines `int run_xxxx_demo()` plus a zero-argument `int main()` that calls it. The corresponding CMake executable is **`run_xxxx_demo`** (Windows: `.exe`). Each demo focuses on one feature; no `--http`, `--stdio`, URL, port or credential command-line options are used.

Build once:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

For Visual Studio multi-config builds, add `--config Debug` to the build command, and `-C Debug` to `ctest`. Run demo executables from your build output directory. Most demos complete on their own; the three servers are designed to keep listening until terminated.

## One executable = one feature

| Run directly | What it demonstrates | Prerequisite |
| --- | --- | --- |
| `run_tools_demo` | Typed tools, auto JSON Schema, validation errors, pagination, discovery | None |
| `run_resources_demo` | Static resources, templates, cached resource lists | None |
| `run_prompts_demo` | Prompt arguments and completion | None |
| `run_mrtr_demo` | Multi round-trip user elicitation on a harmless mock approval | None |
| `run_tasks_demo` | Task creation, async execution and status polling | None |
| `run_extensions_demo` | Extension capability and custom method | None |
| `run_stdio_client_demo` | Launch `run_stdio_server_demo`, discover and call a tool over stdio | Automatically finds its companion executable |
| `run_stdio_server_demo` | Stdio MCP server reading JSON-RPC from stdin | Started by stdio client (or host application) |
| `run_progress_demo` | Report progress notifications from a stdio tool | Automatically starts stdio server |
| `run_http_server_demo` | Loopback HTTP server, tools, resources and publishing | Leave running in terminal 1 |
| `run_http_client_demo` | HTTP MCP client requests | Run HTTP server first |
| `run_subscriptions_demo` | SSE subscription, acknowledgement and resource-update event | Run HTTP server first |
| `run_oauth_demo` | **No-credential mock OAuth**: discovery, native registration, PKCE S256, callback, token exchange | None |
| `run_oauth_login_demo` | Real OAuth server with browser callback and bearer-protected MCP calls | Edit config in source, use a real OAuth provider |
| `run_oauth_resource_demo` | Protected Resource Metadata, 401 challenge, local-only fixed demo Bearer verifier | Leave running in terminal; **development only** |
| `run_oauth_resource_client_demo` | Send a mock Bearer token to the local protected resource server | Run `run_oauth_resource_demo` first |

### Start two HTTP examples with no arguments

Terminal 1:

```bash
./run_http_server_demo
```

Terminal 2:

```bash
./run_http_client_demo
./run_subscriptions_demo
```

The fixed local address/port is defined in `demo_common.hpp` (`http://127.0.0.1:33867/mcp`). Change that header to use another port; both server and clients will share it.

### OAuth walkthrough

Try `./run_oauth_demo` **first**. It starts an in-memory mock authorization server on a random loopback port, then runs protected resource metadata discovery, native dynamic registration, PKCE S256 construction, validates a simulated OAuth callback and exchanges the code. It needs **no real credentials** and does not present a real authorization UI.

For a **real** provider, edit the `configuration` constants at the top of `run_oauth_login_demo.cpp`:

```cpp
constexpr const char* mcp_resource = "https://your-mcp-host.example/mcp";
constexpr const char* public_client_id = "YOUR_PUBLIC_CLIENT_ID";
constexpr bool use_dynamic_registration = false;
constexpr const char* requested_scope = "tools:read";
```

Then rebuild and run `./run_oauth_login_demo`. It opens a local callback listener on an OS-selected port, prints an authorization URL for you to open, waits up to 180 seconds, validates callback state and issuer, exchanges the code and calls MCP with the OAuth session. No tokens are printed or saved; do not commit actual private credentials. The OAuth provider must allow the selected loopback redirect URI or support native dynamic registration.

`run_oauth_resource_demo` is **not an OAuth authorization server** and cannot be used to log in with `run_oauth_login_demo`. It demonstrates only the SDK's protected-resource metadata and Bearer validation hook. Its fixed token is for local security-shape testing, **not a production token verifier**. In real services, verify token signature/introspection, issuer, audience, expiry and scopes; deploy with HTTPS.

## Legacy integration test fixtures

The previous command-line-driven `showcase_*`, `oauth_login.cpp` and `oauth_resource_server.cpp` files have moved to `tests/fixtures/` to keep existing integration tests without exposing old parameter-driven examples. They are compiled only as test fixtures. All files in `examples/` now use the independent `run_xxxx_demo` entrypoint convention.

The SDK is still experimental; these demos do **not** establish full MCP 2026-07-28 conformance. See [`COMPATIBILITY.md`](../COMPATIBILITY.md).


### Streamable HTTP + request-scoped SSE

Start `run_http_server_demo`, then run `run_http_progress_demo` (no flags). The tool sends five real-time `notifications/progress` SSE events and a final JSON-RPC response; ordinary fast tools still return `application/json`. For Codex and Claude Code configuration see [`../INTEROPERABILITY.md`](../INTEROPERABILITY.md).

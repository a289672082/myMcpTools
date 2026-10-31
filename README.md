# myMcpTools

A C++20 MCP client and server library with convenient typed tool registration.

> **Status: experimental prototype, NOT certified against the full MCP specification.**
> Implements a subset of the MCP **2026-07-28** wire protocol. Do not use it as a production-facing unauthenticated service.

## Example (server)

```cpp
#include "mymcp/mcp.hpp"
using namespace mymcp;

int main() {
  Server server("calculator", "1.0.0");
  server.tool("add", "Add integers", [](int a, int b) { return a + b; },
              arg<int>("a", "First number"), arg<int>("b", "Second number"));
  server.resource("app://status", "Status", [] { return std::string("OK"); });
  server.prompt("welcome", "Welcome prompt", [](const Json&) { return "Hi!"; });
  server.run_stdio();                 // or server.run_http(3000)
}
```

## Example (client)

```cpp
#include "mymcp/mcp.hpp"
using namespace mymcp;

int main() {
  auto client = Client::process("./mymcp_example_server");
  auto info = client.discover();
  auto list = client.tools();
  auto answer = client.call("add", json::object{{"a", 1}, {"b", 2}});
}
```

For an HTTP client:

```cpp
auto client = mymcp::Client::http("http://127.0.0.1:3000/mcp");
```

## Standalone no-argument demos

See **[examples/README.md](examples/README.md)**. Every demonstration is a standalone executable named `run_xxxx_demo`, containing a matching `run_xxxx_demo()` function and a zero-argument `main()`. Examples include `run_tools_demo`, `run_resources_demo`, `run_prompts_demo`, `run_tasks_demo`, `run_mrtr_demo`, `run_stdio_client_demo` and `run_oauth_demo` (local mock OAuth). The old CLI-driven examples are retained only as integration test fixtures in `tests/fixtures/`.

## Build

Requirements (migration): C++20, CMake >= 3.20, OpenSSL, Threads, and a network connection for CMake to fetch pinned cpp-httplib and jsoncons sources. No Boost dependency.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Run from the build output directory (Windows: add `.exe`):

```sh
./run_tools_demo
./run_resources_demo
./run_oauth_demo  # local, no external credentials
```

To exercise HTTP and SSE, run `run_http_server_demo` in one terminal, then `run_http_client_demo` and `run_subscriptions_demo` in another. Nothing requires command-line parameters.

## Protocol coverage — October 2026

**Target: MCP specification 2026-07-28. Experimental SDK, NOT full conformance.**

Implemented and covered by local tests:

- Modern stateless request metadata and `resultType`, `server/discover`, legacy stdio fallback.
- Typed C++ tool registration, tool listing/calling, raw handlers for rich tool results.
- Resources and URI templates (simple `{variable}` expansion), full prompt-result handlers and completion.
- Deterministic listing and opaque cursor pagination for tools/resources/prompts/templates; caching `ttlMs` and `cacheScope` on all six required operations.
- MRTR `input_required` on valid operations, client-side callback and `requestState` round trips.
- `subscriptions/listen`: stdio acknowledgement and cancellation, HTTP long-lived SSE with acknowledgement/keepalive, per-subscription filters and explicit `Server::publish(...)`, `Client::listen_http(...)`.
- Custom method handler and extension-capability advertisement APIs.
- Streamable HTTP POST routing/header verification, `x-mcp-header` projection, origin checks, loopback-only listener. Request-scoped SSE response consumption on the client; JSON responses on the server.

Still incomplete (important before production use):

- Remote schema retrieval and schema-derived output validation, typed content types and URI template operators beyond simple variables. The JSON Schema validator now uses jsoncons with 2020-12 support for local schemas.
- OAuth gaps: an in-app browser/callback listener, secure persistent token storage, automatic authorization retry and incremental scope escalation, Client ID Metadata validation, and built-in JWT/JWKS verification. The client has discovery + PKCE + token/refresh flows; the server offers a Bearer verifier callback. The HTTP server binds loopback only; use a trusted HTTPS proxy for remote deployment.
- Legacy HTTP session/handshake compatibility, asynchronous request cancellation outside subscriptions, progress and log notifications, and custom timeout/retry configuration.
- Complete Tasks / Apps / EMA extension implementations (raw custom method dispatch and namespaced capability advertisement are available, but **do not** imply extension conformance).
- Request-scoped SSE output from tool handlers, full stdio asynchronous client subscription management, stream restart resilience, cross-platform concurrency load testing, full official SDK conformance/interop tests.

See [COMPATIBILITY.md](COMPATIBILITY.md) for a conservative per-feature matrix. Passing local tests is not an official compliance certificate.

## New API examples

```cpp
Server server("books", "1.0");
server.cache(60000, "private");
server.page_size(50);
server.resource_template("books://{id}", "Book",
  [](const std::string& uri, const Json& vars) -> Json {
    return json::object{{"contents", json::array{json::object{{"uri", uri},
      {"text", std::string(vars.at("id").as_string())}}}}};
  });
server.completion([](const Json&) -> Json {
  return json::object{{"completion", json::object{{"values", json::array{"cpp", "c++"}}, {"hasMore", false}}}};
});

// Consumers can transparently aggregate all pages:
auto all = client.list_all("tools/list", "tools");

// HTTP notifications: callback returns false to close the SSE connection.
Client::listen_http("http://127.0.0.1:3000/mcp", json::object{{"toolsListChanged", true}},
  [](const Json& event) {
    std::cout << json::serialize(event) << "\n";
    return true;
  });
```

For stdio/HTTP subscriptions, event producers call `server.publish("notifications/tools/list_changed")` or `server.publish("notifications/resources/updated", json::object{{"uri", "..."}})`. The SDK enforces the requested subscription filter and sends an acknowledgment first. Tool catalogs should be registered before starting the server; dynamic mutation of handler maps is not currently thread safe.

## Spec references

- [MCP 2026-07-28](https://modelcontextprotocol.io/specification/2026-07-28)
- [Discovery](https://modelcontextprotocol.io/specification/2026-07-28/server/discover)
- [Streamable HTTP](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http)
- [Tools](https://modelcontextprotocol.io/specification/2026-07-28/server/tools)

## Expanded APIs (October 2026)

Tasks are opt-in and are advertised to clients explicitly:

```cpp
server.enable_tasks();
server.add_task_tool("background", "Long-running work", {{"type", "object"}},
    [](const mymcp::Json& responses, std::atomic_bool& cancelled) -> mymcp::Json {
      if (cancelled) return {{"content", mymcp::json::array{}}};
      return mymcp::detail::make_tool_result("finished");
    });

mymcp::Client client = mymcp::Client::process("./my_server");
client.enable_tasks(); // must be called before discover()
auto handle = client.call("background");
auto state = client.get_task(std::string(handle.at("taskId").as_string()));
// client.update_task(taskId, inputResponses);  client.cancel_task(taskId);
```

The server task registry is process-local, **not durable across restarts**. HTTP tasks calls also validate the `Mcp-Name` header against `taskId`.

A stdio server may report progress from inside an active tool handler using `server.progress(value, total, "message")`, and honor cancellation using `server.is_cancelled()`; the client can use `request_with_progress` and a `Client::process` notification callback. HTTP request-scoped progress is not yet implemented.

For protected HTTP, `server.protect_http(protected_resource_metadata, token_verifier)` installs the metadata endpoint and authorization callback. The verifier must validate issuer, audience, expiry and required scopes. The test fixture server supports a local-only authentication test using `MYMCP_TEST_TOKEN`; new no-argument demo programs live in `examples/`. `Client::http("https://host/mcp", token)` validates TLS certificates/hostnames and includes a Bearer token. `mymcp/oauth_client.hpp` provides OAuth metadata discovery, PKCE S256, registration, token exchange and refresh. It does **not** independently implement interactive browser login or durable token storage.

All raw tool handlers are checked with jsoncons JSON Schema validation (see `include/mymcp/schema.hpp`). Schema violations return JSON-RPC `-32602`, not tool `isError`. Remote schema reference handling and official MCP conformance testing remain outstanding. See [COMPATIBILITY.md](COMPATIBILITY.md) and [SECURITY.md](SECURITY.md).

## Dependency migration (C++20 / cpp-httplib / jsoncons)

This experimental migration removes Boost headers/libraries from the SDK. CMake fetches
pinned `cpp-httplib` and `jsoncons` revisions at configuration time (network access required
on first configure); OpenSSL remains required for TLS and PKCE. JSON values are
`mymcp::Json` (`jsoncons::json`), so callers directly using `boost::json::value`
need to update their source. CMake package exporting vendored dependencies is still being refined.

OAuth client-side interface:

```cpp
#include "mymcp/mcp.hpp"
#include "mymcp/oauth_client.hpp"
auto session = std::make_shared<mymcp::oauth::Session>("https://mcp.example/mcp");
session->discover();
session->client_id("https://client.example/oauth-metadata.json");
auto authorization = session->begin_authorization("http://127.0.0.1:8900/callback", {"tools:read"});
// Open authorization.url in a browser, receive redirect code/state/iss through the app.
session->exchange_code(authorization, code, returned_state, returned_issuer,
                       "http://127.0.0.1:8900/callback");
auto client = mymcp::Client::http_oauth("https://mcp.example/mcp", session);
auto tools = client.tools();
```

The SDK does **not** host an OAuth Authorization Server or control the browser; OAuth
redirect handling must be implemented by the application. Token validation on the
server remains pluggable through `Server::protect_http(metadata, verifier)`, where
`verifier` is responsible for cryptographically validating the credential and enforcing
issuer, audience, expiration, subject and operation-level scopes.

The migration must pass Linux + Windows CI before it is treated as a release.

## Desktop OAuth callback helper

```cpp
auto session = std::make_shared<mymcp::oauth::Session>("https://mcp.example/mcp");
session->discover();
session->client_id("your-registered-client-id");
auto flow = session->begin_authorization("http://127.0.0.1:8765/callback");
// Open flow.url in the OS browser, then receive the redirect in your app.
// The helper checks the full redirect URI, state, issuer and percent-encoding.
session->exchange_callback_url(flow, received_callback_url,
                               "http://127.0.0.1:8765/callback");
auto client = mymcp::Client::http_oauth("https://mcp.example/mcp", session);
auto tools = client.tools();  // refreshes expiring tokens automatically
```

Dynamic registration for a desktop/CLI app uses `application_type=native` by default,
which is required for interoperability with some OpenID Connect servers. Web apps
can pass `"web"` explicitly to `register_dynamic(redirects, name, "web")`.
The host application still owns browser navigation, redirect listener and safe
credential storage; this is **not** a built-in authorization server.

## Independent, no-argument demos

`examples/` now contains single-purpose programs named `run_xxxx_demo.cpp`,
each with `run_xxxx_demo()` and a zero-argument `main()`. For example:

```sh
./run_tools_demo
./run_resources_demo
./run_prompts_demo
./run_tasks_demo
./run_mrtr_demo
./run_oauth_demo             # local mock provider, no credentials
```

For HTTP and SSE, launch `run_http_server_demo` in another terminal, then run
`run_http_client_demo` or `run_subscriptions_demo`. For a real provider, edit the
configuration constants in `examples/run_oauth_login_demo.cpp` and run it.
See [the demo guide](examples/README.md). Old parameterized executables are
retained only in `tests/fixtures/` for regression testing.

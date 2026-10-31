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
  server.prompt("welcome", "Welcome prompt", [](const json::object&) { return "Hi!"; });
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
  auto answer = client.call("add", {{"a", 1}, {"b", 2}});
}
```

For an HTTP client:

```cpp
auto client = mymcp::Client::http("http://127.0.0.1:3000/mcp");
```

## Build

Requirements: C++20, CMake >= 3.20, Boost >= 1.83 (headers, incl. JSON/Beast/Asio/Process), Threads. Boost.JSON is compiled using its `src.hpp` header; a separate Boost JSON library is not required.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Run the server and client from the `build` directory:

```bash
./mymcp_example_client
./mymcp_example_server --http   # in another terminal
./mymcp_example_client http://127.0.0.1:3000/mcp
```

On Windows use the `.exe` variants (and adjust the child executable path in the example). The HTTP server accepts an optional port: `--http 4000`.

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

- Full JSON Schema 2020-12 validator, schema-derived output checking, argument constraints, typed content types and resource URI template operators beyond simple variables.
- Full OAuth 2.1 client/resource server: protected-resource metadata, AS discovery, CIMD, PKCE, issuer validation, scope escalation, token refresh and audience checking. This library currently offers **no authentication**. HTTP binds loopback only and does not support native HTTPS; terminate TLS and enforce authentication in a trusted reverse proxy if required.
- Legacy HTTP session/handshake compatibility, asynchronous request cancellation outside subscriptions, progress and log notifications, and custom timeout/retry configuration.
- Complete Tasks / Apps / EMA extension implementations (raw custom method dispatch and namespaced capability advertisement are available, but **do not** imply extension conformance).
- Request-scoped SSE output from tool handlers, full stdio asynchronous client subscription management, stream restart resilience, cross-platform concurrency load testing, full official SDK conformance/interop tests.

See [COMPATIBILITY.md](COMPATIBILITY.md) for a conservative per-feature matrix. Five local tests passing is not an official compliance certificate.

## New API examples

```cpp
Server server("books", "1.0");
server.cache(60000, "private");
server.page_size(50);
server.resource_template("books://{id}", "Book",
  [](const std::string& uri, const json::object& vars) -> Json {
    return {{"contents", json::array{{{"uri", uri},
      {"text", std::string(vars.at("id").as_string())}}}}};
  });
server.completion([](const json::object&) -> Json {
  return {{"completion", {{"values", json::array{"cpp", "c++"}}, {"hasMore", false}}}};
});

// Consumers can transparently aggregate all pages:
auto all = client.list_all("tools/list", "tools");

// HTTP notifications: callback returns false to close the SSE connection.
Client::listen_http("http://127.0.0.1:3000/mcp", {{"toolsListChanged", true}},
  [](const Json& event) {
    std::cout << json::serialize(event) << "\n";
    return true;
  });
```

For stdio/HTTP subscriptions, event producers call `server.publish("notifications/tools/list_changed")` or `server.publish("notifications/resources/updated", {{"uri", "..."}})`. The SDK enforces the requested subscription filter and sends an acknowledgment first. Tool catalogs should be registered before starting the server; dynamic mutation of handler maps is not currently thread safe.

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

For protected HTTP, `server.protect_http(protected_resource_metadata, token_verifier)` installs the metadata endpoint and authorization callback. The verifier must validate issuer, audience, expiry and required scopes. The example server's `--http-auth` is only a local integration test using the `MYMCP_TEST_TOKEN` environment variable. `Client::http("https://host/mcp", token)` validates TLS certificates/hostnames and includes a Bearer token. `mymcp/oauth.hpp` provides PKCE S256 and `state`/`iss` helpers for a host-managed OAuth authorization code flow; it does **not** implement automatic OAuth discovery, token exchange or refresh.

All raw tool handlers are now checked against the supported JSON Schema assertion subset (see `include/mymcp/schema.hpp`). Schema violations return JSON-RPC `-32602`, not tool `isError`. Full JSON Schema 2020-12 validation and official conformance testing remain outstanding. See [COMPATIBILITY.md](COMPATIBILITY.md) and [SECURITY.md](SECURITY.md).

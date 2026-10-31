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

## Implemented now

- Modern `2026-07-28` JSON-RPC request envelope (`_meta.protocolVersion`, client identity/capabilities) and response metadata
- `server/discover`, including `supportedVersions`, capabilities, identity, and the new `resultType: complete`
- `tools/list`, `tools/call`, `resources/list`, `resources/read`, `resources/templates/list`, `prompts/list`, `prompts/get`
- stdio newline-delimited messages; local-only synchronous Streamable HTTP POST, JSON response; basic HTTP header validation and optional `x-mcp-header` mirroring
- Dual-era stdio support: modern `server/discover`, legacy `initialize`/`notifications/initialized`, and 2025 response shape
- Basic typed tool arguments (integer, number, boolean, string, optional), JSON Schema generation; std::string/primitive/JSON tool results

## Known gaps / constraints

- **Not full protocol conformance**: no subscription/listen streaming, OAuth authorization, progress handling, dynamic list-change notifications, detailed pagination/cache semantics, complete JSON Schema validation, or MCP extension implementations. Legacy HTTP sessions are not implemented.
- Client can perform modern-first stdio negotiation with legacy fallback and basic multi-round-trip elicitation via `on_input_required` callback. It does not yet cover the full capability matrix or server-side legacy MRTR shim.
- HTTP is synchronous `http://` only: no TLS, OAuth, SSE result streaming for subscriptions, or legacy HTTP sessions; use loopback or an authenticated TLS reverse proxy for local testing.
- Avoid exposing this prototype over public networks. Origin checking is intentionally restrictive, and HTTP handlers are synchronous.
- Calls are synchronous and single-threaded per client. For production use add concurrency/timeouts, full request validation, and comprehensive interoperability tests with the official MCP SDKs.

## Spec references

- [MCP 2026-07-28](https://modelcontextprotocol.io/specification/2026-07-28)
- [Discovery](https://modelcontextprotocol.io/specification/2026-07-28/server/discover)
- [Streamable HTTP](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http)
- [Tools](https://modelcontextprotocol.io/specification/2026-07-28/server/tools)

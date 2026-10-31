# MCP 2026-07-28 compatibility matrix

Updated October 31, 2026. **An actively developed subset implementation, not a fully conformant SDK.** A green CTest result only certifies the tested local behavior, not official protocol conformance.

| Protocol area | Current implementation | Remaining work |
|---|---|---|
| Core JSON-RPC | Request IDs, errors, typed tools, basic notifications | Strict wire-schema testing and malformed packet fuzzing |
| Stateless modern lifecycle | `server/discover`, required version + client identity/capabilities in request `_meta`, server response metadata, `resultType` | Official conformance tests and interoperability with TS v2/Kotlin/C# |
| Legacy 2025-11-25 | stdio `initialize` fallback | Legacy HTTP sessions / SSE, version-specific behavior parity |
| Tools | Typed registration, input JSON Schema assertion subset, context tools, async task tools, HTTP header annotations | Complete JSON Schema 2020-12 engine (e.g. remote refs, unevaluatedProperties); structured outputs/outputSchema validation; icons and annotations |
| Resources | Plain text resources, basic URI template variables, update notification | Binary resources and full RFC6570 URI templates |
| Prompts/completions | Rich prompt callback, arguments, completion callback | Schema validation and comprehensive field coverage |
| List pagination/cache | Deterministic in-memory pagination, `ttlMs`, `cacheScope`; authenticated HTTP forces private scope | Snapshot cursors and cache revalidation |
| MRTR | Basic `input_required` and retry with requestState | Full validation of sampling/elicitation, state security, all behaviors |
| Tasks extension | Opt-in capability gate, cryptographic task IDs, poll, input-required/update, cooperative cancel, TTL expiry, HTTP routing headers | Durable persistence across restart; `taskIds` subscriptions/`notifications/tasks`; external task backend; interoperable conformance |
| Request progress/cancellation | Concurrent stdio work, `notifications/progress` with monotonically increasing values, cooperative cancellation; sync client callback | HTTP response-scoped progress SSE and disconnect cancellation, configurable timeouts, robust stdio streaming API |
| Subscriptions | Explicit `subscriptions/listen`, acknowledge, filters, HTTP SSE, stdio cancellation | Task status subscriptions, reconnect/resume, graceful close API integration |
| HTTP transport | 2026 header validation, 4MiB request limit, loopback listener, metadata and bearer verification callback | Network-facing native HTTPS server, authenticated reverse proxy deployment guides, load/DoS tuning, legacy HTTP sessions |
| HTTPS client | TLS with peer/hostname verification (OpenSSL), configurable Bearer token | HTTPS SSE subscription path, timeouts/retries/cert pinning options |
| OAuth resource server | RFC9728-style metadata endpoint + bearer challenge; caller-supplied callback for token validation (MUST validate token/scope/audience/issuer) | Built-in JWT validation/JWKS discovery, OAuth scope step-up, authorization server interoperability |
| OAuth client | PKCE S256/nonce/state helpers, RFC9207 issuer callback checks | Automatic PRM/AS discovery, CIMD/pre-registration/DCR, token exchange/refresh/storage and scope accumulation |
| Extensions | Namespaced capability advertising + custom method registry; Tasks subset | Apps, non-core extensions, full negotiation and independent extension conformance |
| Official conformance | Nine local CTest groups (Linux/GCC) | Official conformance suite, TS v2/Kotlin integration, CI across OSes |

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Dependencies: C++20, Boost.Asio/Beast/Process/JSON headers and OpenSSL (SSL and Crypto).

## Security and scope notes

This repository must not be advertised as full MCP 2026-07-28 compatible. OAuth has **partial** implementation: a token verification callback is an integration hook, not a substitute for a trusted JWT/introspection verifier. The built-in HTTP listener binds only to loopback; a production resource server needs an authenticated TLS gateway and a security review. Long-running tasks are in-memory only, with finite retention and no cross-process durability.

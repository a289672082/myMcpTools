# MCP 2026-07-28 conformance and implementation matrix

Verified as of October 31, 2026. **No claim of full protocol conformance.**

| Protocol area | Status | Notes |
|---|---|---|
| Core JSON-RPC request/response | Basic | Some request IDs, notification behaviors and edge cases remain untested |
| Modern stateless metadata and discovery | Implemented | `server/discover`, protocol version, identity/capabilities, complete result |
| Legacy 2025-11-25 stdio handshake | Basic | Automatic client fallback; HTTP legacy unsupported |
| Tools | Basic | Synchronous typed/simple or raw handlers, listing/calling; no full schema validation |
| Resources | Partial | URI + simple template matching, text resources; no RFC6570 full expansion |
| Prompts | Basic | Text helper, raw rich messages, prompt argument descriptors |
| Completion | Basic | Callback, max 100 values, basic validation |
| Pagination | Implemented for in-memory lists | Stable map sort and per-method cursor; not snapshot-isolated |
| Cache hints | Implemented | `ttlMs` and `cacheScope` on six standard cacheable operations, including discovery |
| MRTR | Partial | `input_required`, `requestState`, callback retry and valid-operation guard; limited validation |
| HTTP Streamable | Partial | POST JSON and `subscriptions/listen` SSE; no native TLS/remote deployment, legacy sessions, response-scoped SSE from custom tools |
| stdio | Partial | Per-line requests, subscription ack/cancel; synchronous client cannot consume unsolicited events |
| Subscriptions | Partial | ACK, filters, event publication, HTTP SSE, stdio cancel; no graceful server-initiated close API |
| Notifications: progress/logging | Not implemented | Request-scoped notification transport remains absent |
| Cancellation | Partial | Stdio subscription cancellation; no general cancellable work |
| OAuth 2.1 | Not implemented | No CIMD/PKCE/issuer/metadata/scope/audience validation |
| Full JSON Schema 2020-12 | Not implemented | Typed parameter decoding only |
| TLS | Not implemented | HTTP restricted to loopback; use TLS/identity-enforcing reverse proxy |
| Extensions | API only | Advertise namespaced extension capability, custom method handlers; no Tasks/Apps/EMA implementation |
| Official SDK interop | Not verified | Need TypeScript v2/Kotlin/C# cross-tests and conformance suite |

## Security warning

DO NOT expose this SDK's built-in HTTP listener over a public network. It has no token authentication, authorization, native HTTPS, workload isolation, quotas or DoS protection. The listener intentionally refuses non-loopback bind addresses. Reverse proxies must enforce authentication and TLS before production deployment; that architecture still requires security review.

## Local tests

`cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure`

Tests: basic protocol, advanced resource/pagination/completion/MRTR/subscription behavior, HTTP headers and errors, stdio cancellation and SSE interoperability between this C++ client and server.

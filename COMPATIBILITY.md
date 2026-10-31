# MCP 2026-07-28 status (prototype)

This checklist is deliberately conservative. Passing local smoke tests is not a conformance certification.

| Area | State | Notes |
|---|---|---|
| JSON-RPC 2.0 basic framing | Implemented | Requests, errors, stdio framing |
| `2026-07-28` metadata | Implemented | Client version/capabilities/identity, server identity |
| `server/discover` | Implemented | Version list and capabilities |
| Tools | Basic | List and call, typed arguments, raw JSON result handlers |
| Resources | Basic | List, read, empty templates list |
| Prompts | Basic | List/get text prompts |
| Legacy stdio | Basic | `initialize` and initialized notification, process fallback |
| Streamable HTTP | Basic | POST JSON only, localhost, standard and custom headers, HTTP error codes |
| MRTR | Partial | `input_required` client callback and server raw handler; no full validation or legacy shim |
| `subscriptions/listen` | Not implemented | Long-lived SSE and acknowledgements required |
| List caching/pagination | Not implemented | No nextCursor, ttl or cacheScope functionality |
| OAuth | Not implemented | Not suitable for public deployments |
| HTTPS | Not implemented | TLS reverse proxy required for deployment |
| SSE streaming | Not implemented | HTTP JSON responses used |
| Cancellation/progress | Not implemented | |
| Extensions (Tasks, Apps) | Not implemented | Future separate modules |
| Full schema validation | Not implemented | Typed basic argument validation only |
| Interop conformance suite | Not run | Need official SDK bidirectional testing |

**Important:** The published spec `2026-07-28` is the target. Implementing a subset at the newest wire version does **not** mean full compliance with all optional and required features. Production use needs more work.

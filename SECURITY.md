# Security notice

This project is an experimental MCP 2026-07-28 SDK, **not production hardened**.

- Built-in HTTP is **loopback-only**. No authentication or HTTPS is provided; do not publicly expose it or forward it without a properly configured, authenticated TLS gateway.
- Tools execute your application code with the server process's privileges. Treat all tool input as untrusted. Validate business rules and authorize access independently of the SDK.
- `x-mcp-header` is checked for equality with declared JSON arguments. Do not use untrusted annotations for authentication/authorization decisions.
- Client-controlled URI inputs must not be mapped directly to unrestricted filesystem paths. URI template helpers are not a sandbox.
- Never trust tool annotations, extension metadata, subscription messages or MRTR input without validating their source and meaning.
- A full JSON Schema engine, modern OAuth resource server, timeouts and rate limiting are not implemented. Deploy with restrictive controls and test with official conformance tools first.

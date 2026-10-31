// Run this standalone stdio server without command line arguments.
// stdin/stdout are reserved for MCP JSON-RPC; logging goes to stderr only.
#include "mymcp/mcp.hpp"

int run_stdio_server_demo() {
  mymcp::Server server("stdio-demo");
  server.tool("multiply", "Multiply two integers", [](int lhs, int rhs) {
    return lhs * rhs;
  }, mymcp::arg<int>("lhs"), mymcp::arg<int>("rhs"));
  server.run_stdio();
  return 0;
}
int main() { return run_stdio_server_demo(); }

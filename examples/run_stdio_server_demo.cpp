// Run this standalone stdio server without command line arguments.
// stdin/stdout are reserved for MCP JSON-RPC; logging goes to stderr only.
#include "mymcp/mcp.hpp"
#include <chrono>
#include <thread>

int run_stdio_server_demo() {
  mymcp::Server server("stdio-demo");
  server.tool("multiply", "Multiply two integers", [](int lhs, int rhs) {
    return lhs * rhs;
  }, mymcp::arg<int>("lhs"), mymcp::arg<int>("rhs"));
  server.add_tool("count", "Report progress for an example long task",
    mymcp::json::object{{"type", "object"}},
    [&server](const mymcp::Json&) -> mymcp::Json {
      for (int n = 1; n <= 4; ++n) {
        if (server.is_cancelled()) return mymcp::detail::make_tool_result("cancelled");
        server.progress(n, 4, "Demo step " + std::to_string(n));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      return mymcp::detail::make_tool_result("completed");
    });
  server.run_stdio();
  return 0;
}
int main() { return run_stdio_server_demo(); }

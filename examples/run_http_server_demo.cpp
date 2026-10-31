// No arguments: start a loopback-only HTTP MCP server on the port in demo_common.hpp.
// This program runs until stopped; run_http_client_demo in a second terminal.
#include "demo_common.hpp"
#include <chrono>
#include <thread>

int run_http_server_demo() {
  return demo::execute("run_http_server_demo", [] {
    mymcp::Server server("http-demo");
    server.tool("hello", "Say hello over HTTP", [] { return "HTTP MCP is ready"; });
    server.resource(demo::status_uri, "Server status", [] { return std::string("online"); });
    server.add_tool("count_with_progress", "Stream progress updates using request-scoped SSE",
      mymcp::json::object{{"type", "object"}}, [&server](const mymcp::Json&) -> mymcp::Json {
        for (int i = 1; i <= 5; ++i) {
          if (server.is_cancelled()) break;
          server.progress(i, 5, "Processing batch");
          std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
        return mymcp::detail::make_tool_result("count complete");
      });
    server.add_tool("emit_update", "Send a notification to SSE subscribers",
      mymcp::json::object{{"type", "object"}}, [&server](const mymcp::Json&) -> mymcp::Json {
        server.publish("notifications/resources/updated", mymcp::json::object{{"uri", demo::status_uri}});
        return mymcp::detail::make_tool_result("event sent");
      });
    std::cout << "MCP HTTP server: " << demo::http_url << '\n' << std::flush;
    server.run_http(demo::http_port); // blocks until termination
  });
}
int main() { return run_http_server_demo(); }

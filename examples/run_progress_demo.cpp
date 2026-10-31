// No arguments: stdio progress notifications from a long-running tool call.
#include "demo_common.hpp"
#include <atomic>

#ifndef MYMCP_DEMO_STDIO_SERVER
#error "CMake must provide MYMCP_DEMO_STDIO_SERVER"
#endif

int run_progress_demo() {
  return demo::execute("run_progress_demo", [] {
    std::atomic<int> notifications{0};
    auto client = mymcp::Client::process(MYMCP_DEMO_STDIO_SERVER, {},
      [&](const mymcp::Json& message) {
        if (message.is_object() && message.contains("method") &&
            message.at("method") == "notifications/progress") {
          ++notifications;
          demo::print("progress event", message.at("params"));
        }
      });
    auto result = client.request_with_progress("tools/call",
      mymcp::json::object{{"name", "count"}, {"arguments", mymcp::json::object{}}},
      "progress-demo-token");
    demo::print("finished", result);
    demo::check(notifications > 0, "no progress notifications observed");
  });
}
int main() { return run_progress_demo(); }

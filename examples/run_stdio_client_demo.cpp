// A zero-argument process transport example. The server binary path is provided by CMake.
#include "demo_common.hpp"

#ifndef MYMCP_DEMO_STDIO_SERVER
#error "CMake must provide MYMCP_DEMO_STDIO_SERVER"
#endif
int run_stdio_client_demo() {
  return demo::execute("run_stdio_client_demo", [] {
    auto client = mymcp::Client::process(MYMCP_DEMO_STDIO_SERVER);
    demo::print("stdio discovery", client.discover());
    auto result = client.call("multiply", mymcp::json::object{{"lhs", 6}, {"rhs", 7}});
    demo::print("multiply", result);
  });
}
int main() { return run_stdio_client_demo(); }

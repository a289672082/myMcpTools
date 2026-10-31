// No command line parameters: custom method and namespaced extension capability.
#include "demo_common.hpp"

int run_extensions_demo() {
  return demo::execute("run_extensions_demo", [] {
    mymcp::Server server("extensions-demo");
    server.extension("example.com/demo", mymcp::json::object{{"revision", 1}});
    server.method("example/echo", [](const mymcp::Json& params) -> mymcp::Json {
      return mymcp::json::object{{"echo", params}};
    });
    auto client = demo::in_process(server);
    demo::print("server extensions", client.discover());
    demo::print("custom method", client.request("example/echo", mymcp::json::object{{"message", "hello"}}));
    // Extensions advertised here are examples, NOT an implementation of the full Apps extension.
  });
}
int main() { return run_extensions_demo(); }

// No command line parameters: run_tools_demo demonstrates typed tools, JSON Schema and pagination.
#include "demo_common.hpp"
#include <string>

int run_tools_demo() {
  return demo::execute("run_tools_demo", [] {
    mymcp::Server server("tools-demo");
    server.page_size(1); // each page has one tool
    server.tool("add", "Add two numbers", [](int a, int b) { return a + b; },
                mymcp::arg<int>("a"), mymcp::arg<int>("b"));
    server.tool("hello", "Say hello", [](std::string name) { return "Hello, " + name; },
                mymcp::arg<std::string>("name"));
    server.add_tool("percentage", "Only accept 0..100",
      mymcp::json::object{{"type", "object"},
        {"properties", mymcp::json::object{{"value", mymcp::json::object{
          {"type", "integer"}, {"minimum", 0}, {"maximum", 100}}}}},
        {"required", mymcp::json::array{"value"}}},
      [](const mymcp::Json& args) -> mymcp::Json {
        return mymcp::detail::make_tool_result(args.at("value").as_integer<int>());
      });

    auto client = demo::in_process(server);
    demo::print("discovery", client.discover());
    auto tools = client.list_all("tools/list", "tools");
    demo::check(tools.size() == 3, "pagination failed");
    demo::print("all tools", tools);
    auto answer = client.call("add", mymcp::json::object{{"a", 10}, {"b", 32}});
    demo::print("add(10,32)", answer);
    demo::print("hello", client.call("hello", mymcp::json::object{{"name", "C++"}}));
    demo::print("valid schema", client.call("percentage", mymcp::json::object{{"value", 42}}));
    try {
      (void)client.call("percentage", mymcp::json::object{{"value", 110}});
      throw std::runtime_error("invalid JSON Schema input was accepted");
    } catch (const mymcp::Error& e) {
      demo::check(e.code == -32602, "incorrect JSON-RPC error for schema violation");
      std::cout << "invalid percentage rejected as expected\n";
    }
  });
}
int main() { return run_tools_demo(); }

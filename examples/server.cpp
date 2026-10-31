#include "mymcp/mcp.hpp"
#include <string>
int main(int argc, char** argv) {
  using namespace mymcp;
  Server server("demo-server", "1.0.0");
  server.instructions("Tools for arithmetic and welcome messages.");
  server.tool("add", "Add two integers", [](int a, int b) { return a+b; },
              arg<int>("a", "First integer"), arg<int>("b", "Second integer"));
  server.tool("greet", "Say hello", [](std::string name) { return "Hello, " + name; },
              arg<std::string>("name", "Person's name"));
  server.resource("demo://status", "Status", [] { return std::string("healthy"); });
  server.prompt("welcome", "A welcome prompt", [](const mymcp::json::object&) { return "Welcome to the MCP server."; });
  if (argc >= 2 && std::string(argv[1]) == "--http")
    server.run_http(argc >= 3 ? static_cast<unsigned short>(std::stoi(argv[2])) : 3000);
  else server.run_stdio();
}

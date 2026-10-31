// No command line parameters: static resources and parameterized resource templates.
#include "demo_common.hpp"

int run_resources_demo() {
  return demo::execute("run_resources_demo", [] {
    mymcp::Server server("resources-demo");
    server.cache(30000, "public");
    server.resource("demo://readme", "Readme", [] { return std::string("C++ MCP resources"); });
    server.resource_template("demo://notes/{id}", "Example note",
      [](const std::string& uri, const mymcp::Json& vars) -> mymcp::Json {
        return mymcp::json::object{{"contents", mymcp::json::array{mymcp::json::object{
          {"uri", uri}, {"mimeType", "text/plain"},
          {"text", std::string("Note #") + vars.at("id").as_string()}}}}};
      });
    auto client = demo::in_process(server);
    demo::print("resource list", client.resources());
    demo::print("resource templates", client.resource_templates());
    demo::print("read fixed resource", client.read("demo://readme"));
    auto note = client.read("demo://notes/17");
    demo::check(note.contains("contents"), "resource template did not resolve");
    demo::print("read template", note);
  });
}
int main() { return run_resources_demo(); }

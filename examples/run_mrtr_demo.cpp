// No command line parameters: safe mock multi-round tool elicitation.
#include "demo_common.hpp"
#include <stdexcept>

int run_mrtr_demo() {
  return demo::execute("run_mrtr_demo", [] {
    mymcp::Server server("mrtr-demo");
    server.add_tool_context("confirm_demo", "Ask to confirm a non-destructive demo",
      mymcp::json::object{{"type", "object"}}, [](const mymcp::Json& params) -> mymcp::Json {
        if (!params.contains("inputResponses"))
          return mymcp::json::object{{"resultType", "input_required"},
            {"requestState", "demo-v1"}, {"inputRequests", mymcp::json::object{
              {"confirmation", mymcp::json::object{{"method", "elicitation/create"},
                {"params", mymcp::json::object{{"mode", "form"},
                  {"message", "Allow this harmless example to finish?"},
                  {"requestedSchema", mymcp::json::object{{"type", "object"},
                    {"properties", mymcp::json::object{{"approved", mymcp::json::object{{"type", "boolean"}}}}},
                    {"required", mymcp::json::array{"approved"}}}}}}}}}}};
        const auto& answers = params.at("inputResponses");
        bool approved = answers.at("confirmation").at("content").at("approved").as_bool();
        return mymcp::detail::make_tool_result(approved ? "confirmed" : "rejected");
      });
    auto client = demo::in_process(server);
    client.on_input_required([](const mymcp::Json& input) -> mymcp::Json {
      demo::print("user-facing request (demo auto-approves ONLY here)", input);
      return mymcp::json::object{{"confirmation", mymcp::json::object{
        {"action", "accept"}, {"content", mymcp::json::object{{"approved", true}}}}}};
    });
    demo::print("MRTR result", client.call("confirm_demo"));
    // Real products must ask their user instead of automatically consenting.
  });
}
int main() { return run_mrtr_demo(); }

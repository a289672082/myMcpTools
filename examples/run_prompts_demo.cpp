// No command line parameters: prompts with arguments and auto-completion.
#include "demo_common.hpp"
#include <string>

int run_prompts_demo() {
  return demo::execute("run_prompts_demo", [] {
    mymcp::Server server("prompts-demo");
    server.prompt_full("review", "Ask for a code review",
      [](const mymcp::Json& params) -> mymcp::Json {
        std::string subject = params.at("subject").as_string();
        return mymcp::json::object{{"messages", mymcp::json::array{mymcp::json::object{
          {"role", "user"}, {"content", mymcp::json::object{
            {"type", "text"}, {"text", "Review file " + subject}}}}}}};
      }, mymcp::json::array{mymcp::json::object{
           {"name", "subject"}, {"required", true}, {"description", "File to review"}}});
    server.completion([](const mymcp::Json&) -> mymcp::Json {
      return mymcp::json::object{{"completion", mymcp::json::object{
        {"values", mymcp::json::array{"main.cpp", "model.cpp"}}, {"hasMore", false}}}};
    });
    auto client = demo::in_process(server);
    demo::print("prompt catalog", client.prompts());
    demo::print("prompt", client.get_prompt("review", mymcp::json::object{{"subject", "main.cpp"}}));
    demo::print("completion", client.complete(mymcp::json::object{{"type", "ref/prompt"}, {"name", "review"}},
                                              "subject", "m"));
  });
}
int main() { return run_prompts_demo(); }

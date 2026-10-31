#include "mymcp/mcp.hpp"
#include "mymcp/schema.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#define CHECK(x) do { if(!(x)) throw std::runtime_error("assertion failed: " #x); } while (0)
using namespace mymcp;
int main() {
  Json nested = { {"type", "object"}, {"required", json::array{"name", "count"}},
    {"additionalProperties", false}, {"properties", {
      {"name", {{"type", "string"}, {"minLength", 2}, {"pattern", "^[A-Z]"}}},
      {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5}}}}}};
  std::string error;
  CHECK(validate_schema(nested, {{"name", "Alpha"}, {"count", 3}}, &error));
  CHECK(!validate_schema(nested, {{"name", "a"}, {"count", 3}}, &error));
  CHECK(error.find("/name") != std::string::npos);
  CHECK(!validate_schema(nested, {{"name", "Alpha"}, {"count", 7}}, &error));
  CHECK(!validate_schema(nested, {{"name", "Alpha"}, {"count", 3}, {"extra", 2}}, &error));
  Json local_ref = {{"$defs", {{"small", {{"type", "integer"}, {"maximum", 3}}}}},
                    {"$ref", "#/$defs/small"}};
  CHECK(validate_schema(local_ref, 2));
  CHECK(!validate_schema(local_ref, 10));
  Server srv("task-test");
  srv.enable_tasks();
  srv.add_tool("schema", "Validate", nested, [](const json::object&) {
    return detail::make_tool_result("ok");
  });
  srv.add_tool("background", "Run task", {{"type", "object"}}, [&](const json::object&) -> Json {
    return srv.start_task([](const Json& responses, std::atomic_bool& cancelled)->Json {
      if (cancelled.load()) return { {"content", json::array{}} };
      if (responses.is_null()) return {
        {"resultType", "input_required"},
        {"inputRequests", {{"confirm", {{"method", "elicitation/create"},
           {"params", {{"mode", "form"}, {"message", "Continue?"},
             {"requestedSchema", {{"type", "object"}}}}}}}}} };
      return detail::make_tool_result(responses.at("confirm"));
    }, 300000, 10);
  });
  Client cli([&](const Json& req) { return srv.handle(req); });
  cli.enable_tasks();
  CHECK(cli.call("schema", {{"name", "Alpha"}, {"count", 2}}).at("resultType") == "complete");
  try { cli.call("schema", {{"name", "A"}, {"count", 2}}); CHECK(false); }
  catch (const Error& e) {CHECK(e.code == -32602);}
  auto task = cli.call("background");
  CHECK(task.at("resultType") == "task");
  const std::string id(task.at("taskId").as_string());
  CHECK(id.size() == 48);
  Json state;
  for (int i=0;i<300;++i) {
    state = cli.get_task(id);
    if (state.at("status") == "input_required") break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(state.at("status") == "input_required");
  CHECK(state.at("inputRequests").as_object().contains("confirm"));
  CHECK(cli.update_task(id, {{"not-known", "ignored"}}).at("resultType") == "complete");
  CHECK(cli.get_task(id).at("status") == "input_required");
  CHECK(cli.update_task(id, {{"confirm", {{"action", "accept"}, {"content", {{"ok", true}}}}}}).at("resultType") == "complete");
  for (int i=0;i<300;++i) {
    state = cli.get_task(id);
    if (state.at("status") == "completed") break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(state.at("status") == "completed");
  CHECK(state.at("result").at("content").as_array()[0].at("text").is_string());
  CHECK(cli.cancel_task(id).at("resultType") == "complete");
  Client no_tasks([&](const Json& req){return srv.handle(req);});
  try { no_tasks.request("tasks/get", {{"taskId",id}}); CHECK(false); }
  catch (const Error& e) { CHECK(e.code == -32021); }
  try { cli.get_task("not-a-task"); CHECK(false); }
  catch (const Error& e) { CHECK(e.code == -32602); }
  try { cli.read("test://missing"); CHECK(false); }
  catch (const Error& e) { CHECK(e.code == -32602); }
  std::cout << "Schema and tasks tests passed\n";
}

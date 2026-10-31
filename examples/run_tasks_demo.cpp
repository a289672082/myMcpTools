// No command line parameters: asynchronous Tasks, client capability and polling.
#include "demo_common.hpp"
#include <atomic>
#include <chrono>
#include <thread>

int run_tasks_demo() {
  return demo::execute("run_tasks_demo", [] {
    mymcp::Server server("tasks-demo");
    server.enable_tasks();
    server.add_task_tool("create_report", "Build an in-memory sample report",
      mymcp::json::object{{"type", "object"}},
      [](const mymcp::Json&, std::atomic_bool& cancelled) -> mymcp::Json {
        for (int i=0; i<8; ++i) {
          if (cancelled.load()) return mymcp::detail::make_tool_result("cancelled");
          std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        return mymcp::detail::make_tool_result("demo report ready");
      });
    auto client = demo::in_process(server);
    client.enable_tasks(); // must advertise before discovering
    const auto result = client.call("create_report");
    demo::print("task created", result);
    const std::string id = result.at("taskId").as_string();
    bool completed = false;
    for (int attempt=0; attempt<100; ++attempt) {
      auto state = client.get_task(id);
      auto status = state.at("status").as_string();
      if (status == "completed") {
        demo::print("task completed", state);
        completed = true;
        break;
      }
      demo::check(status != "failed" && status != "cancelled", "Task unexpectedly failed");
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    demo::check(completed, "Task timed out");
    // See client.cancel_task(id) and client.update_task(id, responses) for other lifecycle operations.
  });
}
int main() { return run_tasks_demo(); }

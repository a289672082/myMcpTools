// Start run_http_server_demo first. No command-line arguments.
#include "demo_common.hpp"
#include <atomic>

int run_http_progress_demo() {
  return demo::execute("run_http_progress_demo", [] {
    std::atomic<int> events{0};
    double last = 0;
    auto client = mymcp::Client::http(demo::http_url, {}, [&](const mymcp::Json& event) {
      if (!event.is_object() || !event.contains("method") ||
          event.at("method") != "notifications/progress") return;
      const auto& params = event.at("params");
      demo::check(params.at("progressToken") == "http-demo-progress", "progress token mismatch");
      const double value = params.at("progress").as<double>();
      demo::check(value > last, "progress not increasing");
      last = value;
      ++events;
      demo::print("HTTP SSE progress", params);
    });
    auto result = client.request_with_progress("tools/call",
      mymcp::json::object{{"name", "count_with_progress"},
                           {"arguments", mymcp::json::object{}}}, "http-demo-progress");
    demo::check(result.at("resultType") == "complete", "missing final result");
    demo::check(events == 5, "not all progress SSE events arrived");
    demo::print("HTTP SSE final result", result);
  });
}
int main() { return run_http_progress_demo(); }

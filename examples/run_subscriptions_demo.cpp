// No arguments: SSE subscription with a publish request from another client.
// Start run_http_server_demo first; this program connects to its fixed loopback URL.
#include "demo_common.hpp"
#include <atomic>
#include <thread>

int run_subscriptions_demo() {
  return demo::execute("run_subscriptions_demo", [] {
    std::thread producer;
    std::atomic_bool updated{false};
    std::atomic_bool acknowledged{false};
    try {
      mymcp::Client::listen_http(demo::http_url,
          mymcp::json::object{{"resourceSubscriptions", mymcp::json::array{demo::status_uri}}},
          [&](const mymcp::Json& message) -> bool {
            const std::string method = message.at("method").as_string();
            if (method == "notifications/subscriptions/acknowledged" && !acknowledged.exchange(true)) {
              producer = std::thread([] {
                try {
                  auto client = mymcp::Client::http(demo::http_url);
                  client.call("emit_update");
                } catch (const std::exception& e) { std::cerr << "publish failed: " << e.what() << '\n'; }
              });
            }
            if (method == "notifications/resources/updated") {
              demo::print("resource updated", message);
              updated = true;
              return false; // close SSE stream cleanly
            }
            return true;
          });
    } catch (...) {
      if (producer.joinable()) producer.join();
      throw;
    }
    if (producer.joinable()) producer.join();
    demo::check(acknowledged && updated, "no subscription acknowledgement/update received");
  });
}
int main() { return run_subscriptions_demo(); }

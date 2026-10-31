// No arguments: talk to the development-only protected resource demo server.
// The hard-coded token is intentionally NOT a valid OAuth token. NEVER use in production.
#include "demo_common.hpp"

int run_oauth_resource_client_demo() {
  return demo::execute("run_oauth_resource_client_demo", [] {
    constexpr const char* local_url = "http://127.0.0.1:33868/mcp";
    constexpr const char* local_test_token = "local-demo-token-not-a-real-oauth-token";
    auto authorized = mymcp::Client::http(local_url, local_test_token);
    demo::print("authenticated discovery", authorized.discover());
    demo::print("protected tool", authorized.call("private_hello"));
  });
}
int main() { return run_oauth_resource_client_demo(); }

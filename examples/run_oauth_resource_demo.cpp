// Fixed development-only token. No command line parameters, binds loopback only.
// Never copy this verifier into production: a string comparison is NOT token validation.
#include "demo_common.hpp"
#include <openssl/crypto.h>

int run_oauth_resource_demo() {
  return demo::execute("run_oauth_resource_demo", [] {
    constexpr unsigned short port = 33868;
    const std::string token = "local-demo-token-not-a-real-oauth-token";
    mymcp::Server server("oauth-resource-demo");
    server.tool("private_hello", "Only available with the demo Bearer token",
                [] { return "protected demo result"; });
    server.protect_http(mymcp::json::object{
        {"resource", "http://127.0.0.1:33868/mcp"},
        {"authorization_servers", mymcp::json::array{"https://auth.example.invalid"}}},
      [token](const std::string& candidate, const std::string&, const std::string&) {
        return candidate.size() == token.size() &&
          CRYPTO_memcmp(candidate.data(), token.data(), candidate.size()) == 0;
      });
    std::cout << "LOCAL TEST ONLY: http://127.0.0.1:33868/mcp\n"
              << "Fixed token is intentionally untrusted demo data.\n" << std::flush;
    server.run_http(port);
  });
}
int main() { return run_oauth_resource_demo(); }

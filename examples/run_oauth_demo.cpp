// No arguments and no credentials: local mock OAuth 2.1 authorization code + PKCE flow.
// The localhost issuer is for educational use only, NOT a production identity provider.
#include "demo_common.hpp"
#include "mymcp/oauth_client.hpp"
#include <httplib.h>
#include <thread>

int run_oauth_demo() {
  return demo::execute("run_oauth_demo", [] {
    httplib::Server idp;
    const int port = idp.bind_to_any_port("127.0.0.1");
    demo::check(port > 0, "could not bind mock authorization server");
    const std::string issuer = "http://127.0.0.1:" + std::to_string(port);
    idp.Get("/.well-known/oauth-protected-resource", [issuer](const httplib::Request&, httplib::Response& res) {
      res.set_content("{\"resource\":\"" + issuer + "/mcp\",\"authorization_servers\":[\"" + issuer + "\"]}", "application/json");
    });
    idp.Get("/.well-known/oauth-authorization-server", [issuer](const httplib::Request&, httplib::Response& res) {
      res.set_content("{\"issuer\":\"" + issuer + "\",\"authorization_endpoint\":\"" + issuer +
         "/authorize\",\"token_endpoint\":\"" + issuer + "/token\",\"registration_endpoint\":\"" +
         issuer + "/register\",\"authorization_response_iss_parameter_supported\":true}", "application/json");
    });
    idp.Post("/register", [](const httplib::Request& req, httplib::Response& res) {
      if (req.body.find("\"application_type\":\"native\"") == std::string::npos) {
        res.status = 400;
        return;
      }
      res.status = 201;
      res.set_content("{\"client_id\":\"demo-native-client\"}", "application/json");
    });
    idp.Post("/token", [](const httplib::Request& req, httplib::Response& res) {
      if (req.body.find("grant_type=authorization_code") == std::string::npos ||
          req.body.find("code_verifier=") == std::string::npos) {
        res.status = 400;
        return;
      }
      res.set_content("{\"access_token\":\"mock-test-token\",\"token_type\":\"Bearer\","
                      "\"refresh_token\":\"mock-refresh\",\"expires_in\":3600}", "application/json");
    });
    std::thread host([&] { idp.listen_after_bind(); });
    try {
      mymcp::oauth::Session oauth(issuer + "/mcp");
      oauth.discover();
      demo::check(oauth.metadata().issuer == issuer, "issuer mismatch");
      const std::string redirect = "http://127.0.0.1:8765/callback";
      oauth.register_dynamic({redirect}, "C++ sample native app");
      auto flow = oauth.begin_authorization(redirect, {"tools:read"});
      demo::check(flow.url.find("code_challenge_method=S256") != std::string::npos, "PKCE S256 not present");
      std::cout << "PKCE authorization URL produced (not printed, to avoid leaking state)\n";
      // Pretend the user approved in the local mock issuer. No actual browser or secret needed.
      const std::string callback = redirect + "?code=mock-auth-code&state=" +
        mymcp::oauth::url_encode(flow.state) + "&iss=" + mymcp::oauth::url_encode(issuer);
      oauth.exchange_callback_url(flow, callback, redirect);
      demo::check(oauth.has_token(), "OAuth token exchange failed");
      std::cout << "OAuth discovery, registration, PKCE, callback validation and code exchange succeeded.\n";
    } catch (...) { idp.stop(); host.join(); throw; }
    idp.stop(); host.join();
  });
}
int main() { return run_oauth_demo(); }

// No arguments: connect to run_http_server_demo on localhost.
#include "demo_common.hpp"

int run_http_client_demo() {
  return demo::execute("run_http_client_demo", [] {
    auto client = mymcp::Client::http(demo::http_url);
    demo::print("HTTP discovery", client.discover());
    demo::print("tools", client.tools());
    demo::print("call", client.call("hello"));
    demo::print("resource", client.read(demo::status_uri));
  });
}
int main() { return run_http_client_demo(); }

// 真实本地 HTTP MCP 服务端：验证接收前字节限额及整个请求的截止时间。
#include "mymcp/mcp.hpp"
#include "httplib.h"
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
using namespace std::chrono_literals;
int main(){
  httplib::Server server;
  server.Post("/oversized",[](const httplib::Request&,httplib::Response& response){
    response.set_content(std::string(4096,'x'),"application/json");
  });
  server.Post("/slow-stream",[](const httplib::Request&,httplib::Response& response){
    response.set_chunked_content_provider("text/event-stream",
      [](std::size_t,httplib::DataSink& sink) {
        std::this_thread::sleep_for(25ms);
        static constexpr char frame[]=": keepalive\n\n";
        return sink.write(frame,sizeof(frame)-1);
      });
  });
  const int port=server.bind_to_any_port("127.0.0.1");
  assert(port>0);
  std::thread runner([&]{server.listen_after_bind();});
  for(int i=0;i<200 && !server.is_running();++i)std::this_thread::sleep_for(5ms);
  assert(server.is_running());
  const auto origin="http://127.0.0.1:"+std::to_string(port);
  mymcp::RequestOptions limited;
  limited.timeout=2s;
  limited.max_response_bytes=256;
  auto oversized=mymcp::Client::http(origin+"/oversized");
  bool failed=false;
  try{(void)oversized.request("server/discover",mymcp::json::object{},limited);}
  catch(const std::runtime_error& e) {
    failed=true;
    assert(std::string(e.what()).find("too large")!=std::string::npos);
  }
  assert(failed);

  mymcp::RequestOptions timed;
  timed.timeout=200ms;
  timed.max_response_bytes=1024*1024;
  auto slow=mymcp::Client::http(origin+"/slow-stream");
  const auto start=std::chrono::steady_clock::now();
  failed=false;
  try{(void)slow.request("server/discover",mymcp::json::object{},timed);}
  catch(const std::runtime_error&){failed=true;}
  assert(failed);
  assert(std::chrono::steady_clock::now()-start < 4s);
  server.stop();runner.join();
  std::cout<<"HTTP MCP frame limits and total deadline OK\n";
}

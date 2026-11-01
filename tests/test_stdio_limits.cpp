// Offline stress tests: stalled stdio servers and oversized JSON replies.
#include "mymcp/mcp.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

int main(int argc,char** argv){
  using namespace std::chrono_literals;
  if(argc>1){
    const std::string mode=argv[1];
    if(mode=="stall" || mode=="no-read"){
      std::this_thread::sleep_for(20s);
      return 0;
    }
    if(mode=="oversize"){
      std::string request;
      std::getline(std::cin,request);
      std::cout<<"{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"data\":\""
               <<std::string(8192,'x')<<"\"}}"<<std::endl;
      return 0;
    }
    return 2;
  }
  const std::string self=argv[0];
  auto verify=[&](const std::string& mode, std::size_t max_bytes,
                  std::chrono::milliseconds timeout, std::string expected){
    const auto begin=std::chrono::steady_clock::now();
    auto client=mymcp::Client::process(self,{mode});
    client.set_transport_limits({timeout,max_bytes});
    bool rejected=false;
    try{
      (void)client.request("server/discover",mymcp::json::object{});
    }catch(const std::exception& e){
      rejected=true;
      const std::string message=e.what();
      assert(message.find(expected)!=std::string::npos);
    }
    assert(rejected);
    assert(std::chrono::steady_clock::now()-begin<10s);
  };
  verify("stall",1024,800ms,"deadline");
  verify("oversize",512,3000ms,"limit");
  std::cout<<"stdio limits tests passed\n";
}

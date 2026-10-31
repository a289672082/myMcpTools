#pragma once
// Shared helpers for the no-argument examples, not part of the public SDK.
#include "mymcp/mcp.hpp"
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace demo {
inline constexpr unsigned short http_port = 33867;
inline constexpr const char* http_url = "http://127.0.0.1:33867/mcp";
inline constexpr const char* status_uri = "demo://current-status";

inline mymcp::Client in_process(mymcp::Server& server) {
  return mymcp::Client([&server](const mymcp::Json& request) { return server.handle(request); });
}
inline void check(bool ok, std::string_view what) {
  if (!ok) throw std::runtime_error(std::string(what));
}
inline void print(std::string_view label, const mymcp::Json& result) {
  std::cout << label << ": " << mymcp::json::serialize(result) << '\n';
}
template<class Function>
int execute(std::string_view name, Function&& body) {
  try {
    body();
    std::cout << name << " OK\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << name << " failed: " << error.what() << '\n';
    return 1;
  }
}
} // namespace demo

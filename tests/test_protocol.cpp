#include "mymcp/mcp.hpp"
#include <stdexcept>
#define REQUIRE(x) do { if (!(x)) throw std::runtime_error("Assertion failed: " #x); } while(false)
#include <iostream>
using namespace mymcp;
int main() {
  Server server("test", "1.0");
  server.tool("add", "Add integers", [](int a, int b){return a+b;}, arg<int>("a"), arg<int>("b"));
  server.resource("test://one", "One", []{return std::string("value");});
  Client client([&](const Json& req){return server.handle(req);});
  auto discovery = client.discover();
  REQUIRE(discovery.at("resultType") == "complete");
  REQUIRE(discovery.at("supportedVersions").as_array()[0] == ProtocolVersion);
  REQUIRE(discovery.at("_meta").as_object().contains("io.modelcontextprotocol/serverInfo"));
  REQUIRE(client.tools().at("tools").as_array().size() == 1);
  auto call = client.call("add", {{"a", 2}, {"b", 3}});
  REQUIRE(call.at("resultType") == "complete");
  REQUIRE(call.at("content").as_array()[0].at("text") == "5");
  REQUIRE(client.read("test://one").at("contents").as_array()[0].at("text") == "value");
  auto bad = client.call("add", {{"a", "bad"}, {"b", 3}});
  REQUIRE(bad.at("isError") == true);
  Json invalid = {{"jsonrpc", "2.0"}, {"id", 88}, {"method", "tools/list"},
                  {"params", {{"_meta", {{"io.modelcontextprotocol/protocolVersion", "2099-01-01"},
                                          {"io.modelcontextprotocol/clientCapabilities", json::object{}}}}}}};
  REQUIRE(server.handle(invalid).at("error").at("code") == -32022);
  // Legacy stdio era: initialize and notifications/initialized, without modern _meta.
  Server legacy_server("legacy-test", "1.0");
  legacy_server.tool("echo", "Echo", [](std::string s){ return s; }, arg<std::string>("text"));
  Client fallback([&](const Json& req){
    if (req.at("method") == "server/discover")
      return Json{{"jsonrpc", "2.0"}, {"id", req.at("id")},
                  {"error", {{"code", -32601}, {"message", "Method not found"}}}};
    return legacy_server.handle(req);
  });
  REQUIRE(fallback.discover().at("protocolVersion") == LegacyProtocolVersion);
  REQUIRE(fallback.call("echo", {{"text", "legacy"}}).at("content").as_array()[0].at("text") == "legacy");
  // HTTP modern discovery must not advertise legacy versions.
  Json modern_req = {{"jsonrpc", "2.0"}, {"id", 19}, {"method", "server/discover"},
                     {"params", {{"_meta", {{"io.modelcontextprotocol/protocolVersion", ProtocolVersion},
                                            {"io.modelcontextprotocol/clientCapabilities", json::object{}}}}}}};
  REQUIRE(server.handle(modern_req, false).at("result").at("supportedVersions").as_array().size() == 1);
  // MRTR: callback supplies an elicitation answer; client retries with requestState.
  Server mrtr("mrtr-server");
  mrtr.add_tool_context("ask", "Ask the user", {{"type", "object"}},
      [](const json::object& params) -> Json {
        if (const auto* responses = params.if_contains("inputResponses")) {
          if (!responses->is_object() || !params.contains("requestState") ||
              params.at("requestState") != "opaque") throw std::runtime_error("Lost requestState");
          return {{"content", json::array{{{"type", "text"}, {"text", "accepted"}}}}, {"isError", false}};
        }
        Json req = {{"method", "elicitation/create"},
                    {"params", {{"mode", "form"}, {"message", "Continue?"},
                    {"requestedSchema", {{"type", "object"}, {"properties", json::object{}}}}}}};
        return {{"resultType", "input_required"},
                {"inputRequests", {{"answer", std::move(req)}}},
                {"requestState", "opaque"}};
      });
  Client mrtr_client([&](const Json& req){ return mrtr.handle(req); });
  mrtr_client.on_input_required([](const Json& requests) -> Json {
    REQUIRE(requests.at("answer").at("method") == "elicitation/create");
    return {{"answer", {{"action", "accept"}, {"content", json::object{}}}}};
  });
  REQUIRE(mrtr_client.call("ask").at("content").as_array()[0].at("text") == "accepted");
  std::cout << "Protocol tests passed\n";
}

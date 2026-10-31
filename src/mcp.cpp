#include "mymcp/mcp.hpp"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/process.hpp>
#include <algorithm>
#include <cctype>
#include <iostream>
#include <mutex>
#include <sstream>

namespace mymcp {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace bp = boost::process;
using tcp = asio::ip::tcp;

namespace {
const Json null_json = nullptr;
const Json& find(const Json& object, boost::json::string_view key) {
  if (!object.is_object()) return null_json;
  if (const auto* p = object.as_object().if_contains(key)) return *p;
  return null_json;
}
std::string string_of(const Json& obj, boost::json::string_view key, std::string fallback = {}) {
  const auto& v = find(obj, key);
  return v.is_string() ? std::string(v.as_string()) : fallback;
}
Json rpc_error(Json id, int code, std::string message, Json data = nullptr) {
  json::object e{{"code", code}, {"message", std::move(message)}};
  if (!data.is_null()) e["data"] = std::move(data);
  return {{"jsonrpc", "2.0"}, {"id", std::move(id)}, {"error", std::move(e)}};
}
Json rpc_success(Json id, Json result) {
  return {{"jsonrpc", "2.0"}, {"id", std::move(id)}, {"result", std::move(result)}};
}
Json complete(Json value, bool modern) {
  if (modern && value.is_object() && !value.as_object().contains("resultType"))
    value.as_object()["resultType"] = "complete";
  return value;
}
std::string ascii_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
  return s;
}
std::string encode_b64(std::string_view s) {
  constexpr auto tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  for (std::size_t i = 0; i < s.size(); i += 3) {
    const unsigned a = static_cast<unsigned char>(s[i]);
    const unsigned b = i + 1 < s.size() ? static_cast<unsigned char>(s[i + 1]) : 0;
    const unsigned c = i + 2 < s.size() ? static_cast<unsigned char>(s[i + 2]) : 0;
    result += tbl[a >> 2]; result += tbl[((a & 3) << 4) | (b >> 4)];
    result += i + 1 < s.size() ? tbl[((b & 15) << 2) | (c >> 6)] : '=';
    result += i + 2 < s.size() ? tbl[c & 63] : '=';
  }
  return result;
}
std::optional<std::string> decode_b64(std::string_view input) {
  if (input.size() % 4) return std::nullopt;
  std::string r;
  for (std::size_t i = 0; i < input.size(); i += 4) {
    unsigned vals[4]{};
    for (int k = 0; k < 4; ++k) {
      char c = input[i + k];
      if (c >= 'A' && c <= 'Z') vals[k] = c - 'A';
      else if (c >= 'a' && c <= 'z') vals[k] = c - 'a' + 26;
      else if (c >= '0' && c <= '9') vals[k] = c - '0' + 52;
      else if (c == '+') vals[k] = 62;
      else if (c == '/') vals[k] = 63;
      else if (c == '=' && i + 4 == input.size() && k >= 2) vals[k] = 0;
      else return std::nullopt;
    }
    r += char((vals[0] << 2) | (vals[1] >> 4));
    if (input[i+2] != '=') r += char((vals[1] << 4) | (vals[2] >> 2));
    if (input[i+3] != '=') r += char((vals[2] << 6) | vals[3]);
  }
  return r;
}
std::string encode_header(std::string s) {
  bool safe = !s.empty() && s.front() != ' ' && s.back() != ' ' && s.front() != '\t' && s.back() != '\t';
  for (unsigned char c : s) if (c < 0x20 || c > 0x7e) safe = false;
  if (s.starts_with("=?base64?") && s.ends_with("?=")) safe = false;
  return safe ? s : "=?base64?" + encode_b64(s) + "?=";
}
std::optional<std::string> decode_header(std::string s) {
  if (s.starts_with("=?base64?")) {
    if (!s.ends_with("?=")) return std::nullopt;
    return decode_b64(std::string_view(s).substr(9, s.size()-11));
  }
  for (unsigned char c : s) if (c < 0x20 || c > 0x7e) return std::nullopt;
  return s;
}
struct HeaderParam { std::string header; std::vector<std::string> path; std::string type; };
void extract_header_fields(const Json& schema, std::vector<std::string> path,
                           std::vector<HeaderParam>& out) {
  if (!schema.is_object()) return;
  const auto& ann = find(schema, "x-mcp-header");
  if (ann.is_string()) {
    std::string name(ann.as_string());
    std::string type = string_of(schema, "type");
    if (name.empty() || !std::all_of(name.begin(), name.end(), [](unsigned char c){
      return std::isalnum(c) || std::string("!#$%&'*+-.^_`|~").find(char(c)) != std::string::npos;
    }) || (type != "integer" && type != "string" && type != "boolean"))
      throw Error(-32602, "Invalid x-mcp-header annotation");
    out.push_back({"Mcp-Param-" + name, path, type});
  }
  const auto& properties = find(schema, "properties");
  if (properties.is_object()) for (const auto& [name, child] : properties.as_object()) {
    auto p = path;
    p.emplace_back(name);
    extract_header_fields(child, std::move(p), out);
  }
}
std::vector<HeaderParam> header_fields(const Json& schema) {
  std::vector<HeaderParam> out;
  extract_header_fields(schema, {}, out);
  std::map<std::string, bool> seen;
  for (const auto& p : out) if (!seen.emplace(ascii_lower(p.header), true).second)
    throw Error(-32602, "Duplicate x-mcp-header annotation");
  return out;
}
const Json& path_value(const Json& root, const std::vector<std::string>& path) {
  const Json* p = &root;
  for (const auto& part : path) p = &find(*p, part);
  return *p;
}
std::string header_value(const Json& v) {
  if (v.is_string()) return std::string(v.as_string());
  if (v.is_bool()) return v.as_bool() ? "true" : "false";
  if (v.is_int64()) return std::to_string(v.as_int64());
  if (v.is_uint64()) return std::to_string(v.as_uint64());
  throw Error(-32602, "Invalid HTTP mirrored parameter value");
}
} // anonymous namespace

namespace detail {
Json make_tool_result(const Json& v) {
  return {{"content", json::array{{{"type", "text"}, {"text", json::serialize(v)}}}},
          {"structuredContent", v}, {"isError", false}};
}
Json make_tool_result(const std::string& s) {
  return {{"content", json::array{{{"type", "text"}, {"text", s}}}}, {"isError", false}};
}
Json make_tool_result(const char* s) { return make_tool_result(std::string(s)); }
Json make_tool_result(bool v) { return make_tool_result(v ? "true" : "false"); }
Json make_tool_result(int v) { return make_tool_result(std::to_string(v)); }
Json make_tool_result(long v) { return make_tool_result(std::to_string(v)); }
Json make_tool_result(long long v) { return make_tool_result(std::to_string(v)); }
Json make_tool_result(double v) { return make_tool_result(std::to_string(v)); }
} // namespace detail

Server::Server(std::string name, std::string version) : name_(std::move(name)), version_(std::move(version)) {}
void Server::add_tool(std::string name, std::string description, Json schema, ToolHandler handler) {
  if (!schema.is_object()) throw std::invalid_argument("Tool input schema must be an object");
  if (!handler) throw std::invalid_argument("Tool handler cannot be empty");
  if (!tools_.emplace(name, Tool{std::move(description), std::move(schema), std::move(handler), false}).second)
    throw std::invalid_argument("Duplicate tool: " + name);
}
void Server::add_tool_context(std::string name, std::string description, Json schema, ToolHandler handler) {
  if (!schema.is_object() || !handler) throw std::invalid_argument("Invalid tool schema or handler");
  if (!tools_.emplace(name, Tool{std::move(description), std::move(schema), std::move(handler), true}).second)
    throw std::invalid_argument("Duplicate tool: " + name);
}
void Server::resource(std::string uri, std::string name, std::function<std::string()> reader, std::string mime) {
  if (!resources_.emplace(uri, Resource{std::move(name), std::move(mime), std::move(reader)}).second)
    throw std::invalid_argument("Duplicate resource: " + uri);
}
void Server::prompt(std::string name, std::string description, std::function<std::string(const json::object&)> reader) {
  if (!prompts_.emplace(name, Prompt{std::move(description), std::move(reader)}).second)
    throw std::invalid_argument("Duplicate prompt: " + name);
}
const Json* Server::tool_schema(const std::string& name) const {
  const auto it = tools_.find(name);
  return it == tools_.end() ? nullptr : &it->second.schema;
}

Json Server::handle(const Json& req, bool allow_legacy) {
  if (!req.is_object() || string_of(req, "jsonrpc") != "2.0" || !find(req, "method").is_string())
    return rpc_error(nullptr, -32600, "Invalid JSON-RPC request");
  const Json id = find(req, "id");
  const bool notification = id.is_null();
  const std::string method = string_of(req, "method");
  const Json& params = find(req, "params");
  const Json& meta = find(params, "_meta");
  const bool modern = meta.is_object() && !find(meta, "io.modelcontextprotocol/protocolVersion").is_null();
  if (notification) return nullptr; // modern stdio client notifications are limited to cancel; no reply
  Json result;
  try {
    if (method == "initialize" && !modern && allow_legacy) {
      legacy_initialized_ = true;
      auto offered = string_of(params, "protocolVersion", LegacyProtocolVersion);
      if (offered != "2025-11-25" && offered != "2025-06-18" && offered != "2025-03-26")
        offered = LegacyProtocolVersion;
      Json caps = {{"tools", json::object{}}, {"resources", json::object{}}, {"prompts", json::object{}}};
      return rpc_success(id, {{"protocolVersion", offered}, {"capabilities", caps},
                              {"serverInfo", {{"name", name_}, {"version", version_}}}});
    }
    if (!modern && (!legacy_initialized_ || !allow_legacy)) throw Error(-32600, "Modern requests require per-request _meta");
    if (modern) {
      if (!find(meta, "io.modelcontextprotocol/clientCapabilities").is_object())
        throw Error(-32602, "Missing clientCapabilities in _meta");
      auto version = string_of(meta, "io.modelcontextprotocol/protocolVersion");
      if (version != ProtocolVersion) {
        return rpc_error(id, -32022, "Unsupported protocol version",
                         {{"requested", version}, {"supported", json::array{ProtocolVersion, LegacyProtocolVersion}}});
      }
    }
    if (method == "server/discover") {
      if (!modern) throw Error(-32601, "Method not found");
      result = {{"supportedVersions", allow_legacy ? json::array{ProtocolVersion, LegacyProtocolVersion} : json::array{ProtocolVersion}},
                {"capabilities", {{"tools", json::object{}}, {"resources", json::object{}}, {"prompts", json::object{}}}}};
      if (!instructions_.empty()) result.as_object()["instructions"] = instructions_;
    } else if (method == "tools/list") {
      json::array list;
      for (const auto& [name, tool] : tools_) list.push_back({{"name", name}, {"description", tool.description}, {"inputSchema", tool.schema}});
      result = {{"tools", std::move(list)}};
    } else if (method == "tools/call") {
      const auto name = string_of(params, "name");
      const auto it = tools_.find(name);
      if (it == tools_.end()) throw Error(-32602, "Unknown tool: " + name);
      const Json& args = find(params, "arguments");
      if (!args.is_null() && !args.is_object()) throw Error(-32602, "Tool arguments must be an object");
      try {
        result = it->second.handler(it->second.with_context ? params.as_object() :
                                     (args.is_object() ? args.as_object() : json::object{}));
      } catch (const std::exception& e) {
        result = {{"content", json::array{{{"type", "text"}, {"text", e.what()}}}}, {"isError", true}};
      }
      if (!result.is_object()) throw Error(-32603, "Invalid tool result");
    } else if (method == "resources/list") {
      json::array list;
      for (const auto& [uri, res] : resources_) list.push_back({{"uri", uri}, {"name", res.name}, {"mimeType", res.mime}});
      result = {{"resources", std::move(list)}};
    } else if (method == "resources/read") {
      const auto uri = string_of(params, "uri");
      const auto it = resources_.find(uri);
      if (it == resources_.end()) throw Error(-32002, "Resource not found: " + uri);
      result = {{"contents", json::array{{{"uri", uri}, {"mimeType", it->second.mime}, {"text", it->second.reader()}}}}};
    } else if (method == "resources/templates/list") {
      result = {{"resourceTemplates", json::array{}}};
    } else if (method == "prompts/list") {
      json::array list;
      for (const auto& [name, prompt] : prompts_) list.push_back({{"name", name}, {"description", prompt.description}});
      result = {{"prompts", std::move(list)}};
    } else if (method == "prompts/get") {
      const auto name = string_of(params, "name");
      const auto it = prompts_.find(name);
      if (it == prompts_.end()) throw Error(-32602, "Unknown prompt: " + name);
      const Json& args = find(params, "arguments");
      result = {{"messages", json::array{{{"role", "user"}, {"content", {{"type", "text"}, {"text", it->second.reader(args.is_object() ? args.as_object() : json::object{})}}}}}}};
    } else if (method == "ping" && !modern) {
      result = json::object{};
    } else {
      throw Error(-32601, "Method not found: " + method);
    }
    result = complete(std::move(result), modern);
    if (modern) result.as_object()["_meta"] = {{"io.modelcontextprotocol/serverInfo", {{"name", name_}, {"version", version_}}}};
    return rpc_success(id, std::move(result));
  } catch (const Error& e) { return rpc_error(id, e.code, e.what()); }
  catch (const std::exception& e) { return rpc_error(id, -32603, e.what()); }
}

void Server::run_stdio() {
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    try {
      Json reply = handle(json::parse(line));
      if (!reply.is_null()) std::cout << json::serialize(reply) << '\n' << std::flush;
    } catch (const std::exception&) {
      std::cout << json::serialize(rpc_error(nullptr, -32700, "Parse error")) << '\n' << std::flush;
    }
  }
}

namespace {
using Request = http::request<http::string_body>;
using Response = http::response<http::string_body>;
std::string header(const Request& request, const std::string& name) {
  const auto it = request.find(name);
  return it == request.end() ? "" : std::string(it->value());
}
Response http_response(http::status status, const Json& payload, unsigned version = 11) {
  Response response{status, version};
  response.set(http::field::content_type, "application/json");
  response.set(http::field::server, "myMcpTools");
  response.keep_alive(false);
  response.body() = json::serialize(payload);
  response.prepare_payload();
  return response;
}
Response on_http(Server& server, const Request& req, const std::string& bind_host, unsigned short port) {
  Json id = nullptr;
  if (req.method() != http::verb::post || req.target() != "/mcp")
    return http_response(http::status::not_found, rpc_error(id, -32601, "MCP endpoint is POST /mcp"), req.version());
  const auto origin = header(req, "Origin");
  if (!origin.empty() && origin != "http://" + bind_host + ":" + std::to_string(port) &&
       !(bind_host == "127.0.0.1" && origin == "http://localhost:" + std::to_string(port)))
    return http_response(http::status::forbidden, rpc_error(id, -32000, "Invalid Origin"), req.version());
  const std::string accept = header(req, "Accept");
  if (accept.find("application/json") == std::string::npos || accept.find("text/event-stream") == std::string::npos ||
      header(req, "Content-Type").find("application/json") == std::string::npos)
    return http_response(http::status::bad_request, rpc_error(id, -32600, "Invalid HTTP Content-Type or Accept"), req.version());
  Json body;
  try { body = json::parse(req.body()); }
  catch (const std::exception&) { return http_response(http::status::bad_request, rpc_error(nullptr, -32700, "Parse error"), req.version()); }
  id = find(body, "id");
  const Json& params = find(body, "params");
  const Json& meta = find(params, "_meta");
  std::string method = string_of(body, "method");
  const std::string ver = string_of(meta, "io.modelcontextprotocol/protocolVersion");
  if (ver.empty() || header(req, "MCP-Protocol-Version") != ver || header(req, "Mcp-Method") != method)
    return http_response(http::status::bad_request, rpc_error(id, -32020, "HeaderMismatch: version or method"), req.version());
  if (ver != ProtocolVersion)
    return http_response(http::status::bad_request,
      rpc_error(id, -32022, "Unsupported protocol version", {{"requested", ver}, {"supported", json::array{ProtocolVersion}}}), req.version());
  if (method == "tools/call" || method == "resources/read" || method == "prompts/get") {
    const std::string expected = string_of(params, method == "resources/read" ? "uri" : "name");
    const auto decoded = decode_header(header(req, "Mcp-Name"));
    if (!decoded || *decoded != expected)
      return http_response(http::status::bad_request, rpc_error(id, -32020, "HeaderMismatch: Mcp-Name"), req.version());
  }
  if (method == "tools/call") {
    const auto schema = server.tool_schema(string_of(params, "name"));
    if (schema) {
      try {
        const auto fields = header_fields(*schema);
        const Json& args = find(params, "arguments");
        for (const auto& field : fields) {
          const Json& val = path_value(args, field.path);
          const auto actual = header(req, field.header);
          if (val.is_null()) {
            if (!actual.empty()) throw Error(-32020, "HeaderMismatch: unexpected " + field.header);
          } else {
            const auto decoded = decode_header(actual);
            if (!decoded || *decoded != header_value(val))
              throw Error(-32020, "HeaderMismatch: " + field.header);
          }
        }
      } catch (const Error& e) {
        return http_response(http::status::bad_request, rpc_error(id, -32020, e.what()), req.version());
      }
    }
  }
  Json response = server.handle(body, false);
  if (response.is_null()) {
    Response accepted{http::status::accepted, req.version()};
    accepted.keep_alive(false); accepted.prepare_payload(); return accepted;
  }
  if (find(response, "error").is_object()) {
    const auto error_code = find(find(response, "error"), "code");
    if (error_code.is_int64() && error_code.as_int64() == -32601)
      return http_response(http::status::not_found, response, req.version());
  }
  return http_response(http::status::ok, response, req.version());
}
} // anonymous namespace

void Server::run_http(unsigned short port, std::string host) {
  asio::io_context io;
  tcp::acceptor listener(io, tcp::endpoint(asio::ip::make_address(host), port));
  for (;;) {
    tcp::socket socket(io);
    listener.accept(socket);
    try {
      beast::flat_buffer buffer;
      Request request;
      http::request_parser<http::string_body> parser;
      parser.body_limit(4 * 1024 * 1024);
      http::read(socket, buffer, parser);
      request = parser.release();
      Response response = on_http(*this, request, host, port);
      http::write(socket, response);
    } catch (const std::exception& e) {
      std::cerr << "MCP HTTP error: " << e.what() << '\n';
    }
    beast::error_code ec;
    socket.shutdown(tcp::socket::shutdown_both, ec);
  }
}

namespace {
struct ProcessState {
  bp::ipstream output;
  bp::opstream input;
  bp::child child;
  std::mutex mutex;
  ProcessState(const std::string& executable, const std::vector<std::string>& args)
      : child(executable, bp::args(args), bp::std_out > output, bp::std_in < input) {}
  ~ProcessState() {
    input.pipe().close();
    if (child.running()) child.terminate();
    child.wait();
  }
  Json send(const Json& message) {
    std::lock_guard<std::mutex> lock(mutex);
    input << json::serialize(message) << '\n' << std::flush;
    if (find(message, "id").is_null()) return nullptr;
    std::string line;
    while (std::getline(output, line)) {
      try {
        Json response = json::parse(line);
        if (find(message, "id").is_null()) return nullptr;
        if (find(response, "id") == find(message, "id")) return response;
        // Ignore progress/subscription notifications for synchronous client calls.
      } catch (const std::exception&) { throw std::runtime_error("Invalid JSON-RPC output from MCP server"); }
    }
    throw std::runtime_error("MCP server closed stdout");
  }
};
struct ParsedUrl {std::string host, port, target;};
ParsedUrl parse_url(std::string url) {
  if (!url.starts_with("http://")) throw std::invalid_argument("Only http:// supported; use TLS proxy for https://");
  url.erase(0, 7);
  auto p = url.find('/');
  std::string authority = url.substr(0, p);
  std::string target = p == std::string::npos ? "/mcp" : url.substr(p);
  p = authority.rfind(':');
  if (p == std::string::npos) return {authority, "80", target};
  return {authority.substr(0,p), authority.substr(p+1), target};
}
struct HttpState {
  ParsedUrl url;
  std::map<std::string, Json> tool_schemas;
  std::mutex mutex;
  explicit HttpState(const std::string& path) : url(parse_url(path)) {}
  Json send(const Json& msg) {
    std::lock_guard<std::mutex> lock(mutex);
    const std::string method = string_of(msg, "method");
    const Json& params = find(msg, "params");
    asio::io_context io;
    tcp::resolver resolver(io);
    beast::tcp_stream stream(io);
    stream.connect(resolver.resolve(url.host, url.port));
    Request req{http::verb::post, url.target, 11};
    req.set(http::field::host, url.host + ":" + url.port);
    req.set(http::field::user_agent, "myMcpTools/0.1");
    req.set(http::field::accept, "application/json, text/event-stream");
    req.set(http::field::content_type, "application/json");
    req.set("MCP-Protocol-Version", ProtocolVersion);
    req.set("Mcp-Method", method);
    if (method == "tools/call" || method == "prompts/get" || method == "resources/read")
      req.set("Mcp-Name", encode_header(string_of(params, method == "resources/read" ? "uri" : "name")));
    if (method == "tools/call") {
      const auto it = tool_schemas.find(string_of(params, "name"));
      if (it != tool_schemas.end()) {
        const Json& values = find(params, "arguments");
        for (const auto& field : header_fields(it->second)) {
          const Json& value = path_value(values, field.path);
          if (!value.is_null()) req.set(field.header, encode_header(header_value(value)));
        }
      }
    }
    req.body() = json::serialize(msg);
    req.prepare_payload();
    http::write(stream, req);
    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);
    beast::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);
    if (res.result() == http::status::accepted) return nullptr;
    Json response;
    if (std::string(res[http::field::content_type]).starts_with("text/event-stream")) {
      std::istringstream ss(res.body());
      std::string line;
      while (std::getline(ss, line)) {
        if (!line.starts_with("data:")) continue;
        std::string data = line.substr(5);
        if (!data.empty() && data[0] == ' ') data.erase(0,1);
        auto parsed = json::parse(data);
        if (!find(parsed, "id").is_null()) response = parsed;
      }
      if (response.is_null()) throw std::runtime_error("No JSON-RPC response in SSE stream");
    } else response = json::parse(res.body());
    if (method == "tools/list") {
      const Json& tools = find(find(response, "result"), "tools");
      if (tools.is_array()) {
        for (const auto& tool : tools.as_array()) {
          const auto name = string_of(tool, "name");
          const auto schema = find(tool, "inputSchema");
          try { header_fields(schema); tool_schemas[name] = schema; }
          catch (const Error&) { tool_schemas.erase(name); }
        }
      }
    }
    return response;
  }
};
} // anonymous namespace

Client::Client(std::function<Json(const Json&)> send, std::string name)
    : send_(std::move(send)), name_(std::move(name)) {}
Client Client::process(const std::string& executable, std::vector<std::string> args) {
  auto state = std::make_shared<ProcessState>(executable, args);
  return Client([state](const Json& message){ return state->send(message); });
}
Client Client::http(const std::string& url) {
  auto state = std::make_shared<HttpState>(url);
  Client client([state](const Json& message){ return state->send(message); });
  client.allow_legacy_ = false; // HTTP legacy needs session transport, intentionally unsupported here.
  client.http_ = true;
  return client;
}
Json Client::request(const std::string& method, Json params) {
  if (!params.is_object()) throw std::invalid_argument("Request params must be an object");
  if (!connected_ && method != "server/discover" && method != "initialize") discover();
  for (int round = 0; round < 9; ++round) {
    Json actual_params = params;
    if (!legacy_) actual_params.as_object()["_meta"] = {
      {"io.modelcontextprotocol/protocolVersion", ProtocolVersion},
      {"io.modelcontextprotocol/clientCapabilities", capabilities_},
      {"io.modelcontextprotocol/clientInfo", {{"name", name_}, {"version", "1.0.0"}}}};
    Json message = {{"jsonrpc", "2.0"}, {"id", next_id_++}, {"method", method}, {"params", std::move(actual_params)}};
    Json response = send_(message);
    if (!response.is_object()) throw std::runtime_error("Invalid JSON-RPC response");
    if (find(response, "id") != find(message, "id")) throw std::runtime_error("JSON-RPC response ID mismatch");
    const Json& e = find(response, "error");
    if (e.is_object()) throw Error(static_cast<int>(find(e, "code").as_int64()), string_of(e, "message", "MCP error"));
    const Json& result = find(response, "result");
    if (!result.is_object()) throw std::runtime_error("Invalid MCP result");
    if (method == "tools/list") tools_known_ = true;
    if (string_of(result, "resultType") != "input_required" || !input_handler_) return result;
    if (round == 8) throw Error(-32603, "Input-required rounds exceeded");
    const Json& input_requests = find(result, "inputRequests");
    if (!input_requests.is_object()) throw Error(-32603, "Invalid inputRequests");
    Json answers = input_handler_(input_requests);
    if (!answers.is_object()) throw Error(-32602, "Input callback must return an object");
    params.as_object()["inputResponses"] = std::move(answers);
    const Json& state = find(result, "requestState");
    if (state.is_string()) params.as_object()["requestState"] = state;
    else params.as_object().erase("requestState");
  }
  throw Error(-32603, "Input-required rounds exceeded");
}
Json Client::discover() {
  if (connected_) return discovery_;
  try {
    Json info = request("server/discover");
    const auto& versions = find(info, "supportedVersions");
    if (!versions.is_array()) throw Error(-32600, "Malformed discovery response");
    for (const auto& version : versions.as_array()) {
      if (version.is_string() && version.as_string() == ProtocolVersion) {
        connected_ = true;
        discovery_ = info;
        return info;
      }
    }
    if (!allow_legacy_) throw Error(-32022, "Server does not support 2026-07-28");
  } catch (const Error& e) {
    if (!allow_legacy_ || (e.code != -32601 && e.code != -32600 && e.code != -32022)) throw;
  }
  // stdio fallback for pre-2026 servers. Never negotiate modern versions in initialize.
  legacy_ = true;
  Json init = request("initialize", {{"protocolVersion", LegacyProtocolVersion},
                                     {"clientInfo", {{"name", name_}, {"version", "1.0.0"}}},
                                     {"capabilities", json::object{}}});
  connected_ = true;
  discovery_ = init;
  // The legacy notifications/initialized message has no response. Send it directly.
  Json notification = {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
  send_(notification);
  return init;
}
Json Client::tools() { return request("tools/list"); }
Json Client::call(const std::string& name, Json arguments) {
  if (http_ && !tools_known_) tools(); // discover x-mcp-header annotations before calling
  return request("tools/call", {{"name", name}, {"arguments", std::move(arguments)}});
}
Json Client::resources() { return request("resources/list"); }
Json Client::read(const std::string& uri) { return request("resources/read", {{"uri", uri}}); }
Json Client::prompts() { return request("prompts/list"); }
Json Client::get_prompt(const std::string& name, Json arguments) {
  return request("prompts/get", {{"name", name}, {"arguments", std::move(arguments)}});
}
} // namespace mymcp

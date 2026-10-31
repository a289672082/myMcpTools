#include "mymcp/mcp.hpp"
#include "mymcp/schema.hpp"
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <ctime>
#include <cmath>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/process.hpp>
#include <algorithm>
#include <cctype>
#include <iostream>
#include <mutex>
#include <sstream>
#include <limits>
#include <thread>
#include <chrono>
#include <atomic>
#include <regex>
#include <cstdint>

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
  if (v.is_int64()) {
    if (v.as_int64() < -9007199254740991LL || v.as_int64() > 9007199254740991LL)
      throw Error(-32602, "HTTP mirrored integers must be IEEE754 safe integers");
    return std::to_string(v.as_int64());
  }
  if (v.is_uint64()) {
    if (v.as_uint64() > 9007199254740991ULL)
      throw Error(-32602, "HTTP mirrored integers must be IEEE754 safe integers");
    return std::to_string(v.as_uint64());
  }
  throw Error(-32602, "Invalid HTTP mirrored parameter value");
}
void paginate_list(json::array& values, const Json& params, Json& result,
                   const std::string& method, std::size_t page_size) {
  const std::size_t total = values.size();
  const Json& cursor = find(params, "cursor");
  std::size_t offset = 0;
  if (!cursor.is_null()) {
    if (!cursor.is_string()) throw Error(-32602, "Cursor must be a string");
    const std::string token(cursor.as_string());
    const std::string prefix = method + ":";
    if (!token.starts_with(prefix)) throw Error(-32602, "Invalid cursor");
    const auto digits = token.substr(prefix.length());
    if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos)
      throw Error(-32602, "Invalid cursor");
    try {
      const auto number = std::stoull(digits);
      if (number > std::numeric_limits<std::size_t>::max()) throw std::out_of_range("cursor");
      offset = static_cast<std::size_t>(number);
    } catch (...) { throw Error(-32602, "Invalid cursor"); }
    if (offset >= total) throw Error(-32602, "Expired or invalid cursor");
  }
  json::array page;
  const std::size_t end = offset + std::min(page_size, total - offset);
  for (auto i = offset; i < end; ++i) page.push_back(values[i]);
  values = std::move(page);
  if (end < total) result.as_object()["nextCursor"] = method + ":" + std::to_string(end);
}
// Simple RFC6570 level-1 URI templates: {variable}. No unsafe partial match.
std::optional<json::object> template_match(std::string_view pattern, std::string_view uri) {
  std::string rx = "^";
  std::vector<std::string> names;
  for (size_t i = 0; i < pattern.size(); ++i) {
    if (pattern[i] == '{') {
      const auto end = pattern.find('}', i + 1);
      if (end == std::string_view::npos || end == i+1) return std::nullopt;
      std::string n(pattern.substr(i+1, end-i-1));
      if (!std::all_of(n.begin(), n.end(), [](unsigned char c){return std::isalnum(c)||c=='_';})) return std::nullopt;
      names.push_back(std::move(n)); rx += "([^?#]*)"; i = end;
    } else {
      unsigned char c = pattern[i];
      if (std::string(R"(\.[]{}()+*?^$|)").find(static_cast<char>(c)) != std::string::npos) rx += '\\';
      rx += static_cast<char>(c);
    }
  }
  rx += '$';
  try {
    std::smatch m;
    std::string input(uri);
    if (!std::regex_match(input, m, std::regex(rx))) return std::nullopt;
    json::object arguments;
    for (size_t j = 0; j < names.size(); ++j) arguments[names[j]] = m[j+1].str();
    return arguments;
  } catch (const std::regex_error&) { return std::nullopt; }
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
void Server::add_task_tool(std::string name, std::string description, Json schema, TaskHandler handler) {
  if (!handler) throw std::invalid_argument("Invalid task handler");
  const std::string key = name;
  add_tool(std::move(name), std::move(description), std::move(schema),
    [this, job = std::move(handler)](const json::object&) -> Json {
      return start_task(job);
    });
  tools_.at(key).requires_tasks = true;
}
void Server::resource(std::string uri, std::string name, std::function<std::string()> reader, std::string mime) {
  if (!resources_.emplace(uri, Resource{std::move(name), std::move(mime), std::move(reader)}).second)
    throw std::invalid_argument("Duplicate resource: " + uri);
}
void Server::prompt(std::string name, std::string description, std::function<std::string(const json::object&)> reader) {
  if (!prompts_.emplace(name, Prompt{std::move(description), std::move(reader), {}, json::array{}}).second)
    throw std::invalid_argument("Duplicate prompt: " + name);
}
void Server::resource_template(std::string uri_template, std::string name,
                               std::function<Json(const std::string&, const json::object&)> reader,
                               std::string mime, std::string description) {
  if (!reader || uri_template.empty()) throw std::invalid_argument("Invalid resource template");
  if (!templates_.emplace(uri_template, ResourceTemplate{std::move(name), std::move(mime),
           std::move(description), std::move(reader)}).second)
    throw std::invalid_argument("Duplicate resource template");
}
void Server::prompt_full(std::string name, std::string description,
                         std::function<Json(const json::object&)> handler, Json arguments) {
  if (!handler || !arguments.is_array()) throw std::invalid_argument("Invalid prompt");
  if (!prompts_.emplace(name, Prompt{std::move(description), {}, std::move(handler),
                                     std::move(arguments)}).second)
    throw std::invalid_argument("Duplicate prompt");
}
void Server::completion(std::function<Json(const json::object&)> handler) {
  if (!handler) throw std::invalid_argument("Invalid completion handler");
  completion_ = std::move(handler);
}
void Server::method(std::string name, std::function<Json(const json::object&)> handler) {
  if (!handler || name.empty() || !methods_.emplace(name, std::move(handler)).second)
    throw std::invalid_argument("Duplicate or invalid method");
}
void Server::extension(std::string id, Json settings) {
  if (id.find('/') == std::string::npos || !settings.is_object())
    throw std::invalid_argument("Extension identifier must be namespaced and settings must be an object");
  extensions_[id] = std::move(settings);
}
void Server::protect_http(Json metadata, TokenVerifier verifier) {
  if (!verifier || !metadata.is_object() || !find(metadata, "resource").is_string() ||
      !find(metadata, "authorization_servers").is_array())
    throw std::invalid_argument("Valid protected resource metadata and token verifier required");
  token_verifier_ = std::move(verifier);
  protected_resource_metadata_ = std::move(metadata);
  cache_scope_ = "private"; // authenticated responses must not be shared between callers
}
bool Server::verify_http(const std::string& token, const std::string& method,
                         const std::string& name) const {
  try { return token_verifier_ && !token.empty() && token_verifier_(token, method, name); }
  catch (...) { return false; }
}
void Server::cache(std::uint64_t ttl_ms, std::string scope) {
  if (scope != "public" && scope != "private") throw std::invalid_argument("Invalid cache scope");
  ttl_ms_ = ttl_ms;
  cache_scope_ = std::move(scope);
}
void Server::page_size(std::size_t value) {
  if (!value || value > 100000) throw std::invalid_argument("Invalid page size");
  page_size_ = value;
}
std::shared_ptr<Server::Subscription> Server::subscribe(Json id, Json filter) {
  if (!(id.is_string() || id.is_int64() || id.is_uint64()) || !filter.is_object())
    throw Error(-32602, "Invalid subscription ID/filter");
  json::object accepted;
  for (const auto& [name, value] : filter.as_object()) {
    const std::string key(name);
    if (key == "resourceSubscriptions") {
      if (!value.is_array()) throw Error(-32602, "resourceSubscriptions must be an array");
      json::array resources;
      for (const auto& uri : value.as_array()) {
        if (!uri.is_string()) throw Error(-32602, "Resource subscription URI must be string");
        if (resources.size() > 10000) throw Error(-32602, "Too many subscriptions");
        resources.push_back(uri);
      }
      accepted[key] = std::move(resources);
    } else if (key == "toolsListChanged" || key == "promptsListChanged" || key == "resourcesListChanged") {
      if (!value.is_bool()) throw Error(-32602, "Subscription flags must be boolean");
      if (value.as_bool()) accepted[key] = true;
    } else throw Error(-32602, "Unknown subscription filter: " + key);
  }
  auto sub = std::make_shared<Subscription>();
  sub->id = std::move(id);
  sub->notifications = std::move(accepted);
  // Acknowledgement is always queued before any other notification.
  sub->messages.push_back({{"jsonrpc", "2.0"}, {"method", "notifications/subscriptions/acknowledged"},
    {"params", {{"_meta", {{"io.modelcontextprotocol/subscriptionId", sub->id}}},
                {"notifications", sub->notifications}}}});
  {
    std::lock_guard lock(subscriber_mutex_);
    subscribers_.erase(std::remove_if(subscribers_.begin(), subscribers_.end(),
       [](const auto& entry){ return entry.expired(); }), subscribers_.end());
    subscribers_.push_back(sub);
  }
  return sub;
}
void Server::publish(const std::string& method, Json params) {
  std::string filter;
  if (method == "notifications/tools/list_changed") filter = "toolsListChanged";
  else if (method == "notifications/prompts/list_changed") filter = "promptsListChanged";
  else if (method == "notifications/resources/list_changed") filter = "resourcesListChanged";
  else if (method == "notifications/resources/updated") filter = "resourceSubscriptions";
  else throw std::invalid_argument("Unsupported subscription notification");
  if (!params.is_object()) throw std::invalid_argument("Notification parameters must be an object");
  const std::string uri = string_of(params, "uri");
  if (filter == "resourceSubscriptions" && uri.empty())
    throw std::invalid_argument("Resource update notification requires URI");
  std::lock_guard lock(subscriber_mutex_);
  for (auto it = subscribers_.begin(); it != subscribers_.end();) {
    auto sub = it->lock();
    if (!sub) { it = subscribers_.erase(it); continue; }
    ++it;
    const Json& allowed = find(sub->notifications, filter);
    bool selected = allowed.is_bool() && allowed.as_bool();
    if (allowed.is_array()) for (const auto& entry : allowed.as_array())
      if (entry.is_string() && entry.as_string() == uri) selected = true;
    if (!selected) continue;
    Json payload = params;
    payload.as_object()["_meta"] = {{"io.modelcontextprotocol/subscriptionId", sub->id}};
    Json event = {{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(payload)}};
    {
      std::lock_guard stream_lock(sub->mutex);
      if (!sub->closed && sub->messages.size() < 4096) sub->messages.push_back(std::move(event));
    }
    sub->ready.notify_one();
  }
}
void Server::enable_tasks() {
  extension("io.modelcontextprotocol/tasks");
}

namespace {
std::string secure_task_id() {
  unsigned char b[24];
  if (RAND_bytes(b, sizeof(b)) != 1) throw Error(-32603, "Cryptographic RNG failure");
  constexpr char hex[] = "0123456789abcdef";
  std::string id; id.reserve(sizeof(b) * 2);
  for (unsigned char byte : b) {id += hex[byte >> 4]; id += hex[byte & 15];}
  return id;
}
std::string utc_now() {
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm output{};
#ifdef _WIN32
  gmtime_s(&output, &t);
#else
  gmtime_r(&t, &output);
#endif
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &output);
  return buffer;
}
}

void Server::run_task(const std::shared_ptr<TaskState>& task, Json input_responses) {
  std::thread([task, input_responses = std::move(input_responses)]() mutable {
    Json answer;
    try {
      answer = task->handler(input_responses, task->cancel_requested);
      std::lock_guard lock(task->mutex);
      if (task->details.at("status") == "cancelled") return;
      task->details.as_object()["lastUpdatedAt"] = utc_now();
      if (!task->cancel_requested.load() && answer.is_object() &&
          answer.as_object().if_contains("resultType") && answer.at("resultType") == "input_required") {
        const Json& items = answer.at("inputRequests");
        if (!items.is_object() || items.as_object().empty()) throw Error(-32603, "Invalid task input requests");
        for (const auto& [key, item] : items.as_object()) {
          if (task->used_input_ids.count(std::string(key))) throw Error(-32603, "Task input ID reused");
          if (!item.is_object()) throw Error(-32603, "Invalid input request");
          task->used_input_ids.insert(std::string(key));
        }
        task->details.as_object()["status"] = "input_required";
        task->details.as_object()["inputRequests"] = items;
      } else if (task->cancel_requested.load()) {
        task->details.as_object()["status"] = "cancelled";
      } else if (answer.is_object()) {
        task->details.as_object()["status"] = "completed";
        task->details.as_object()["result"] = std::move(answer);
      } else throw Error(-32603, "Task handler must return a JSON object");
    } catch (const Error& error) {
      std::lock_guard lock(task->mutex);
      task->details.as_object()["status"] = "failed";
      task->details.as_object()["error"] = {{"code", error.code}, {"message", error.what()}};
      task->details.as_object()["lastUpdatedAt"] = utc_now();
    } catch (const std::exception& error) {
      std::lock_guard lock(task->mutex);
      task->details.as_object()["status"] = "failed";
      task->details.as_object()["error"] = {{"code", -32603}, {"message", error.what()}};
      task->details.as_object()["lastUpdatedAt"] = utc_now();
    }
  }).detach();
}

Json Server::start_task(TaskHandler handler, std::uint64_t ttl_ms, std::uint64_t poll_ms) {
  if (!extensions_.contains("io.modelcontextprotocol/tasks"))
    throw Error(-32603, "Call enable_tasks() before returning task handles");
  if (!handler || !poll_ms || !ttl_ms || poll_ms > 3600000 || ttl_ms > 604800000)
    throw Error(-32602, "Invalid task handler or task timing");
  auto task = std::make_shared<TaskState>();
  const auto id = secure_task_id();
  const auto timestamp = utc_now();
  task->handler = std::move(handler);
  task->expires_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(ttl_ms);
  task->details = {{"taskId", id}, {"status", "working"},
     {"createdAt", timestamp}, {"lastUpdatedAt", timestamp},
     {"ttlMs", ttl_ms}, {"pollIntervalMs", poll_ms}};
  {
    std::lock_guard lock(task_mutex_);
    // Expired tasks may be purged, including from a long-lived server.
    const auto now = std::chrono::steady_clock::now();
    for (auto it = tasks_.begin(); it != tasks_.end();) {
      if (now >= it->second->expires_at) it = tasks_.erase(it);
      else ++it;
    }
    if (tasks_.size() >= 10000) throw Error(-32603, "Task capacity exceeded");
    tasks_.emplace(id, task);
  }
  Json result = task->details;
  run_task(task, nullptr);
  result.as_object()["resultType"] = "task";
  return result;
}

Json Server::task_state(const std::string& id) {
  std::shared_ptr<TaskState> task;
  {
    std::lock_guard lock(task_mutex_);
    auto it = tasks_.find(id);
    if (it == tasks_.end()) throw Error(-32602, "Unknown taskId");
    if (std::chrono::steady_clock::now() >= it->second->expires_at) {
      tasks_.erase(it);
      throw Error(-32602, "Task expired");
    }
    task = it->second;
  }
  std::lock_guard lock(task->mutex);
  Json status = task->details;
  status.as_object()["resultType"] = "complete";
  return status;
}

void Server::close_subscription(const std::shared_ptr<Subscription>& subscription) {
  if (!subscription) return;
  {
    std::lock_guard lock(subscription->mutex);
    subscription->closed = true;
  }
  subscription->ready.notify_all();
}

const Json* Server::tool_schema(const std::string& name) const {
  const auto it = tools_.find(name);
  return it == tools_.end() ? nullptr : &it->second.schema;
}

Json Server::handle(const Json& req, bool allow_legacy) {
  if (!req.is_object() || string_of(req, "jsonrpc") != "2.0" || !find(req, "method").is_string())
    return rpc_error(nullptr, -32600, "Invalid JSON-RPC request");
  const Json id = find(req, "id");
  const bool notification = !req.as_object().contains("id");
  if (!notification && !(id.is_string() || id.is_int64() || id.is_uint64()))
    return rpc_error(nullptr, -32600, "Invalid JSON-RPC ID");
  const std::string method = string_of(req, "method");
  const Json& params = find(req, "params");
  const Json& meta = find(params, "_meta");
  const bool modern = meta.is_object() && !find(meta, "io.modelcontextprotocol/protocolVersion").is_null();
  if (notification) {
    // Modern core notifications are restricted to stdio cancellation.
    if (method == "notifications/cancelled" || method == "notifications/initialized") return nullptr;
    return nullptr;
  }
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
      if (!params.is_object()) throw Error(-32602, "Request parameters must be an object");
      if (!find(meta, "io.modelcontextprotocol/clientCapabilities").is_object())
        throw Error(-32602, "Missing clientCapabilities in _meta");
      const Json& info = find(meta, "io.modelcontextprotocol/clientInfo");
      if (!info.is_object() || !find(info, "name").is_string() ||
          !find(info, "version").is_string())
        throw Error(-32602, "Missing or invalid clientInfo in _meta");
      auto version = string_of(meta, "io.modelcontextprotocol/protocolVersion");
      if (version != ProtocolVersion) {
        return rpc_error(id, -32022, "Unsupported protocol version",
                         {{"requested", version}, {"supported", json::array{ProtocolVersion, LegacyProtocolVersion}}});
      }
    }
    if (method == "server/discover") {
      if (!modern) throw Error(-32601, "Method not found");
      json::object capabilities{{"tools", json::object{}}, {"resources", json::object{}},
                                {"prompts", json::object{}}};
      if (completion_) capabilities["completions"] = json::object{};
      if (!extensions_.empty()) capabilities["extensions"] = extensions_;
      result = {{"supportedVersions", allow_legacy ? json::array{ProtocolVersion, LegacyProtocolVersion} : json::array{ProtocolVersion}},
                {"capabilities", std::move(capabilities)}};
      if (!instructions_.empty()) result.as_object()["instructions"] = instructions_;
    } else if (method == "tools/list") {
      json::array list;
      for (const auto& [name, tool] : tools_) list.push_back({{"name", name}, {"description", tool.description}, {"inputSchema", tool.schema}});
      result = {{"tools", std::move(list)}};
      paginate_list(result.as_object().at("tools").as_array(), params, result, method, page_size_);
    } else if (method == "tools/call") {
      const auto name = string_of(params, "name");
      const auto it = tools_.find(name);
      if (it == tools_.end()) throw Error(-32602, "Unknown tool: " + name);
      if (it->second.requires_tasks &&
          !find(find(find(meta, "io.modelcontextprotocol/clientCapabilities"), "extensions"),
                "io.modelcontextprotocol/tasks").is_object())
        throw Error(-32021, "Task tool requires the io.modelcontextprotocol/tasks client extension");
      const Json& args = find(params, "arguments");
      if (!args.is_null() && !args.is_object()) throw Error(-32602, "Tool arguments must be an object");
      // Validate before entering the tool, including raw/context handlers.
      const Json actual_args = args.is_object() ? args : Json(json::object{});
      std::string schema_error;
      if (!validate_schema(it->second.schema, actual_args, &schema_error))
        throw Error(-32602, "Tool inputSchema violation: " + schema_error);
      try {
        result = it->second.handler(it->second.with_context ? params.as_object() :
                                     (args.is_object() ? args.as_object() : json::object{}));
      } catch (const std::exception& e) {
        result = {{"content", json::array{{{"type", "text"}, {"text", e.what()}}}}, {"isError", true}};
      }
      if (!result.is_object()) throw Error(-32603, "Invalid tool result");
    } else if (method == "tasks/get" || method == "tasks/update" || method == "tasks/cancel") {
      if (!modern || !extensions_.contains("io.modelcontextprotocol/tasks") ||
          !find(find(meta, "io.modelcontextprotocol/clientCapabilities"), "extensions").is_object() ||
          !find(find(find(meta, "io.modelcontextprotocol/clientCapabilities"), "extensions"),
                "io.modelcontextprotocol/tasks").is_object())
        throw Error(-32021, "Missing Required Client Capability: io.modelcontextprotocol/tasks");
      const std::string task_id = string_of(params, "taskId");
      if (task_id.empty()) throw Error(-32602, "Missing taskId");
      if (method == "tasks/get") result = task_state(task_id);
      else {
        std::shared_ptr<TaskState> task;
        {
          std::lock_guard lock(task_mutex_);
          auto it = tasks_.find(task_id);
          if (it == tasks_.end()) throw Error(-32602, "Unknown taskId");
          if (std::chrono::steady_clock::now() >= it->second->expires_at) {
            tasks_.erase(it);
            throw Error(-32602, "Task expired");
          }
          task = it->second;
        }
        if (method == "tasks/cancel") {
          task->cancel_requested = true;
          {
            std::lock_guard lock(task->mutex);
            if (task->details.at("status") == "input_required") {
              task->details.as_object()["status"] = "cancelled";
              task->details.as_object().erase("inputRequests");
              task->details.as_object()["lastUpdatedAt"] = utc_now();
            }
          }
        } else {
          const auto& input = find(params, "inputResponses");
          if (!input.is_object()) throw Error(-32602, "inputResponses must be an object");
          bool resume = false;
          Json answers;
          {
            std::lock_guard lock(task->mutex);
            if (task->details.at("status") == "input_required") {
              auto& outstanding = task->details.as_object().at("inputRequests").as_object();
              for (const auto& [key, answer] : input.as_object()) {
                if (outstanding.contains(key)) {
                  task->responses[std::string(key)] = answer;
                  outstanding.erase(key);
                }
              }
              if (outstanding.empty()) {
                resume = true;
                answers = task->responses;
                task->responses.clear();
                task->details.as_object().erase("inputRequests");
                task->details.as_object()["status"] = "working";
                task->details.as_object()["lastUpdatedAt"] = utc_now();
              }
            }
          }
          if (resume) run_task(task, std::move(answers));
        }
        result = json::object{};
      }
    } else if (method == "resources/list") {
      json::array list;
      for (const auto& [uri, res] : resources_) list.push_back({{"uri", uri}, {"name", res.name}, {"mimeType", res.mime}});
      result = {{"resources", std::move(list)}};
      paginate_list(result.as_object().at("resources").as_array(), params, result, method, page_size_);
    } else if (method == "resources/read") {
      const auto uri = string_of(params, "uri");
      const auto it = resources_.find(uri);
      if (it != resources_.end()) {
        result = {{"contents", json::array{{{"uri", uri}, {"mimeType", it->second.mime}, {"text", it->second.reader()}}}}};
      } else {
        bool matched = false;
        for (const auto& [pattern, entry] : templates_) {
          auto bindings = template_match(pattern, uri);
          if (!bindings) continue;
          result = entry.reader(uri, *bindings);
          matched = true;
          break;
        }
        if (!matched) throw Error(-32602, "Resource not found: " + uri);
      }
    } else if (method == "resources/templates/list") {
      json::array list;
      for (const auto& [uri, t] : templates_) {
        Json entry = {{"uriTemplate", uri}, {"name", t.name}, {"mimeType", t.mime}};
        if (!t.description.empty()) entry.as_object()["description"] = t.description;
        list.push_back(std::move(entry));
      }
      result = {{"resourceTemplates", std::move(list)}};
      paginate_list(result.as_object().at("resourceTemplates").as_array(), params, result, method, page_size_);
    } else if (method == "prompts/list") {
      json::array list;
      for (const auto& [name, prompt] : prompts_) {
        Json desc = {{"name", name}, {"description", prompt.description}};
        if (!prompt.arguments.as_array().empty()) desc.as_object()["arguments"] = prompt.arguments;
        list.push_back(std::move(desc));
      }
      result = {{"prompts", std::move(list)}};
      paginate_list(result.as_object().at("prompts").as_array(), params, result, method, page_size_);
    } else if (method == "prompts/get") {
      const auto name = string_of(params, "name");
      const auto it = prompts_.find(name);
      if (it == prompts_.end()) throw Error(-32602, "Unknown prompt: " + name);
      const Json& args = find(params, "arguments");
      const json::object values = args.is_object() ? args.as_object() : json::object{};
      if (it->second.full_reader) result = it->second.full_reader(values);
      else result = {{"messages", json::array{{{"role", "user"}, {"content", {{"type", "text"}, {"text", it->second.reader(values)}}}}}}};
    } else if (method == "completion/complete" && completion_) {
      if (!params.is_object() || !find(params, "ref").is_object() || !find(params, "argument").is_object())
        throw Error(-32602, "Invalid completion request");
      result = completion_(params.as_object());
      if (!result.is_object() || !find(result, "completion").is_object() ||
          !find(find(result, "completion"), "values").is_array() ||
          find(find(result, "completion"), "values").as_array().size() > 100)
        throw Error(-32603, "Invalid completion result");
    } else if (auto registered = methods_.find(method); registered != methods_.end()) {
      if (!params.is_object()) throw Error(-32602, "Invalid method parameters");
      result = registered->second(params.as_object());
    } else if (method == "ping" && !modern) {
      result = json::object{};
    } else {
      throw Error(-32601, "Method not found: " + method);
    }
    if (!result.is_object()) throw Error(-32603, "Handler must return an object");
    if (modern && find(result, "resultType") == "task") {
      if (method != "tools/call" || !extensions_.contains("io.modelcontextprotocol/tasks") ||
          !find(find(find(meta, "io.modelcontextprotocol/clientCapabilities"), "extensions"),
                "io.modelcontextprotocol/tasks").is_object())
        throw Error(-32021, "Task handles require the io.modelcontextprotocol/tasks client extension");
    }
    if (modern && find(result, "resultType") == "input_required") {
      if (method != "tools/call" && method != "prompts/get" && method != "resources/read")
        throw Error(-32603, "input_required is not allowed for this method");
      const auto& requests = find(result, "inputRequests");
      if (!requests.is_null() && !requests.is_object()) throw Error(-32603, "Invalid inputRequests");
      if (requests.is_object()) for (auto& [key, item] : requests.as_object()) {
        const auto method_name = string_of(item, "method");
        if (method_name != "elicitation/create" && method_name != "sampling/createMessage" && method_name != "roots/list")
          throw Error(-32603, "Invalid input request type");
      }
    }
    result = complete(std::move(result), modern);
    if (modern) {
      json::object meta_out = find(result, "_meta").is_object() ? find(result, "_meta").as_object() : json::object{};
      meta_out["io.modelcontextprotocol/serverInfo"] = {{"name", name_}, {"version", version_}};
      result.as_object()["_meta"] = std::move(meta_out);
      if (method == "server/discover" || method == "tools/list" || method == "prompts/list" ||
          method == "resources/list" || method == "resources/templates/list" || method == "resources/read") {
        if (find(result, "resultType") == "complete") {
          result.as_object()["ttlMs"] = ttl_ms_;
          result.as_object()["cacheScope"] = cache_scope_;
        }
      }
    }
    return rpc_success(id, std::move(result));
  } catch (const Error& e) { return rpc_error(id, e.code, e.what()); }
  catch (const std::exception& e) { return rpc_error(id, -32603, e.what()); }
}

namespace {
struct ActiveRequest {
  Json token = nullptr;
  std::atomic_bool cancelled{false};
  std::mutex mutex;
  double last_progress = -std::numeric_limits<double>::infinity();
  std::function<void(const Json&)> emit;
};
thread_local ActiveRequest* current_request = nullptr;
struct RequestGuard {
  ActiveRequest* previous;
  explicit RequestGuard(ActiveRequest* request) : previous(current_request) {current_request = request;}
  ~RequestGuard() {current_request = previous;}
};
}
void Server::progress(double value, std::optional<double> total, std::string message) {
  auto* ctx = current_request;
  if (!ctx || ctx->token.is_null() || ctx->cancelled.load()) return;
  if (!std::isfinite(value) || (total && (!std::isfinite(*total) || *total < 0)))
    throw std::invalid_argument("Progress and total must be finite, non-negative values");
  Json event;
  {
    std::lock_guard lock(ctx->mutex);
    if (value <= ctx->last_progress) throw std::invalid_argument("Progress must increase");
    ctx->last_progress = value;
    Json params = {{"progressToken", ctx->token}, {"progress", value}};
    if (total) params.as_object()["total"] = *total;
    if (!message.empty()) params.as_object()["message"] = std::move(message);
    event = {{"jsonrpc", "2.0"}, {"method", "notifications/progress"}, {"params", std::move(params)}};
  }
  if (!ctx->cancelled.load()) ctx->emit(event);
}
bool Server::is_cancelled() const {return current_request && current_request->cancelled.load();}

void Server::run_stdio() {
  std::mutex stdout_mutex;
  std::mutex active_mutex;
  std::vector<std::shared_ptr<Subscription>> subscriptions;
  std::vector<std::thread> workers;
  std::map<std::string, std::shared_ptr<ActiveRequest>> active;
  auto output = [&](const Json& value) {
    std::lock_guard lock(stdout_mutex);
    std::cout << json::serialize(value) << '\n' << std::flush;
  };
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    try {
      Json request = json::parse(line);
      const auto method = string_of(request, "method");
      if (method == "notifications/cancelled") {
        const Json& cancelled = find(find(request, "params"), "requestId");
        std::lock_guard lock(active_mutex);
        for (auto& sub : subscriptions) if (sub->id == cancelled) close_subscription(sub);
        auto it = active.find(json::serialize(cancelled));
        if (it != active.end()) it->second->cancelled = true;
        continue;
      }
      if (method == "subscriptions/listen") {
        Json checked = handle(request);
        const Json& error_code = find(find(checked, "error"), "code");
        if (!error_code.is_int64() || error_code.as_int64() != -32601) {
          if (!checked.is_null()) output(checked);
          continue;
        }
        std::shared_ptr<Subscription> subscription;
        try {subscription = subscribe(find(request, "id"), find(find(request, "params"), "notifications"));}
        catch (const Error& e) {output(rpc_error(find(request, "id"), e.code, e.what())); continue;}
        subscriptions.push_back(subscription);
        workers.emplace_back([subscription, &output] {
          for (;;) {
            Json item;
            {
              std::unique_lock lock(subscription->mutex);
              subscription->ready.wait(lock, [&] {
                return subscription->closed || !subscription->messages.empty();
              });
              if (subscription->closed) return;
              item = std::move(subscription->messages.front());
              subscription->messages.pop_front();
            }
            output(item);
          }
        });
        continue;
      }
      if (find(request, "id").is_null()) {
        const auto result = handle(request);
        if (!result.is_null()) output(result);
        continue;
      }
      auto context = std::make_shared<ActiveRequest>();
      context->token = find(find(find(request, "params"), "_meta"), "progressToken");
      // A malformed progress token is ignored, not echoed.
      if (!context->token.is_string() && !context->token.is_int64() &&
          !context->token.is_uint64()) context->token = nullptr;
      context->emit = output;
      std::string key = json::serialize(find(request, "id"));
      {
        std::lock_guard lock(active_mutex);
        if (active.count(key)) {output(rpc_error(find(request, "id"), -32600, "Duplicate active request ID")); continue;}
        active[key] = context;
      }
      workers.emplace_back([this, request = std::move(request), key, context, &output, &active, &active_mutex] {
        RequestGuard guard(context.get());
        Json response = handle(request);
        if (!context->cancelled.load() && !response.is_null()) output(response);
        std::lock_guard lock(active_mutex);
        active.erase(key);
      });
    } catch (const std::exception&) {
      output(rpc_error(nullptr, -32700, "Parse error"));
    }
  }
  {
    std::lock_guard lock(active_mutex);
    for (auto& [id, request] : active) request->cancelled = true;
  }
  for (auto& sub : subscriptions) close_subscription(sub);
  for (auto& worker : workers) worker.join();
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
  if (req.method() == http::verb::get && req.target() == "/.well-known/oauth-protected-resource" &&
      server.http_requires_auth())
    return http_response(http::status::ok, server.protected_resource_metadata(), req.version());
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
  if (server.http_requires_auth()) {
    const std::string authorization = header(req, "Authorization");
    const std::string token = authorization.starts_with("Bearer ") ? authorization.substr(7) : std::string{};
    const std::string target = string_of(params, method == "resources/read" ? "uri" :
                                       method.starts_with("tasks/") ? "taskId" : "name");
    if (!server.verify_http(token, method, target)) {
      Response denied = http_response(http::status::unauthorized,
                           rpc_error(id, -32001, "Unauthorized"), req.version());
      const std::string resource = string_of(server.protected_resource_metadata(), "resource");
      const auto scheme_end = resource.find("://");
      const auto path_begin = scheme_end == std::string::npos ? std::string::npos :
                              resource.find('/', scheme_end + 3);
      const std::string base = resource.substr(0, path_begin);
      denied.set(http::field::www_authenticate,
                 "Bearer resource_metadata=\"" + base +
                 "/.well-known/oauth-protected-resource\"");
      return denied;
    }
  }
  const std::string ver = string_of(meta, "io.modelcontextprotocol/protocolVersion");
  if (ver.empty() || header(req, "MCP-Protocol-Version") != ver || header(req, "Mcp-Method") != method)
    return http_response(http::status::bad_request, rpc_error(id, -32020, "HeaderMismatch: version or method"), req.version());
  if (ver != ProtocolVersion)
    return http_response(http::status::bad_request,
      rpc_error(id, -32022, "Unsupported protocol version", {{"requested", ver}, {"supported", json::array{ProtocolVersion}}}), req.version());
  if (method == "tools/call" || method == "resources/read" || method == "prompts/get" ||
      method == "tasks/get" || method == "tasks/update" || method == "tasks/cancel") {
    const std::string expected = string_of(params, method == "resources/read" ? "uri" :
                            method.starts_with("tasks/") ? "taskId" : "name");
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
  auto address = asio::ip::make_address(host);
  if (!address.is_loopback())
    throw std::invalid_argument("Insecure HTTP must bind to loopback; deploy authenticated TLS proxy for remote access");
  asio::io_context io;
  tcp::acceptor listener(io, tcp::endpoint(address, port));
  for (;;) {
    tcp::socket socket(io);
    listener.accept(socket);
    std::thread([this, sock = std::move(socket), host, port]() mutable {
      try {
        beast::flat_buffer buffer;
        http::request_parser<http::string_body> parser;
        parser.body_limit(4 * 1024 * 1024);
        http::read(sock, buffer, parser);
        Request request = parser.release();
        Response checked = on_http(*this, request, host, port);
        Json message;
        try { message = json::parse(request.body()); } catch (...) {}
        if (string_of(message, "method") != "subscriptions/listen" ||
            checked.result() != http::status::not_found ||
            string_of(find(json::parse(checked.body()), "error"), "message") != "Method not found: subscriptions/listen") {
          http::write(sock, checked);
        } else {
          Json id = find(message, "id");
          const Json& filters = find(find(message, "params"), "notifications");
          std::shared_ptr<Subscription> subscription;
          try { subscription = subscribe(id, filters); }
          catch (const Error& e) {
            http::write(sock, http_response(http::status::bad_request, rpc_error(id, e.code, e.what())));
            return;
          }
          http::response<http::empty_body> header_msg{http::status::ok, request.version()};
          header_msg.set(http::field::content_type, "text/event-stream");
          header_msg.set(http::field::cache_control, "no-cache");
          header_msg.set("X-Accel-Buffering", "no");
          header_msg.chunked(true);
          header_msg.keep_alive(true);
          http::response_serializer<http::empty_body> serializer{header_msg};
          http::write_header(sock, serializer);
          auto send = [&sock](const std::string& payload) {
            asio::write(sock, http::make_chunk(asio::buffer(payload)));
          };
          for (;;) {
            Json next = nullptr;
            {
              std::unique_lock lock(subscription->mutex);
              if (!subscription->ready.wait_for(lock, std::chrono::seconds(15), [&] {
                return subscription->closed || !subscription->messages.empty();
              })) {
                lock.unlock();
                send(": keepalive\n\n");
                continue;
              }
              if (subscription->closed) break;
              next = std::move(subscription->messages.front());
              subscription->messages.pop_front();
            }
            send("data: " + json::serialize(next) + "\n\n");
          }
          Json completed = rpc_success(subscription->id,
            {{"resultType", "complete"}, {"_meta", {{"io.modelcontextprotocol/subscriptionId", subscription->id}}}});
          send("data: " + json::serialize(completed) + "\n\n");
          asio::write(sock, http::make_chunk_last());
        }
      } catch (const std::exception& e) {
        std::cerr << "MCP HTTP error: " << e.what() << '\n';
      }
      beast::error_code ec;
      sock.shutdown(tcp::socket::shutdown_both, ec);
    }).detach();
  }
}

namespace {
struct ProcessState {
  std::function<void(const Json&)> on_notification;
  bp::ipstream output;
  bp::opstream input;
  bp::child child;
  std::mutex mutex;
  ProcessState(const std::string& executable, const std::vector<std::string>& args,
               std::function<void(const Json&)> callback)
      : on_notification(std::move(callback)),
        child(executable, bp::args(args), bp::std_out > output, bp::std_in < input) {}
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
        if (find(response, "method").is_string() && on_notification) on_notification(response);
      } catch (const std::exception&) { throw std::runtime_error("Invalid JSON-RPC output from MCP server"); }
    }
    throw std::runtime_error("MCP server closed stdout");
  }
};
struct ParsedUrl {std::string host, port, target; bool tls = false;};
ParsedUrl parse_url(std::string url) {
  const bool tls = url.starts_with("https://");
  if (!tls && !url.starts_with("http://")) throw std::invalid_argument("URL must use http:// or https://");
  url.erase(0, tls ? 8 : 7);
  auto p = url.find('/');
  std::string authority = url.substr(0, p);
  std::string target = p == std::string::npos ? "/mcp" : url.substr(p);
  p = authority.rfind(':');
  if (p == std::string::npos) return {authority, tls ? "443" : "80", target, tls};
  return {authority.substr(0,p), authority.substr(p+1), target, tls};
}
struct HttpState {
  ParsedUrl url;
  std::map<std::string, Json> tool_schemas;
  std::mutex mutex;
  std::string token;
  explicit HttpState(const std::string& path, std::string bearer)
    : url(parse_url(path)), token(std::move(bearer)) {}
  Json send(const Json& msg) {
    std::lock_guard<std::mutex> lock(mutex);
    const std::string method = string_of(msg, "method");
    const Json& params = find(msg, "params");
    asio::io_context io;
    tcp::resolver resolver(io);
    Request req{http::verb::post, url.target, 11};
    req.set(http::field::host, url.host + ":" + url.port);
    req.set(http::field::user_agent, "myMcpTools/0.1");
    req.set(http::field::accept, "application/json, text/event-stream");
    req.set(http::field::content_type, "application/json");
    if (!token.empty()) req.set(http::field::authorization, "Bearer " + token);
    req.set("MCP-Protocol-Version", ProtocolVersion);
    req.set("Mcp-Method", method);
    if (method == "tools/call" || method == "prompts/get" || method == "resources/read" ||
        method == "tasks/get" || method == "tasks/update" || method == "tasks/cancel")
      req.set("Mcp-Name", encode_header(string_of(params, method == "resources/read" ? "uri" :
                                            method.starts_with("tasks/") ? "taskId" : "name")));
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
    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    if (url.tls) {
      asio::ssl::context ssl_context(asio::ssl::context::tls_client);
      ssl_context.set_default_verify_paths();
      beast::ssl_stream<beast::tcp_stream> stream(io, ssl_context);
      stream.set_verify_mode(asio::ssl::verify_peer);
      stream.set_verify_callback(asio::ssl::host_name_verification(url.host));
      if (!SSL_set_tlsext_host_name(stream.native_handle(), url.host.c_str()))
        throw std::runtime_error("TLS SNI initialization failed");
      beast::get_lowest_layer(stream).connect(resolver.resolve(url.host, url.port));
      stream.handshake(asio::ssl::stream_base::client);
      http::write(stream, req);
      http::read(stream, buffer, res);
      beast::error_code ec;
      stream.shutdown(ec);
    } else {
      beast::tcp_stream stream(io);
      stream.connect(resolver.resolve(url.host, url.port));
      http::write(stream, req);
      http::read(stream, buffer, res);
      beast::error_code ec;
      stream.socket().shutdown(tcp::socket::shutdown_both, ec);
    }
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

void Client::listen_http(const std::string& url, Json notifications,
                         std::function<bool(const Json&)> on_event) {
  if (!notifications.is_object() || !on_event) throw std::invalid_argument("Invalid subscription callback/filter");
  auto target = parse_url(url);
  asio::io_context io;
  tcp::resolver resolver(io);
  beast::tcp_stream stream(io);
  stream.connect(resolver.resolve(target.host, target.port));
  Json message = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "subscriptions/listen"},
    {"params", {{"notifications", std::move(notifications)},
                {"_meta", {{"io.modelcontextprotocol/protocolVersion", ProtocolVersion},
                           {"io.modelcontextprotocol/clientCapabilities", json::object{}},
                           {"io.modelcontextprotocol/clientInfo", {{"name", "mymcp-client"}, {"version", "1.0.0"}}}}}}}};
  Request req{http::verb::post, target.target, 11};
  req.set(http::field::host, target.host + ":" + target.port);
  req.set(http::field::accept, "application/json, text/event-stream");
  req.set(http::field::content_type, "application/json");
  req.set("MCP-Protocol-Version", ProtocolVersion);
  req.set("Mcp-Method", "subscriptions/listen");
  req.body() = json::serialize(message);
  req.prepare_payload();
  http::write(stream, req);
  beast::flat_buffer buffer;
  http::response_parser<http::buffer_body> parser;
  parser.body_limit((std::numeric_limits<std::uint64_t>::max)());
  http::read_header(stream, buffer, parser);
  if (parser.get().result() != http::status::ok ||
      !std::string(parser.get()[http::field::content_type]).starts_with("text/event-stream"))
    throw Error(-32603, "Server rejected SSE subscription");
  std::string pending;
  while (!parser.is_done()) {
    char chunk[4096];
    parser.get().body().data = chunk;
    parser.get().body().size = sizeof(chunk);
    beast::error_code ec;
    http::read_some(stream, buffer, parser, ec);
    const std::size_t n = sizeof(chunk) - parser.get().body().size;
    pending.append(chunk, n);
    for (;;) {
      const auto sep = pending.find("\n\n");
      if (sep == std::string::npos) break;
      std::string event = pending.substr(0, sep);
      pending.erase(0, sep + 2);
      std::string data;
      std::istringstream lines(event);
      std::string line;
      while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.starts_with("data:")) {
          if (!data.empty()) data += '\n';
          std::string segment = line.substr(5);
          if (!segment.empty() && segment[0] == ' ') segment.erase(0, 1);
          data += segment;
        }
      }
      if (!data.empty()) {
        if (!on_event(json::parse(data))) {
          beast::error_code ignored;
          stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
          return;
        }
      }
    }
    if (ec == http::error::need_buffer) continue;
    if (ec == asio::error::eof || ec == http::error::end_of_stream) break;
    if (ec) throw beast::system_error(ec);
  }
}

Client::Client(std::function<Json(const Json&)> send, std::string name)
    : send_(std::move(send)), name_(std::move(name)) {}
Client Client::process(const std::string& executable, std::vector<std::string> args,
                       std::function<void(const Json&)> on_notification) {
  auto state = std::make_shared<ProcessState>(executable, args, std::move(on_notification));
  return Client([state](const Json& message){ return state->send(message); });
}
Client Client::http(const std::string& url, std::string bearer_token) {
  auto state = std::make_shared<HttpState>(url, std::move(bearer_token));
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
    if (!legacy_) {
      json::object metadata = find(actual_params, "_meta").is_object() ?
                              find(actual_params, "_meta").as_object() : json::object{};
      metadata["io.modelcontextprotocol/protocolVersion"] = ProtocolVersion;
      metadata["io.modelcontextprotocol/clientCapabilities"] = capabilities_;
      metadata["io.modelcontextprotocol/clientInfo"] = {{"name", name_}, {"version", "1.0.0"}};
      actual_params.as_object()["_meta"] = std::move(metadata);
    }
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
Json Client::request_with_progress(const std::string& method, Json params, Json token) {
  if (!params.is_object() || !(token.is_string() || token.is_int64() || token.is_uint64()))
    throw std::invalid_argument("Invalid progress parameters/token");
  Json metadata = find(params, "_meta").is_object() ? find(params,"_meta") : Json(json::object{});
  metadata.as_object()["progressToken"] = std::move(token);
  params.as_object()["_meta"] = std::move(metadata);
  return request(method, std::move(params));
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
  if (http_ && !tools_known_) list_all("tools/list", "tools"); // discover paginated header annotations
  return request("tools/call", {{"name", name}, {"arguments", std::move(arguments)}});
}
Json Client::resources() { return request("resources/list"); }
Json Client::read(const std::string& uri) { return request("resources/read", {{"uri", uri}}); }
Json Client::prompts() { return request("prompts/list"); }
Json Client::resource_templates() { return request("resources/templates/list"); }
void Client::enable_tasks() { extension("io.modelcontextprotocol/tasks"); }
Json Client::get_task(const std::string& id) { return request("tasks/get", {{"taskId", id}}); }
Json Client::update_task(const std::string& id, Json responses) {
  return request("tasks/update", {{"taskId", id}, {"inputResponses", std::move(responses)}});
}
Json Client::cancel_task(const std::string& id) { return request("tasks/cancel", {{"taskId", id}}); }
Json Client::complete(Json ref, std::string name, std::string value, Json context) {
  return request("completion/complete", {{"ref", std::move(ref)},
         {"argument", {{"name", name}, {"value", value}}}, {"context", std::move(context)}});
}
void Client::extension(std::string name, Json settings) {
  if (connected_) throw std::logic_error("Extensions must be configured before discovery");
  if (name.find('/') == std::string::npos || !settings.is_object())
    throw std::invalid_argument("Invalid extension identifier/settings");
  if (!capabilities_.as_object().contains("extensions")) capabilities_.as_object()["extensions"] = json::object{};
  capabilities_.as_object().at("extensions").as_object()[name] = std::move(settings);
}
Json Client::list_all(const std::string& method, const std::string& array_field) {
  if (method != "tools/list" && method != "resources/list" && method != "prompts/list" &&
      method != "resources/templates/list") throw std::invalid_argument("Not a list operation");
  Json combined = json::array{};
  std::set<std::string> visited;
  Json params = json::object{};
  for (size_t i = 0; i < 10000; ++i) {
    auto response = request(method, params);
    if (!find(response, array_field).is_array()) throw Error(-32603, "Invalid list response");
    for (const auto& item : find(response, array_field).as_array()) combined.as_array().push_back(item);
    const auto& next = find(response, "nextCursor");
    if (next.is_null()) return combined;
    if (!next.is_string()) throw Error(-32603, "Invalid nextCursor");
    const std::string cursor(next.as_string());
    if (!visited.insert(cursor).second) throw Error(-32603, "Pagination loop detected");
    params = {{"cursor", cursor}};
  }
  throw Error(-32603, "Too many list pages");
}
Json Client::get_prompt(const std::string& name, Json arguments) {
  return request("prompts/get", {{"name", name}, {"arguments", std::move(arguments)}});
}
} // namespace mymcp

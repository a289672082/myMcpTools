#pragma once

#include <boost/json.hpp>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace mymcp {
namespace json = boost::json;
using Json = json::value;
inline constexpr const char* ProtocolVersion = "2026-07-28";
inline constexpr const char* LegacyProtocolVersion = "2025-11-25";

struct Error : std::runtime_error {
  int code;
  explicit Error(int c, std::string message) : std::runtime_error(std::move(message)), code(c) {}
};

template<class T> struct Arg {
  std::string name;
  std::string description;
  bool required = true;
};
template<class T> Arg<T> arg(std::string name, std::string description = "", bool required = true) {
  return {std::move(name), std::move(description), required};
}
namespace detail {
template<class T> struct optional_type : std::false_type {};
template<class T> struct optional_type<std::optional<T>> : std::true_type {};
template<class T> Json schema() {
  if constexpr (optional_type<T>::value) return schema<typename T::value_type>();
  else if constexpr (std::is_same_v<T, bool>) return {{"type", "boolean"}};
  else if constexpr (std::is_integral_v<T>) return {{"type", "integer"}};
  else if constexpr (std::is_floating_point_v<T>) return {{"type", "number"}};
  else if constexpr (std::is_same_v<T, std::string>) return {{"type", "string"}};
  else return {{"type", "object"}};
}
template<class T> T decode(const Json& j) {
  if constexpr (optional_type<T>::value) {
    if (j.is_null()) return std::nullopt;
    return decode<typename T::value_type>(j);
  } else if constexpr (std::is_same_v<T, Json>) return j;
  else if constexpr (std::is_same_v<T, std::string>) return std::string(j.as_string());
  else if constexpr (std::is_same_v<T, bool>) return j.as_bool();
  else if constexpr (std::is_integral_v<T>) {
    if (j.is_int64()) return static_cast<T>(j.as_int64());
    if (j.is_uint64()) return static_cast<T>(j.as_uint64());
    throw Error(-32602, "Expected integer argument");
  } else if constexpr (std::is_floating_point_v<T>) {
    if (j.is_double()) return static_cast<T>(j.as_double());
    if (j.is_int64()) return static_cast<T>(j.as_int64());
    if (j.is_uint64()) return static_cast<T>(j.as_uint64());
    throw Error(-32602, "Expected number argument");
  } else static_assert(!sizeof(T), "Unsupported argument type: use Json");
}
template<class T> T get_arg(const json::object& args, const Arg<T>& spec) {
  const auto* p = args.if_contains(spec.name);
  if (!p || p->is_null()) {
    if constexpr (optional_type<T>::value) return std::nullopt;
    throw Error(-32602, "Missing argument: " + spec.name);
  }
  try { return decode<T>(*p); }
  catch (const Error&) { throw; }
  catch (const std::exception&) { throw Error(-32602, "Invalid argument: " + spec.name); }
}
Json make_tool_result(const Json& value);
Json make_tool_result(const std::string& value);
Json make_tool_result(const char* value);
Json make_tool_result(bool value);
Json make_tool_result(int value);
Json make_tool_result(long value);
Json make_tool_result(long long value);
Json make_tool_result(double value);
} // namespace detail

class Server {
public:
  explicit Server(std::string name, std::string version = "1.0.0");
  using ToolHandler = std::function<Json(const json::object&)>;
  void add_tool(std::string name, std::string description, Json input_schema, ToolHandler handler);
  // Low-level advanced handler receives all tools/call params, including MRTR inputResponses.
  void add_tool_context(std::string name, std::string description, Json input_schema, ToolHandler handler);

  // C++ equivalent of Kotlin's addTool, including automatic JSON schema and argument conversion.
  template<class F, class... Ts>
  void tool(std::string name, std::string description, F fn, Arg<Ts>... arguments) {
    static_assert(std::is_invocable_v<F, Ts...>, "Handler argument types must match arg<T> declarations");
    json::object properties;
    json::array required;
    (void)std::initializer_list<int>{([&] {
      Json p = detail::schema<Ts>();
      if (!arguments.description.empty()) p.as_object()["description"] = arguments.description;
      properties[arguments.name] = std::move(p);
      if (arguments.required && !detail::optional_type<Ts>::value) required.emplace_back(arguments.name);
    }(), 0)...};
    Json schema = {{"type", "object"}, {"properties", std::move(properties)},
                   {"required", std::move(required)}, {"additionalProperties", false}};
    add_tool(std::move(name), std::move(description), std::move(schema),
      [f = std::move(fn), arguments...](const json::object& values) -> Json {
        try {
          if constexpr (std::is_void_v<std::invoke_result_t<F, Ts...>>) {
            std::invoke(f, detail::get_arg<Ts>(values, arguments)...);
            return detail::make_tool_result("OK");
          } else {
            return detail::make_tool_result(std::invoke(f, detail::get_arg<Ts>(values, arguments)...));
          }
        } catch (const std::exception& e) {
          return {{"content", json::array{{{"type", "text"}, {"text", e.what()}}}}, {"isError", true}};
        }
      });
  }
  void resource(std::string uri, std::string name, std::function<std::string()> reader, std::string mime_type = "text/plain");
  void prompt(std::string name, std::string description, std::function<std::string(const json::object&)> reader);
  void instructions(std::string value) { instructions_ = std::move(value); }
  Json handle(const Json& request, bool allow_legacy = true); // JSON-RPC request -> response; null for notifications
  void run_stdio();
  void run_http(unsigned short port = 3000, std::string host = "127.0.0.1");
  const Json* tool_schema(const std::string& name) const;
private:
  struct Tool { std::string description; Json schema; ToolHandler handler; bool with_context = false; };
  struct Resource { std::string name; std::string mime; std::function<std::string()> reader; };
  struct Prompt { std::string description; std::function<std::string(const json::object&)> reader; };
  std::string name_, version_, instructions_;
  std::map<std::string, Tool> tools_;
  std::map<std::string, Resource> resources_;
  std::map<std::string, Prompt> prompts_;
  bool legacy_initialized_ = false;
};

class Client {
public:
  explicit Client(std::function<Json(const Json&)> send, std::string name = "mymcp-client");
  static Client process(const std::string& executable, std::vector<std::string> args = {});
  static Client http(const std::string& url);
  Json discover();
  Json request(const std::string& method, Json params = json::object{});
  Json tools();
  Json call(const std::string& name, Json arguments = json::object{});
  Json resources();
  Json read(const std::string& uri);
  Json prompts();
  Json get_prompt(const std::string& name, Json arguments = json::object{});
  Json capabilities() const { return capabilities_; }
  // Callback receives inputRequests and returns a JSON object keyed by request IDs.
  // The SDK retries the original request with inputResponses and the opaque requestState.
  void on_input_required(std::function<Json(const Json&)> handler) {
    input_handler_ = std::move(handler);
    capabilities_.as_object()["elicitation"] = {{"form", json::object{}}};
  }
private:
  std::function<Json(const Json&)> send_;
  std::string name_;
  long long next_id_ = 1;
  Json capabilities_ = json::object{};
  bool connected_ = false;
  bool legacy_ = false;
  bool allow_legacy_ = true;
  Json discovery_ = nullptr;
  bool http_ = false;
  bool tools_known_ = false;
  std::function<Json(const Json&)> input_handler_;
};

} // namespace mymcp

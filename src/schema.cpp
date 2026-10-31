#include "mymcp/schema.hpp"
#include <cmath>
#include <limits>
#include <regex>
#include <set>
#include <string>

namespace mymcp {
namespace {
using Json = boost::json::value;
const Json* prop(const Json& v, boost::json::string_view k) {
  return v.is_object() ? v.as_object().if_contains(k) : nullptr;
}
std::string path_component(std::string s) {
  std::string result;
  for (char c : s) { if (c == '~') result += "~0"; else if (c == '/') result += "~1"; else result += c; }
  return result;
}
bool fail(std::string* why, const std::string& at, const std::string& message) {
  if (why) *why = (at.empty() ? std::string("/") : at) + ": " + message;
  return false;
}
bool number(const Json& v, double& result) {
  if (v.is_double()) result = v.as_double();
  else if (v.is_int64()) result = static_cast<double>(v.as_int64());
  else if (v.is_uint64()) result = static_cast<double>(v.as_uint64());
  else return false;
  return std::isfinite(result);
}
bool type_matches(const Json& v, const std::string& type) {
  if (type == "null") return v.is_null();
  if (type == "object") return v.is_object();
  if (type == "array") return v.is_array();
  if (type == "string") return v.is_string();
  if (type == "boolean") return v.is_bool();
  if (type == "number") { double n; return number(v,n); }
  if (type == "integer") { double n; return number(v,n) && std::trunc(n) == n; }
  return false;
}
const Json* resolve(const Json& root, std::string ref) {
  if (!ref.starts_with("#/")) return nullptr;
  const Json* cur = &root;
  for (std::size_t start = 2; start <= ref.size();) {
    const auto end = ref.find('/', start);
    std::string part = ref.substr(start, end == std::string::npos ? end : end - start);
    std::string decoded;
    for (std::size_t i = 0; i < part.size(); ++i) {
      if (part[i] == '~') {
        if (++i >= part.size()) return nullptr;
        if (part[i] == '0') decoded += '~';
        else if (part[i] == '1') decoded += '/';
        else return nullptr;
      } else decoded += part[i];
    }
    cur = prop(*cur, decoded);
    if (!cur || end == std::string::npos) return cur;
    start = end + 1;
  }
  return cur;
}
bool check(const Json& schema, const Json& value, const Json& root,
           std::string* why, const std::string& at, int depth) {
  if (depth > 64) return fail(why, at, "maximum schema recursion exceeded");
  if (schema.is_bool()) return schema.as_bool() || fail(why, at, "false schema");
  if (!schema.is_object()) return fail(why, at, "schema must be an object or boolean");
  if (const Json* reference = prop(schema, "$ref")) {
    if (!reference->is_string()) return fail(why, at, "invalid $ref");
    const Json* target = resolve(root, std::string(reference->as_string()));
    if (!target) return fail(why, at, "unsupported or unresolved $ref");
    if (!check(*target, value, root, why, at, depth + 1)) return false;
  }
  if (const Json* t = prop(schema, "type")) {
    bool ok = false;
    if (t->is_string()) ok = type_matches(value, std::string(t->as_string()));
    else if (t->is_array()) {
      for (const auto& entry : t->as_array())
        if (entry.is_string() && type_matches(value, std::string(entry.as_string()))) ok = true;
    }
    if (!ok) return fail(why, at, "type mismatch");
  }
  if (const Json* c = prop(schema, "const"))
    if (*c != value) return fail(why, at, "const mismatch");
  if (const Json* e = prop(schema, "enum")) {
    if (!e->is_array()) return fail(why, at, "invalid enum");
    bool ok = false;
    for (const auto& item : e->as_array()) if (item == value) ok = true;
    if (!ok) return fail(why, at, "not in enum");
  }
  for (const auto* key : {"allOf", "anyOf", "oneOf"}) {
    if (const Json* group = prop(schema, key)) {
      if (!group->is_array()) return fail(why, at, std::string("invalid ") + key);
      int matched = 0;
      for (const auto& sub : group->as_array())
        if (check(sub, value, root, nullptr, at, depth + 1)) ++matched;
      if ((std::string(key) == "allOf" && matched != int(group->as_array().size())) ||
          (std::string(key) == "anyOf" && !matched) ||
          (std::string(key) == "oneOf" && matched != 1))
        return fail(why, at, std::string(key) + " mismatch");
    }
  }
  if (const Json* negate = prop(schema, "not"))
    if (check(*negate, value, root, nullptr, at, depth+1)) return fail(why, at, "not mismatch");
  if (const Json* conditional = prop(schema, "if")) {
    const bool condition = check(*conditional, value, root, nullptr, at, depth + 1);
    const Json* branch = prop(schema, condition ? "then" : "else");
    if (branch && !check(*branch, value, root, why, at, depth+1)) return false;
  }
  if (value.is_object()) {
    const auto& o = value.as_object();
    if (const Json* r = prop(schema, "required")) {
      if (!r->is_array()) return fail(why, at, "invalid required");
      for (const auto& k : r->as_array())
        if (!k.is_string() || !o.contains(k.as_string())) return fail(why, at, "missing required property");
    }
    if (const Json* min = prop(schema, "minProperties"))
      if (min->is_int64() && o.size() < std::size_t(std::max<std::int64_t>(min->as_int64(), 0))) return fail(why,at,"minProperties");
    if (const Json* max = prop(schema, "maxProperties"))
      if (max->is_int64() && o.size() > std::size_t(std::max<std::int64_t>(max->as_int64(), 0))) return fail(why,at,"maxProperties");
    const Json* properties = prop(schema,"properties");
    const Json* patterns = prop(schema,"patternProperties");
    const Json* additional = prop(schema,"additionalProperties");
    for (const auto& [name, item] : o) {
      const std::string key(name);
      const std::string child = at + "/" + path_component(key);
      bool covered = false;
      if (properties && properties->is_object()) if (auto* sub = properties->as_object().if_contains(name)) {
        covered = true;
        if (!check(*sub,item,root,why,child,depth+1)) return false;
      }
      if (patterns && patterns->is_object()) for (const auto& [regex, sub] : patterns->as_object()) {
        try {
          if (std::regex_search(key,std::regex(std::string(regex)))) {
            covered = true;
            if (!check(sub,item,root,why,child,depth+1)) return false;
          }
        } catch(const std::regex_error&) { return fail(why,at,"invalid patternProperties regex"); }
      }
      if (!covered && additional && !check(*additional,item,root,why,child,depth+1)) return false;
    }
    if (const Json* dependencies = prop(schema,"dependentRequired")) {
      if (!dependencies->is_object()) return fail(why,at,"invalid dependentRequired");
      for (const auto& [key, required] : dependencies->as_object()) if (o.contains(key)) {
        if (!required.is_array()) return fail(why,at,"invalid dependentRequired");
        for (const auto& r : required.as_array()) if (!r.is_string() || !o.contains(r.as_string()))
          return fail(why,at,"dependentRequired mismatch");
      }
    }
  }
  if (value.is_array()) {
    const auto& a = value.as_array();
    if (const Json* min = prop(schema,"minItems"))
      if (min->is_int64() && a.size() < std::size_t(std::max<std::int64_t>(min->as_int64(),0))) return fail(why,at,"minItems");
    if (const Json* max = prop(schema,"maxItems"))
      if (max->is_int64() && a.size() > std::size_t(std::max<std::int64_t>(max->as_int64(),0))) return fail(why,at,"maxItems");
    if (const Json* unique = prop(schema,"uniqueItems")) if (unique->is_bool() && unique->as_bool()) {
      for (size_t i=0;i<a.size();++i) for(size_t j=0;j<i;++j)
        if (a[i]==a[j]) return fail(why,at,"uniqueItems");
    }
    const Json* prefix = prop(schema,"prefixItems");
    const std::size_t pref = prefix && prefix->is_array() ? prefix->as_array().size() : 0;
    for (std::size_t i=0;i<a.size();++i) {
      const Json* sub = nullptr;
      if (i<pref) sub = &prefix->as_array()[i];
      else sub = prop(schema,"items");
      if (sub && !check(*sub,a[i],root,why,at+"/"+std::to_string(i),depth+1)) return false;
    }
    if (const Json* contains = prop(schema,"contains")) {
      std::size_t count = 0;
      for (const auto& item : a) if (check(*contains,item,root,nullptr,at,depth+1)) ++count;
      std::size_t min = 1, max = std::numeric_limits<std::size_t>::max();
      if (auto* m=prop(schema,"minContains")) if(m->is_int64() && m->as_int64()>=0) min=m->as_int64();
      if (auto* m=prop(schema,"maxContains")) if(m->is_int64() && m->as_int64()>=0) max=m->as_int64();
      if (count<min || count>max) return fail(why,at,"contains mismatch");
    }
  }
  if (value.is_string()) {
    const std::string s(value.as_string());
    // ASCII length is exact; non-ASCII UTF-8 is counted in Unicode scalar values.
    std::size_t count = 0;
    for (unsigned char c : s) if ((c & 0xc0) != 0x80) ++count;
    if (const Json* m=prop(schema,"minLength")) if (m->is_int64() && m->as_int64()>0 && count<std::size_t(m->as_int64())) return fail(why,at,"minLength");
    if (const Json* m=prop(schema,"maxLength")) if (m->is_int64() && m->as_int64()>=0 && count>std::size_t(m->as_int64())) return fail(why,at,"maxLength");
    if (const Json* pattern=prop(schema,"pattern")) if (pattern->is_string()) {
      try { if (!std::regex_search(s,std::regex(std::string(pattern->as_string())))) return fail(why,at,"pattern mismatch"); }
      catch (const std::regex_error&) {return fail(why,at,"invalid pattern regex");}
    }
  }
  double n;
  if (number(value,n)) {
    for (auto* key : {"minimum","maximum","exclusiveMinimum","exclusiveMaximum"}) {
      if (const Json* bound=prop(schema,key)) { double b;
        if (number(*bound,b) && ((std::string(key)=="minimum" && n<b) ||
          (std::string(key)=="maximum" && n>b) ||
          (std::string(key)=="exclusiveMinimum" && n<=b) ||
          (std::string(key)=="exclusiveMaximum" && n>=b))) return fail(why,at,std::string(key));
      }
    }
    if (const Json* multiple=prop(schema,"multipleOf")) { double m;
      if (number(*multiple,m) && m>0 && std::abs(n/m-std::round(n/m))>1e-9) return fail(why,at,"multipleOf");
    }
  }
  return true;
}
}
bool validate_schema(const boost::json::value& schema, const boost::json::value& instance, std::string* error) {
  return check(schema,instance,schema,error,"",0);
}
}

#include "mymcp/oauth_client.hpp"
#include <httplib.h>
#include <jsoncons/json.hpp>
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <sstream>
#include <limits>

namespace mymcp::oauth {
namespace {
using Json = jsoncons::json;
struct Url {std::string scheme,host,port,path;};
Url parse_url(const std::string& u) {
  const auto p=u.find("://");
  if(p==std::string::npos)throw std::invalid_argument("Invalid OAuth URL");
  Url x; x.scheme=u.substr(0,p);
  const auto first=p+3;
  const auto slash=u.find('/',first);
  std::string authority=u.substr(first,slash==std::string::npos?slash:slash-first);
  if(authority.empty()||authority.find('@')!=std::string::npos||authority.find('#')!=std::string::npos)
    throw std::invalid_argument("Invalid OAuth URL authority");
  if(authority.front()=='['){
    auto end=authority.find(']');if(end==std::string::npos)throw std::invalid_argument("Invalid IPv6 URL");
    x.host=authority.substr(1,end-1);
    x.port=end+1==authority.size()?(x.scheme=="https"?"443":"80"):authority.substr(end+2);
  }else{
    auto pos=authority.find(':');x.host=authority.substr(0,pos);
    x.port=pos==std::string::npos?(x.scheme=="https"?"443":"80"):authority.substr(pos+1);
  }
  x.path=slash==std::string::npos?"/":u.substr(slash);
  if(x.path.find('#')!=std::string::npos||x.host.empty()||x.port.empty())
    throw std::invalid_argument("Invalid OAuth URL");
  return x;
}
bool loopback(const std::string& host){return host=="127.0.0.1"||host=="localhost"||host=="::1";}
void safe_endpoint(const std::string& u){
  auto p=parse_url(u);
  if(p.scheme!="https"&&!(p.scheme=="http"&&loopback(p.host)))
    throw std::invalid_argument("OAuth URL requires HTTPS outside loopback");
}
std::unique_ptr<httplib::Client> make_client(const std::string& u){
  safe_endpoint(u);
  auto x=parse_url(u);
  std::string authority=x.host.find(':')!=std::string::npos?"["+x.host+"]":x.host;
  auto c=std::make_unique<httplib::Client>(x.scheme+"://"+authority+":"+x.port);
  c->enable_server_certificate_verification(true);
  c->set_follow_location(false);
  c->set_connection_timeout(5,0);
  c->set_read_timeout(10,0);
  return c;
}
std::string require_string(const Json& v,const std::string& k){
  if(!v.is_object()||!v.contains(k)||!v.at(k).is_string())throw std::runtime_error("Missing OAuth field: "+k);
  return v.at(k).as_string();
}
std::vector<std::string> strings(const Json& j,const std::string& key){
  std::vector<std::string> out;
  if(!j.is_object()||!j.contains(key))return out;
  if(!j.at(key).is_array())throw std::runtime_error("OAuth field is not an array: "+key);
  for(const auto& v:j.at(key).array_range()){
    if(!v.is_string())throw std::runtime_error("Invalid OAuth array member");
    out.push_back(v.as_string());
  }
  return out;
}
Json get_json(const std::string& url){
  auto c=make_client(url);
  auto p=parse_url(url);
  auto response=c->Get(p.path);
  if(!response||response->status!=200||response->body.size()>1024*1024)
    throw std::runtime_error("OAuth discovery GET failed");
  return Json::parse(response->body);
}
Json post_form(const std::string& endpoint,const httplib::Params& params){
  auto c=make_client(endpoint);
  auto r=c->Post(parse_url(endpoint).path,params);
  if(!r||r->body.size()>1024*1024)throw std::runtime_error("OAuth token endpoint failed");
  Json j=Json::parse(r->body);
  if(r->status!=200)throw std::runtime_error("OAuth token endpoint rejected the request (HTTP "+std::to_string(r->status)+")");
  return j;
}
Token parse_token(const Json& j,const std::string& preserved_refresh={}){
  Token t;
  t.access_token=require_string(j,"access_token");
  t.token_type=require_string(j,"token_type");
  if(t.token_type!="Bearer"&&t.token_type!="bearer")throw std::runtime_error("Unsupported OAuth token_type");
  t.refresh_token=j.contains("refresh_token")?require_string(j,"refresh_token"):preserved_refresh;
  if(j.contains("scope"))t.scope=require_string(j,"scope");
  if(j.contains("expires_in")){
    auto seconds=j.at("expires_in").as_integer<std::int64_t>();
    if(seconds<=0||seconds>315360000)throw std::runtime_error("Invalid expires_in");
    t.expires_at=std::chrono::system_clock::now()+std::chrono::seconds(seconds);
  }
  return t;
}
} // namespace
bool Token::expired(std::chrono::seconds margin) const{
  return expires_at!=std::chrono::system_clock::time_point{} &&
      std::chrono::system_clock::now()+margin>=expires_at;
}
Session::Session(std::string resource):resource_url_(std::move(resource)){safe_endpoint(resource_url_);}
void Session::client_id(std::string value){
  if(value.empty())throw std::invalid_argument("OAuth client ID must not be empty");
  client_id_=std::move(value);
}
void Session::discover(){
  auto target=parse_url(resource_url_);
  std::string origin=target.scheme+"://"+(target.host.find(':')!=std::string::npos?"["+target.host+"]":target.host);
  if(target.port!=(target.scheme=="https"?"443":"80"))origin+=":"+target.port;
  // RFC 9728: path-qualified resource metadata gets priority over root metadata.
  std::vector<std::string> paths;
  if(target.path!="/")paths.push_back(origin+"/.well-known/oauth-protected-resource"+target.path);
  paths.push_back(origin+"/.well-known/oauth-protected-resource");
  Json protected_resource;
  bool found=false;
  for(const auto& path:paths){try{protected_resource=get_json(path);found=true;break;}catch(const std::exception&){}}
  if(!found)throw std::runtime_error("MCP protected resource metadata not found");
  const auto resource=require_string(protected_resource,"resource");
  if(resource!=resource_url_)throw std::runtime_error("MCP OAuth resource identifier mismatch");
  auto servers=strings(protected_resource,"authorization_servers");
  if(servers.empty())throw std::runtime_error("No MCP authorization servers");
  const auto issuer=servers[0];safe_endpoint(issuer);
  auto as=parse_url(issuer);
  std::string base=as.scheme+"://"+(as.host.find(':')!=std::string::npos?"["+as.host+"]":as.host);
  if(as.port!=(as.scheme=="https"?"443":"80"))base+=":"+as.port;
  // RFC 8414 and OIDC discovery paths, including issuer path.
  const auto suffix=as.path=="/"?"":as.path;
  const std::vector<std::string> candidates{
    base+"/.well-known/oauth-authorization-server"+suffix,
    base+"/.well-known/openid-configuration"+suffix,
    issuer+(issuer.back()=='/'?"":"/")+".well-known/openid-configuration"
  };
  Json info;bool resolved=false;
  for(const auto& url:candidates){try{info=get_json(url);resolved=true;break;}catch(const std::exception&){}}
  if(!resolved)throw std::runtime_error("Authorization server metadata discovery failed");
  if(require_string(info,"issuer")!=issuer)throw std::runtime_error("Authorization server issuer mismatch");
  Metadata next;
  next.resource=resource;next.issuer=issuer;
  next.authorization_endpoint=require_string(info,"authorization_endpoint");
  next.token_endpoint=require_string(info,"token_endpoint");
  safe_endpoint(next.authorization_endpoint);safe_endpoint(next.token_endpoint);
  if(info.contains("registration_endpoint")){
    next.registration_endpoint=require_string(info,"registration_endpoint");
    safe_endpoint(next.registration_endpoint);
  }
  if(info.contains("authorization_response_iss_parameter_supported"))
    next.issuer_parameter_supported=info.at("authorization_response_iss_parameter_supported").as_bool();
  next.scopes_supported=strings(protected_resource,"scopes_supported");
  metadata_=std::move(next);
}
std::string Session::register_dynamic(const std::vector<std::string>& redirects,std::string name){
  if(metadata_.registration_endpoint.empty())throw std::runtime_error("Dynamic registration not advertised");
  if(redirects.empty())throw std::invalid_argument("Redirect URIs required");
  Json body(jsoncons::json_object_arg);
  body["client_name"]=std::move(name);
  body["token_endpoint_auth_method"]="none";
  body["grant_types"]=Json::make_array({"authorization_code","refresh_token"});
  Json r=jsoncons::json::make_array();for(const auto& u:redirects)r.push_back(u);
  body["redirect_uris"]=std::move(r);
  auto c=make_client(metadata_.registration_endpoint);
  auto response=c->Post(parse_url(metadata_.registration_endpoint).path,body.to_string(),"application/json");
  if(!response || (response->status!=200 && response->status!=201))throw std::runtime_error("Dynamic OAuth registration failed");
  auto registration=Json::parse(response->body);
  client_id_=require_string(registration,"client_id");
  return client_id_;
}
Authorization Session::begin_authorization(std::string redirect,const std::vector<std::string>& scopes){
  if(metadata_.issuer.empty())discover();
  if(client_id_.empty())throw std::logic_error("Configure OAuth client_id or register dynamically");
  if(redirect.empty())throw std::invalid_argument("Redirect URI required");
  redirect_uri_=std::move(redirect);
  return begin(metadata_.authorization_endpoint,metadata_.issuer,client_id_,redirect_uri_,resource_url_,
               scopes.empty()?metadata_.scopes_supported:scopes,metadata_.issuer_parameter_supported);
}
Token Session::exchange_code(const Authorization& flow,const std::string& code,
    const std::string& state,const std::string& issuer,const std::string& redirect){
  validate_callback(flow,state,issuer);
  if(code.empty()||redirect!=redirect_uri_)throw std::invalid_argument("Invalid OAuth authorization callback");
  if(flow.expected_issuer!=metadata_.issuer)throw std::runtime_error("OAuth issuer changed");
  httplib::Params pjson::object{{"grant_type","authorization_code"},{"code",code},{"redirect_uri",redirect},
    {"client_id",client_id_},{"code_verifier",flow.code_verifier},{"resource",resource_url_}};
  auto t=parse_token(post_form(metadata_.token_endpoint,p));
  {std::lock_guard lock(mutex_);token_=t;token_available_=true;}
  return t;
}
Token Session::refresh(){
  std::lock_guard lock(mutex_);
  if(!token_available_||token_.refresh_token.empty())throw std::runtime_error("No OAuth refresh token available");
  httplib::Params pjson::object{{"grant_type","refresh_token"},{"refresh_token",token_.refresh_token},
    {"client_id",client_id_},{"resource",resource_url_}};
  Token updated=parse_token(post_form(metadata_.token_endpoint,p),token_.refresh_token);
  token_=updated;return updated;
}
std::string Session::bearer_token(){
  {std::lock_guard lock(mutex_);if(!token_available_)throw std::runtime_error("OAuth authorization required");
   if(!token_.expired())return token_.access_token;}
  return refresh().access_token;
}
bool Session::has_token()const{std::lock_guard lock(mutex_);return token_available_;}
} // namespace mymcp::oauth

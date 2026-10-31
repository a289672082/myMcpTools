#include "mymcp/oauth.hpp"
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <string_view>

namespace mymcp::oauth {
namespace {
std::string base64url(const unsigned char* bytes, std::size_t size) {
  std::string b64(4 * ((size + 2) / 3), '\0');
  EVP_EncodeBlock(reinterpret_cast<unsigned char*>(b64.data()), bytes, static_cast<int>(size));
  while (!b64.empty() && b64.back() == '=') b64.pop_back();
  for (auto& c : b64) {if (c == '+') c = '-'; if (c == '/') c = '_';}
  return b64;
}
std::string nonce(std::size_t bytes) {
  std::string random(bytes, '\0');
  if (RAND_bytes(reinterpret_cast<unsigned char*>(random.data()), static_cast<int>(bytes)) != 1)
    throw std::runtime_error("CSPRNG failure");
  return base64url(reinterpret_cast<const unsigned char*>(random.data()), bytes);
}
void require_https(const std::string& url) {
  if (!url.starts_with("https://")) throw std::invalid_argument("OAuth endpoints must use HTTPS");
  if (url.find('#') != std::string::npos) throw std::invalid_argument("OAuth endpoint must not have a fragment");
}
}
std::string url_encode(const std::string& input) {
  std::ostringstream out;
  out << std::uppercase << std::hex;
  for (unsigned char c : input) {
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.' || c == '~') out << char(c);
    else out << '%' << std::setw(2) << std::setfill('0') << int(c);
  }
  return out.str();
}
Authorization begin(const std::string& authorization_endpoint,
                    const std::string& expected_issuer,
                    const std::string& client_id,
                    const std::string& redirect_uri,
                    const std::string& resource,
                    const std::vector<std::string>& scopes,
                    bool issuer_parameter_supported) {
  require_https(authorization_endpoint);
  require_https(expected_issuer);
  if (client_id.empty() || redirect_uri.empty() || resource.empty())
    throw std::invalid_argument("OAuth requires client_id, redirect_uri and resource");
  if (!resource.starts_with("https://") && !resource.starts_with("http://127.0.0.1") &&
      !resource.starts_with("http://localhost"))
    throw std::invalid_argument("OAuth resource must be HTTPS (or loopback)");
  Authorization flow;
  flow.state = nonce(32);
  flow.code_verifier = nonce(48);
  flow.expected_issuer = expected_issuer;
  flow.issuer_parameter_supported = issuer_parameter_supported;
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  if (EVP_Digest(flow.code_verifier.data(), flow.code_verifier.size(),
                 digest, &len, EVP_sha256(), nullptr) != 1)
    throw std::runtime_error("PKCE SHA-256 failure");
  const auto challenge = base64url(digest, len);
  std::string scope;
  for (const auto& item : scopes) {
    if (!scope.empty()) scope += ' ';
    scope += item;
  }
  flow.url = authorization_endpoint;
  flow.url += (flow.url.find('?') == std::string::npos ? '?' : '&');
  flow.url += "response_type=code&client_id=" + url_encode(client_id) +
              "&redirect_uri=" + url_encode(redirect_uri) +
              "&code_challenge_method=S256&code_challenge=" + url_encode(challenge) +
              "&state=" + url_encode(flow.state) + "&resource=" + url_encode(resource);
  if (!scope.empty()) flow.url += "&scope=" + url_encode(scope);
  return flow;
}
void validate_callback(const Authorization& flow, const std::string& state, const std::string& issuer) {
  if (state.size() != flow.state.size() ||
      CRYPTO_memcmp(state.data(), flow.state.data(), state.size()) != 0)
    throw std::invalid_argument("OAuth callback state mismatch");
  if ((flow.issuer_parameter_supported && issuer.empty()) ||
      (!issuer.empty() && issuer != flow.expected_issuer))
    throw std::invalid_argument("OAuth callback issuer mismatch or missing issuer");
}
}

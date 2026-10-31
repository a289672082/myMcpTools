#pragma once
#include <boost/json.hpp>
#include <string>

namespace mymcp {
// Checks the supported JSON Schema 2020-12 assertions. This is intentionally a
// SUBSET validator; it does not claim complete 2020-12 conformance.
// On failure, error receives an instance path and a short explanation.
bool validate_schema(const boost::json::value& schema,
                     const boost::json::value& instance,
                     std::string* error = nullptr);
}

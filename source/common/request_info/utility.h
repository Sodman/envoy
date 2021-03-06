#pragma once

#include <chrono>
#include <cstdint>

#include "envoy/request_info/request_info.h"

namespace Envoy {
namespace RequestInfo {

/**
 * Util class for ResponseFlags.
 */
class ResponseFlagUtils {
public:
  static const std::string toShortString(const RequestInfo& request_info);

private:
  ResponseFlagUtils();
  static void appendString(std::string& result, const std::string& append);

  const static std::string NONE;
  const static std::string FAILED_LOCAL_HEALTH_CHECK;
  const static std::string NO_HEALTHY_UPSTREAM;
  const static std::string UPSTREAM_REQUEST_TIMEOUT;
  const static std::string LOCAL_RESET;
  const static std::string UPSTREAM_REMOTE_RESET;
  const static std::string UPSTREAM_CONNECTION_FAILURE;
  const static std::string UPSTREAM_CONNECTION_TERMINATION;
  const static std::string UPSTREAM_OVERFLOW;
  const static std::string NO_ROUTE_FOUND;
  const static std::string DELAY_INJECTED;
  const static std::string FAULT_INJECTED;
  const static std::string RATE_LIMITED;
};

} // namespace RequestInfo
} // namespace Envoy

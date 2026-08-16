#ifndef FIBER_ACCESS_SERVER_ACCESS_ACTIVATION_ENDPOINT_H
#define FIBER_ACCESS_SERVER_ACCESS_ACTIVATION_ENDPOINT_H

#include "../observability/AccessActivationEvidence.h"
#include "../observability/AccessDiscoveryMetrics.h"

#include <string>
#include <string_view>

#include <fiber/async/Task.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>

namespace fiber::http {
class HttpExchange;
}

namespace fiber::access_server {

inline constexpr std::string_view kAccessActivationEvidencePath = "/v1/activation-evidence";

struct AccessActivationEndpointOptions {
    bool enabled = false;
    std::string instance_id;
    std::string bearer_token;
};

class AccessActivationEndpoint final : public common::NonCopyable, public common::NonMovable {
public:
    AccessActivationEndpoint(const AccessActivationEvidenceStore *evidence, const AccessDiscoveryMetrics *discovery,
                             AccessActivationEndpointOptions options);

    [[nodiscard]] bool enabled() const noexcept { return options_.enabled; }
    [[nodiscard]] async::Task<void> handle(http::HttpExchange &exchange) const noexcept;

private:
    const AccessActivationEvidenceStore *evidence_ = nullptr;
    const AccessDiscoveryMetrics *discovery_ = nullptr;
    AccessActivationEndpointOptions options_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_ACTIVATION_ENDPOINT_H

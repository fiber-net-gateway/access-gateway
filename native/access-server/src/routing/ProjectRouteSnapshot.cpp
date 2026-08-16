#include "ProjectRouteSnapshot.h"

#include <utility>

#include <fiber/http/HttpHeaderHash.h>

namespace fiber::access_server {
namespace {

std::string lowcase_header_name(std::string_view name) {
    std::string result(name);
    for (char &ch: result) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return result;
}

} // namespace

CompiledResponseHeaderTemplate::CompiledResponseHeaderTemplate(std::string original_name,
                                                               CompiledTemplate compiled_value) :
    name(std::move(original_name)), lowcase_name(lowcase_header_name(name)),
    name_hash(http::http_header_name_hash(name)), value(std::move(compiled_value)) {}

const CompiledHost *ProjectRouteSnapshot::match_host(std::string_view host) const noexcept {
    const std::optional<std::uint32_t> index = host_matcher_.match(host);
    if (!index) {
        return nullptr;
    }
    return &hosts_[*index];
}

async::Task<std::expected<void, ProxyAddressReadyError>> ProjectRouteSnapshot::wait_ready() const noexcept {
    for (const CompiledRoute &route: routes_) {
        if (!route.proxy || !route.proxy->address_selector) {
            continue;
        }
        auto ready = co_await route.proxy->address_selector->wait_ready();
        if (!ready) {
            co_return std::unexpected(ready.error());
        }
    }
    co_return std::expected<void, ProxyAddressReadyError>{};
}

bool ProjectRouteSnapshot::ready_for_publish() const noexcept {
    for (const CompiledRoute &route: routes_) {
        if (route.proxy && route.proxy->address_selector && !route.proxy->address_selector->ready_for_publish()) {
            return false;
        }
    }
    return true;
}

} // namespace fiber::access_server

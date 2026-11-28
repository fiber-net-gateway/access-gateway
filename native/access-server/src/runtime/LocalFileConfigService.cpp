#include "LocalFileConfigService.h"

#include "../observability/AccessServerLogCategories.h"

#include <algorithm>
#include <set>
#include <utility>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/WhenAny.h>
#include <fiber/common/Assert.h>
#include <fiber/log/Log.h>

namespace fiber::access_server {
namespace {

DEFINE_LOGGER(LOG_CONFIG, kAccessServerConfigLogger);

} // namespace

LocalFileConfigService::LocalFileConfigService(event::EventLoop &loop, LocalFileConfigServiceOptions options) :
    loop_(&loop), source_(std::move(options.directory), std::move(options.layout)),
    poll_interval_(options.poll_interval), not_found_data_(make_not_found_data()) {
    status_publisher_ = status_.acquire_publisher();
    FIBER_ASSERT(status_publisher_.has_value());
    stop_publisher_ = stop_signal_.acquire_publisher();
    FIBER_ASSERT(stop_publisher_.has_value());
    reload_publisher_ = reload_trigger_.acquire_publisher();
    FIBER_ASSERT(reload_publisher_.has_value());
}

LocalFileConfigService::~LocalFileConfigService() noexcept {
    FIBER_ASSERT(state_ == State::Created || state_ == State::Stopped);
    FIBER_ASSERT(subscribers_.empty());
    FIBER_ASSERT(background_tasks_.empty());
}

std::shared_ptr<const nacos::ConfigData> LocalFileConfigService::make_present_data(std::string md5,
                                                                                   std::string content) {
    auto owned = std::make_shared<OwnedConfigData>();
    owned->md5 = std::move(md5);
    owned->content = std::move(content);
    owned->view.state = nacos::ConfigState::Present;
    owned->view.md5 = owned->md5;
    owned->view.content = owned->content;
    return std::shared_ptr<const nacos::ConfigData>(std::move(owned), &owned->view);
}

std::shared_ptr<const nacos::ConfigData> LocalFileConfigService::make_not_found_data() {
    auto owned = std::make_shared<OwnedConfigData>();
    owned->view.state = nacos::ConfigState::NotFound;
    return std::shared_ptr<const nacos::ConfigData>(std::move(owned), &owned->view);
}

common::IoResult<void> LocalFileConfigService::start() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ != State::Created) {
        return std::unexpected(common::IoErr::Already);
    }

    status_publisher_->publish(nacos::ConfigServiceStatus{
            .connection =
                    {
                            .phase = nacos::NacosServicePhase::Connecting,
                    },
    });

    auto loaded = source_.load();
    if (!loaded) {
        last_reload_error_ = loaded.error();
        ++failed_reloads_;
        LOG(LOG_CONFIG, ERROR) << "local_file_config_start_failed"
                               << " path=\"" << loaded.error().path << "\""
                               << " detail=\"" << loaded.error().detail << "\"";
        status_publisher_->publish(nacos::ConfigServiceStatus{
                .connection =
                        {
                                .phase = nacos::NacosServicePhase::Stopped,
                                .failure = nacos::NacosServiceFailureCategory::Protocol,
                        },
        });
        return std::unexpected(loaded.error().io_error == common::IoErr::None ? common::IoErr::Invalid
                                                                              : loaded.error().io_error);
    }

    documents_.clear();
    for (auto &[key, doc]: loaded->documents) {
        documents_.emplace(key, make_present_data(std::move(doc.md5), std::move(doc.content)));
    }
    last_reload_error_.reset();
    state_ = State::Running;
    status_publisher_->publish(nacos::ConfigServiceStatus{
            .connection =
                    {
                            .phase = nacos::NacosServicePhase::Ready,
                            .rpc_available = true,
                            .connection_ready_count = 1,
                    },
    });
    background_tasks_.add();
    async::spawn(*loop_, [this]() { return poll_loop(); });
    LOG(LOG_CONFIG, INFO) << "local_file_config_started"
                          << " directory=\"" << source_.directory() << "\""
                          << " documents=" << documents_.size() << " poll_interval_ms=" << poll_interval_.count();
    return {};
}

async::Task<void> LocalFileConfigService::shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ == State::Stopped) {
        co_return;
    }
    state_ = State::Stopping;
    status_publisher_->publish(nacos::ConfigServiceStatus{
            .connection =
                    {
                            .phase = nacos::NacosServicePhase::Stopping,
                            .failure = nacos::NacosServiceFailureCategory::Shutdown,
                    },
    });
    stop_publisher_->publish(true);
    co_await background_tasks_.join();

    // Deliver Closed to any remaining subscribers and detach them cleanly.
    auto remaining = std::move(subscribers_);
    subscribers_.clear();
    for (auto &[key, nodes]: remaining) {
        (void) key;
        for (SubscriptionNode *node: nodes) {
            node->attached = false;
            node->closed = true;
            if (node->on_notify) {
                node->on_notify(node->ctx, nacos::SubscriptionResult<nacos::ConfigData>{
                                                   .kind = nacos::ResultKind::Closed,
                                           });
            }
        }
    }
    documents_.clear();
    state_ = State::Stopped;
    status_publisher_->publish(nacos::ConfigServiceStatus{
            .connection =
                    {
                            .phase = nacos::NacosServicePhase::Stopped,
                            .failure = nacos::NacosServiceFailureCategory::Shutdown,
                    },
    });
}

LocalFileConfigService::StatusSubscriber LocalFileConfigService::subscribe_status() { return status_.subscribe(); }

async::Task<std::expected<std::shared_ptr<const nacos::ConfigData>, nacos::ConfigServiceError>>
LocalFileConfigService::get_config(std::string data_id, std::string group) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ != State::Running) {
        co_return std::unexpected(nacos::ConfigServiceError{
                .code = nacos::ConfigServiceErrorCode::Shutdown,
                .io_error = common::IoErr::Canceled,
                .message = "local file config service is not running",
        });
    }
    const auto found = documents_.find(LocalFileConfigKey{.data_id = std::move(data_id), .group = std::move(group)});
    if (found == documents_.end()) {
        co_return not_found_data_;
    }
    co_return found->second;
}

async::Task<std::expected<void, nacos::ConfigServiceError>>
LocalFileConfigService::publish(std::string, std::string, std::string, nacos::ConfigType,
                                std::optional<std::string>) noexcept {
    co_return std::unexpected(nacos::ConfigServiceError{
            .code = nacos::ConfigServiceErrorCode::InvalidArgument,
            .io_error = common::IoErr::NotSupported,
            .message = "local file config provider is read-only",
    });
}

async::Task<std::expected<void, nacos::ConfigServiceError>>
LocalFileConfigService::remove_config(std::string, std::string) noexcept {
    co_return std::unexpected(nacos::ConfigServiceError{
            .code = nacos::ConfigServiceErrorCode::InvalidArgument,
            .io_error = common::IoErr::NotSupported,
            .message = "local file config provider is read-only",
    });
}

std::expected<nacos::Subscription<nacos::ConfigData>, nacos::ConfigServiceError>
LocalFileConfigService::subscribe(std::string_view data_id, std::string_view group,
                                  nacos::Subscription<nacos::ConfigData>::NotifyCallback on_notify, void *ctx) {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ != State::Running) {
        return std::unexpected(nacos::ConfigServiceError{
                .code = nacos::ConfigServiceErrorCode::Shutdown,
                .io_error = common::IoErr::Canceled,
                .message = "local file config service is not running",
        });
    }
    if (data_id.empty() || group.empty() || !on_notify) {
        return std::unexpected(nacos::ConfigServiceError{
                .code = nacos::ConfigServiceErrorCode::InvalidArgument,
                .io_error = common::IoErr::Invalid,
                .message = "subscription requires non-empty data_id, group, and callback",
        });
    }

    auto *node = new SubscriptionNode{
            .service = this,
            .key = LocalFileConfigKey{.data_id = std::string(data_id), .group = std::string(group)},
            .on_notify = on_notify,
            .ctx = ctx,
            .attached = true,
            .closed = false,
    };
    subscribers_[node->key].push_back(node);

    const auto found = documents_.find(node->key);
    const std::shared_ptr<const nacos::ConfigData> initial =
            found != documents_.end() ? found->second : not_found_data_;
    node->on_notify(node->ctx, nacos::SubscriptionResult<nacos::ConfigData>{
                                       .kind = nacos::ResultKind::Success,
                                       .data = initial,
                               });
    return nacos::Subscription<nacos::ConfigData>(node, &close_subscription, &subscription_closed);
}

void LocalFileConfigService::close_subscription(void *raw_node) noexcept {
    std::unique_ptr<SubscriptionNode> node(static_cast<SubscriptionNode *>(raw_node));
    if (!node) {
        return;
    }
    if (node->attached && node->service) {
        FIBER_ASSERT(node->service->loop_->in_loop());
        auto it = node->service->subscribers_.find(node->key);
        if (it != node->service->subscribers_.end()) {
            auto &list = it->second;
            std::erase(list, node.get());
            if (list.empty()) {
                node->service->subscribers_.erase(it);
            }
        }
        node->attached = false;
    }
    node->closed = true;
}

bool LocalFileConfigService::subscription_closed(const void *raw_node) noexcept {
    return static_cast<const SubscriptionNode *>(raw_node)->closed;
}

int LocalFileConfigService::notification_priority(const LocalFileConfigKey &key) const noexcept {
    const LocalFileConfigLayout &layout = source_.layout();
    if (key.data_id == layout.tls_certificates_data_id && key.group == layout.tls_certificates_group) {
        return 0;
    }
    if (!layout.edge_function_script_data_id_prefix.empty() &&
        key.data_id.starts_with(layout.edge_function_script_data_id_prefix) &&
        key.group == layout.edge_functions_group) {
        return 1;
    }
    if (key.data_id == layout.edge_functions_data_id && key.group == layout.edge_functions_group) {
        return 2;
    }
    if ((key.data_id == layout.gray_data_id && key.group == layout.gray_group) ||
        (key.data_id == layout.temp_cluster_data_id && key.group == layout.temp_cluster_group)) {
        return 3;
    }
    if (key.data_id == layout.project_list_data_id && key.group == layout.project_route_group) {
        return 5;
    }
    return 4;
}

void LocalFileConfigService::notify_key(const LocalFileConfigKey &key,
                                        const std::shared_ptr<const nacos::ConfigData> &data) noexcept {
    auto it = subscribers_.find(key);
    if (it == subscribers_.end()) {
        return;
    }
    // Snapshot raw pointers before invoking callbacks because a callback (such
    // as project-list reconciliation) may subscribe or unsubscribe other keys.
    std::vector<SubscriptionNode *> snapshot = it->second;
    for (SubscriptionNode *node: snapshot) {
        if (!node->attached || node->closed) {
            continue;
        }
        node->on_notify(node->ctx, nacos::SubscriptionResult<nacos::ConfigData>{
                                           .kind = nacos::ResultKind::Success,
                                           .data = data,
                                   });
    }
}

std::expected<bool, LocalFileConfigError> LocalFileConfigService::reload() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ != State::Running) {
        return std::unexpected(LocalFileConfigError{
                .io_error = common::IoErr::Canceled,
                .path = source_.directory(),
                .detail = "local file config service is not running",
        });
    }

    auto loaded = source_.load();
    if (!loaded) {
        last_reload_error_ = loaded.error();
        ++failed_reloads_;
        LOG(LOG_CONFIG, WARN) << "local_file_config_reload_failed"
                              << " path=\"" << loaded.error().path << "\""
                              << " detail=\"" << loaded.error().detail << "\"";
        return std::unexpected(std::move(loaded.error()));
    }

    std::map<LocalFileConfigKey, std::shared_ptr<const nacos::ConfigData>> next_documents;
    for (auto &[key, doc]: loaded->documents) {
        next_documents.emplace(key, make_present_data(std::move(doc.md5), std::move(doc.content)));
    }

    std::vector<LocalFileConfigKey> changed_keys;
    for (const auto &[key, next_data]: next_documents) {
        const auto prev = documents_.find(key);
        if (prev == documents_.end() || prev->second->md5 != next_data->md5 ||
            prev->second->content != next_data->content) {
            changed_keys.push_back(key);
        }
    }
    for (const auto &[key, prev_data]: documents_) {
        (void) prev_data;
        if (!next_documents.contains(key)) {
            changed_keys.push_back(key);
        }
    }

    last_reload_error_.reset();
    if (changed_keys.empty()) {
        return false;
    }

    documents_ = std::move(next_documents);
    ++successful_reloads_;

    std::sort(changed_keys.begin(), changed_keys.end(),
              [this](const LocalFileConfigKey &a, const LocalFileConfigKey &b) {
                  const int pa = notification_priority(a);
                  const int pb = notification_priority(b);
                  return pa != pb ? pa < pb : a < b;
              });

    LOG(LOG_CONFIG, INFO) << "local_file_config_reloaded"
                          << " directory=\"" << source_.directory() << "\""
                          << " changed=" << changed_keys.size() << " documents=" << documents_.size();

    for (const LocalFileConfigKey &key: changed_keys) {
        if (state_ != State::Running) {
            break;
        }
        const auto found = documents_.find(key);
        notify_key(key, found != documents_.end() ? found->second : not_found_data_);
    }
    return true;
}

void LocalFileConfigService::trigger_reload() noexcept {
    if (loop_->in_loop()) {
        if (state_ == State::Running) {
            (void) reload();
        }
        return;
    }
    const std::uint64_t next = reload_sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
    reload_publisher_->publish(next);
}

async::DetachedTask LocalFileConfigService::poll_loop() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    auto stopped = stop_signal_.subscribe();
    auto stop_snapshot = stopped.current();
    auto reload_sub = reload_trigger_.subscribe();
    auto reload_snapshot = reload_sub.current();
    while (state_ == State::Running && (!stop_snapshot.value || !*stop_snapshot.value)) {
        if (poll_interval_ > std::chrono::milliseconds::zero()) {
            auto event = co_await async::when_any(
                    [delay = poll_interval_]() { return async::sleep(delay); },
                    [&reload_sub, version = reload_snapshot.version]() { return reload_sub.next(version); },
                    [&stopped, version = stop_snapshot.version]() { return stopped.next(version); });
            if (event.is<2>()) {
                stop_snapshot = std::move(event).get<2>();
                continue;
            }
            if (event.is<1>()) {
                reload_snapshot = std::move(event).get<1>();
            } else {
                (void) std::move(event).get<0>();
            }
        } else {
            auto event = co_await async::when_any(
                    [&reload_sub, version = reload_snapshot.version]() { return reload_sub.next(version); },
                    [&stopped, version = stop_snapshot.version]() { return stopped.next(version); });
            if (event.is<1>()) {
                stop_snapshot = std::move(event).get<1>();
                continue;
            }
            reload_snapshot = std::move(event).get<0>();
        }
        if (state_ != State::Running) {
            break;
        }
        (void) reload();
        stop_snapshot = stopped.current();
    }
    background_tasks_.done();
}

LocalStandaloneNamingService::LocalStandaloneNamingService(event::EventLoop &loop) : loop_(&loop) {
    status_publisher_ = status_.acquire_publisher();
    FIBER_ASSERT(status_publisher_.has_value());
}

LocalStandaloneNamingService::~LocalStandaloneNamingService() noexcept = default;

common::IoResult<void> LocalStandaloneNamingService::start() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    stopped_ = false;
    status_publisher_->publish(nacos::NamingServiceStatus{
            .connection =
                    {
                            .phase = nacos::NacosServicePhase::Ready,
                            .rpc_available = true,
                            .connection_ready_count = 1,
                    },
    });
    return {};
}

async::Task<void> LocalStandaloneNamingService::shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (stopped_) {
        co_return;
    }
    stopped_ = true;
    status_publisher_->publish(nacos::NamingServiceStatus{
            .connection =
                    {
                            .phase = nacos::NacosServicePhase::Stopped,
                            .failure = nacos::NacosServiceFailureCategory::Shutdown,
                    },
    });
    co_return;
}

LocalStandaloneNamingService::StatusSubscriber LocalStandaloneNamingService::subscribe_status() {
    return status_.subscribe();
}

async::Task<std::expected<std::shared_ptr<const nacos::ServiceInfo>, nacos::NamingServiceError>>
LocalStandaloneNamingService::get(std::string, std::string) noexcept {
    co_return std::unexpected(nacos::NamingServiceError{
            .code = nacos::NamingServiceErrorCode::InvalidArgument,
            .io_error = common::IoErr::NotSupported,
            .message = "service discovery requires Nacos; configure static addresses when using local file config",
    });
}

std::expected<nacos::Subscription<nacos::ServiceInfo>, nacos::NamingServiceError>
LocalStandaloneNamingService::subscribe(std::string_view, std::string_view,
                                        nacos::Subscription<nacos::ServiceInfo>::NotifyCallback, void *) {
    return std::unexpected(nacos::NamingServiceError{
            .code = nacos::NamingServiceErrorCode::InvalidArgument,
            .io_error = common::IoErr::NotSupported,
            .message = "service discovery requires Nacos; configure static addresses when using local file config",
    });
}

std::expected<void, nacos::NamingServiceError>
LocalStandaloneNamingService::update_registration(void *, nacos::Instance) noexcept {
    return {};
}

nacos::InstanceRegistration::StatusSubscriber LocalStandaloneNamingService::subscribe_registration_status(void *ctx) {
    return static_cast<RegistrationStateBox *>(ctx)->status.subscribe();
}

void LocalStandaloneNamingService::close_registration(void *ctx) noexcept {
    auto *box = static_cast<RegistrationStateBox *>(ctx);
    if (box && box->publisher) {
        box->publisher->publish(nacos::RegistrationStatus{.state = nacos::RegistrationState::Closed});
    }
}

std::expected<nacos::InstanceRegistration, nacos::NamingServiceError>
LocalStandaloneNamingService::registry(std::string_view, std::string_view, nacos::Instance) {
    FIBER_ASSERT(loop_->in_loop());
    if (stopped_) {
        return std::unexpected(nacos::NamingServiceError{
                .code = nacos::NamingServiceErrorCode::Shutdown,
                .io_error = common::IoErr::Canceled,
                .message = "standalone naming service is stopped",
        });
    }
    auto box = std::make_shared<RegistrationStateBox>();
    box->publisher = box->status.acquire_publisher();
    FIBER_ASSERT(box->publisher.has_value());
    box->publisher->publish(nacos::RegistrationStatus{.state = nacos::RegistrationState::Registered});
    RegistrationStateBox *raw = box.get();
    return nacos::InstanceRegistration(std::move(box), raw, &update_registration, &subscribe_registration_status,
                                       &close_registration);
}

} // namespace fiber::access_server

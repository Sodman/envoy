#pragma once

#include "envoy/config/common/tap/v2alpha/common.pb.h"
#include "envoy/local_info/local_info.h"
#include "envoy/thread_local/thread_local.h"

#include "extensions/common/tap/admin.h"
#include "extensions/common/tap/tap.h"
#include "extensions/common/tap/tds.h"

namespace Envoy {
namespace Extensions {
namespace Common {
namespace Tap {

/**
 * Base class for tap extension configuration. Used by all tap extensions.
 */
class ExtensionConfigBase : public ExtensionConfig, Logger::Loggable<Logger::Id::tap> {
public:
  // Extensions::Common::Tap::ExtensionConfig
  void clearTapConfig() override;
  const absl::string_view adminId() override;
  void newTapConfig(envoy::service::tap::v2alpha::TapConfig&& proto_config,
                    Sink* admin_streamer) override;

protected:
  ExtensionConfigBase(const envoy::config::common::tap::v2alpha::CommonExtensionConfig proto_config,
                      TapConfigFactoryPtr&& config_factory, Server::Admin& admin,
                      Singleton::Manager& singleton_manager, ThreadLocal::SlotAllocator& tls,
                      Event::Dispatcher& main_thread_dispatcher,

                      Init::Manager* init_manager, const std::string& stat_prefix,
                      Stats::Scope& stats, Upstream::ClusterManager& cluster_Manager,
                      const LocalInfo::LocalInfo& local_info,
                      Envoy::Runtime::RandomGenerator& random, Api::Api& api,
                      ProtobufMessage::ValidationVisitor& validation_visitor

  );
  ~ExtensionConfigBase() override;

  // All tap configurations derive from TapConfig for type safety. In order to use a common
  // extension base class (with TLS logic, etc.) we must dynamic cast to the actual tap
  // configuration type that the extension expects (and is created by the configuration factory).
  template <class T> std::shared_ptr<T> currentConfigHelper() const {
    return std::dynamic_pointer_cast<T>(tls_slot_->getTyped<TlsFilterConfig>().config_);
  }

private:
  struct TlsFilterConfig : public ThreadLocal::ThreadLocalObject {
    TapConfigSharedPtr config_;
  };

  const envoy::config::common::tap::v2alpha::CommonExtensionConfig proto_config_;
  TapConfigFactoryPtr config_factory_;
  ThreadLocal::SlotPtr tls_slot_;
  AdminHandlerSharedPtr admin_handler_;

  // manager must be before subscription so it is destructed after.
  std::shared_ptr<TapConfigProviderManager> tap_config_provider_manager_;
  TdsTapConfigSubscriptionHandlePtr subscription_;
};

} // namespace Tap
} // namespace Common
} // namespace Extensions
} // namespace Envoy

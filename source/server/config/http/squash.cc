#include "server/config/http/squash.h"

#include <string>

#include "common/common/logger.h"

#include "common/http/filter/squash_filter.h"

#include "common/config/json_utility.h"
#include "common/config/filter_json.h"
#include "common/protobuf/protobuf.h"
#include "common/protobuf/utility.h"
#include "common/json/config_schemas.h"

#include "envoy/registry/registry.h"

#include "api/filter/http/squash.pb.validate.h"


namespace Envoy {
namespace Server {
namespace Configuration {

HttpFilterFactoryCb
SquashFilterConfig::createFilterFactory(
    const Envoy::Json::Object &json_config, const std::string &,
    FactoryContext &context) {
  envoy::api::v2::filter::http::SquashConfig proto_config;
  Config::FilterJson::translateSquashConfig(json_config, proto_config);

  return createFilter(proto_config, context);
}

HttpFilterFactoryCb
SquashFilterConfig::createFilterFactoryFromProto(
    const Envoy::Protobuf::Message &proto_config, const std::string &,
    FactoryContext &context) {
  return createFilter(
       Envoy::MessageUtil::downcastAndValidate<const
       envoy::api::v2::filter::http::SquashConfig&>(proto_config),
      context);
}

HttpFilterFactoryCb
SquashFilterConfig::createFilter(
    const envoy::api::v2::filter::http::SquashConfig &proto_config,
    FactoryContext &context) {

  Http::SquashFilterConfigSharedPtr config = std::make_shared<Http::SquashFilterConfig>(
      Http::SquashFilterConfig(proto_config, context.clusterManager()));

  return [&context,
          config](Http::FilterChainFactoryCallbacks &callbacks) -> void {
    auto filter = new Http::SquashFilter(config, context.clusterManager());
    callbacks.addStreamDecoderFilter(
        Http::StreamDecoderFilterSharedPtr{filter});
  };
}

/**
 * Static registration for this sample filter. @see RegisterFactory.
 */
static Envoy::Registry::RegisterFactory<
    SquashFilterConfig,
    NamedHttpFilterConfigFactory>
    register_;

} // namespace Configuration
} // namespace Server
} // namespace Envoy

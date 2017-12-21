#pragma once

#include <map>
#include <string>

#include "envoy/common/optional.h"
#include "envoy/http/async_client.h"
#include "envoy/http/filter.h"
#include "envoy/upstream/cluster_manager.h"

#include "common/common/logger.h"
#include "common/protobuf/protobuf.h"

#include "api/filter/http/squash.pb.h"

namespace Envoy {
namespace Http {

class SquashFilterConfig : protected Logger::Loggable<Logger::Id::config> {
public:
  SquashFilterConfig(const envoy::api::v2::filter::http::SquashConfig& proto_config,
                     Upstream::ClusterManager& clusterManager);
  const std::string& cluster_name() { return cluster_name_; }
  const std::string& attachment_json() { return attachment_json_; }
  const std::chrono::milliseconds& attachment_timeout() { return attachment_timeout_; }
  const std::chrono::milliseconds& attachment_poll_period() { return attachment_poll_period_; }
  const std::chrono::milliseconds& request_timeout() { return request_timeout_; }

private:
  static std::string getAttachment(const ProtobufWkt::Struct& attachment_template);
  static void getAttachmentFromValue(ProtobufWkt::Value& curvalue);
  static std::string replaceEnv(const std::string& attachment_template);

  std::string cluster_name_;
  std::string attachment_json_;
  std::chrono::milliseconds attachment_timeout_;
  std::chrono::milliseconds attachment_poll_period_;
  std::chrono::milliseconds request_timeout_;
};

typedef std::shared_ptr<SquashFilterConfig> SquashFilterConfigSharedPtr;

class SquashFilter : public StreamDecoderFilter,
                     protected Logger::Loggable<Logger::Id::filter>,
                     public AsyncClient::Callbacks {
public:
  SquashFilter(SquashFilterConfigSharedPtr config, Upstream::ClusterManager& cm);
  ~SquashFilter();

  // Http::StreamFilterBase
  void onDestroy() override;

  // Http::StreamDecoderFilter
  FilterHeadersStatus decodeHeaders(HeaderMap& headers, bool) override;
  FilterDataStatus decodeData(Buffer::Instance&, bool) override;
  FilterTrailersStatus decodeTrailers(HeaderMap&) override;
  void setDecoderFilterCallbacks(StreamDecoderFilterCallbacks& callbacks) override;

  // Http::AsyncClient::Callbacks
  void onSuccess(MessagePtr&&) override;
  void onFailure(AsyncClient::FailureReason) override;

private:
  enum State {
    INITIAL,
    CREATE_CONFIG,
    CHECK_ATTACHMENT,
  };
  SquashFilterConfigSharedPtr config_;
  Upstream::ClusterManager& cm_;
  StreamDecoderFilterCallbacks* decoder_callbacks_;

  State state_;
  std::string debugConfigPath_;
  Event::TimerPtr delay_timer_;
  Event::TimerPtr attachment_timeout_timer_;
  AsyncClient::Request* in_flight_request_;

  void pollForAttachment();
  void doneSquashing();
  const LowerCaseString& squashHeaderKey();
  const std::string& postAttachmentPath();
  const std::string& severAuthority();
  void retry();
};

} // namespace Http
} // namespace Envoy

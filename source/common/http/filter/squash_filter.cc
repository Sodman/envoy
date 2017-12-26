#include "common/http/filter/squash_filter.h"

#include <regex>
#include <string>

#include "common/common/logger.h"
#include "common/common/utility.h"
#include "common/http/message_impl.h"
#include "common/protobuf/protobuf.h"
#include "common/protobuf/utility.h"

namespace Envoy {
namespace Http {

SquashFilterConfig::SquashFilterConfig(
    const envoy::api::v2::filter::http::Squash& proto_config,
    Upstream::ClusterManager& clusterManager)
    : cluster_name_(proto_config.cluster()),
      attachment_json_(getAttachment(proto_config.attachment_template())),
      attachment_timeout_(PROTOBUF_GET_MS_OR_DEFAULT(proto_config, attachment_timeout, 60000)),
      attachment_poll_period_(
          PROTOBUF_GET_MS_OR_DEFAULT(proto_config, attachment_poll_period, 1000)),
      request_timeout_(PROTOBUF_GET_MS_OR_DEFAULT(proto_config, request_timeout, 1000)) {

  if (!clusterManager.get(cluster_name_)) {
    throw EnvoyException(
        fmt::format("squash filter: unknown cluster '{}' in squash config", cluster_name_));
  }
}

std::string SquashFilterConfig::getAttachment(const ProtobufWkt::Struct& attachment_template) {
  ProtobufWkt::Struct attachment_json(attachment_template);
  getAttachmentFromStruct(attachment_json);
  return MessageUtil::getJsonStringFromMessage(attachment_json);
}

void SquashFilterConfig::getAttachmentFromStruct(ProtobufWkt::Struct& attachment_template) {
  for (auto& value_it : *attachment_template.mutable_fields()) {
    auto& curvalue = value_it.second;
    if (curvalue.kind_case() == ProtobufWkt::Value::kStructValue) {
      getAttachmentFromStruct(*curvalue.mutable_struct_value());
    } else {
      getAttachmentFromValue(curvalue);
    }
  }
}

void SquashFilterConfig::getAttachmentFromValue(ProtobufWkt::Value& curvalue) {
  switch (curvalue.kind_case()) {
  case ProtobufWkt::Value::kStructValue: {
    getAttachmentFromStruct(*curvalue.mutable_struct_value());
    break;
  }
  case ProtobufWkt::Value::kListValue: {
    ProtobufWkt::ListValue& values = *curvalue.mutable_list_value();
    for (int i = 0; i < values.values_size(); i++) {
      getAttachmentFromValue(*values.mutable_values(i));
    }
    break;
  }
  case ProtobufWkt::Value::kStringValue: {
    curvalue.set_string_value(replaceEnv(curvalue.string_value()));
    break;
  }
  case ProtobufWkt::Value::KIND_NOT_SET:
  case ProtobufWkt::Value::kNullValue:
  case ProtobufWkt::Value::kBoolValue:
  case ProtobufWkt::Value::kNumberValue: {
    // nothing here... we only need to transform strings
  }
  }
}

std::string SquashFilterConfig::replaceEnv(const std::string& attachment_template) {
  std::string s;

  const std::regex env_regex("\\{\\{ ([a-zA-Z_]+) \\}\\}");
  auto end_last_match = attachment_template.begin();

  auto callback = [&s, &attachment_template,
                   &end_last_match](const std::match_results<std::string::const_iterator>& match) {
    auto start_match = attachment_template.begin() + match.position(0);

    s.append(end_last_match, start_match);

    std::string envar_name = match[1].str();
    const char* envar_value = std::getenv(envar_name.c_str());
    if (envar_value == nullptr) {
      ENVOY_LOG(warn, "Squash: no environment variable named {}.", envar_name);
    } else {
      s.append(envar_value);
    }
    end_last_match = start_match + match.length(0);
  };

  std::sregex_iterator begin(attachment_template.begin(), attachment_template.end(), env_regex),
      end;
  std::for_each(begin, end, callback);
  s.append(end_last_match, attachment_template.end());

  return s;
}

SquashFilter::SquashFilter(SquashFilterConfigSharedPtr config, Upstream::ClusterManager& cm)
    : config_(config), cm_(cm), decoder_callbacks_(nullptr), state_(SquashFilter::INITIAL),
      debugConfigPath_(), delay_timer_(nullptr), attachment_timeout_timer_(nullptr),
      in_flight_request_(nullptr) {}

SquashFilter::~SquashFilter() {}

void SquashFilter::onDestroy() {
  if (in_flight_request_ != nullptr) {
    in_flight_request_->cancel();
    in_flight_request_ = nullptr;
  }

  if (attachment_timeout_timer_) {
    attachment_timeout_timer_->disableTimer();
    attachment_timeout_timer_.reset();
  }

  if (delay_timer_.get() != nullptr) {
    delay_timer_.reset();
  }
}

FilterHeadersStatus SquashFilter::decodeHeaders(HeaderMap& headers, bool) {

  // check for squash header
  if (!headers.get(squashHeaderKey())) {
    ENVOY_LOG(warn, "Squash: no squash header. ignoring.");
    return FilterHeadersStatus::Continue;
  }

  ENVOY_LOG(info, "Squash:we need to squash something");

  MessagePtr request(new RequestMessageImpl());
  request->headers().insertContentType().value().setReference(
      Headers::get().ContentTypeValues.Json);
  request->headers().insertPath().value().setReference(postAttachmentPath());
  request->headers().insertHost().value().setReference(severAuthority());
  request->headers().insertMethod().value().setReference(Headers::get().MethodValues.Post);
  request->body().reset(new Buffer::OwnedImpl(config_->attachment_json()));

  state_ = CREATE_CONFIG;
  in_flight_request_ = cm_.httpAsyncClientForCluster(config_->cluster_name())
                           .send(std::move(request), *this, config_->request_timeout());

  if (in_flight_request_ == nullptr) {
    state_ = INITIAL;
    return FilterHeadersStatus::Continue;
  }

  attachment_timeout_timer_ =
      decoder_callbacks_->dispatcher().createTimer([this]() -> void { doneSquashing(); });
  attachment_timeout_timer_->enableTimer(config_->attachment_timeout());
  // check if the timer expired inline.
  if (state_ == INITIAL) {
    return FilterHeadersStatus::Continue;
  }

  return FilterHeadersStatus::StopIteration;
}

void SquashFilter::onSuccess(MessagePtr&& m) {
  in_flight_request_ = nullptr;
  Buffer::InstancePtr& data = m->body();
  uint64_t num_slices = data->getRawSlices(nullptr, 0);
  Buffer::RawSlice slices[num_slices];
  data->getRawSlices(slices, num_slices);
  std::string jsonbody;
  for (Buffer::RawSlice& slice : slices) {
    jsonbody += std::string(static_cast<const char*>(slice.mem_), slice.len_);
  }

  switch (state_) {

  case INITIAL: {
    // Should never happen..
    break;
  }
  case CREATE_CONFIG: {
    // get the config object that was created
    if (m->headers().Status()->value() != "201") {
      ENVOY_LOG(info, "Squash: can't create attachment object. status {} - not squashing",
                m->headers().Status()->value().c_str());
      doneSquashing();
    } else {
      state_ = CHECK_ATTACHMENT;

      std::string debugConfigId;
      try {
        Json::ObjectSharedPtr json_config = Json::Factory::loadFromString(jsonbody);
        debugConfigId = json_config->getObject("metadata", true)->getString("name", "");
      } catch (Json::Exception&) {
        debugConfigId = "";
      }

      if (debugConfigId.empty()) {
        doneSquashing();
      } else {
        debugConfigPath_ = "/api/v2/debugattachment/" + debugConfigId;
        pollForAttachment();
      }
    }

    break;
  }
  case CHECK_ATTACHMENT: {

    std::string attachmentstate;
    try {
      Json::ObjectSharedPtr json_config = Json::Factory::loadFromString(jsonbody);
      attachmentstate = json_config->getObject("status", true)->getString("state", "");
    } catch (Json::Exception&) {
      // no state yet.. leave it empty for the retry logic.
    }

    bool attached = attachmentstate == "attached";
    bool error = attachmentstate == "error";
    bool finalstate = attached || error;

    if (finalstate) {
      doneSquashing();
    } else {
      retry();
    }
    break;
  }
  }
}

void SquashFilter::onFailure(AsyncClient::FailureReason) {
  bool cleanupneeded = in_flight_request_ != nullptr;
  in_flight_request_ = nullptr;
  switch (state_) {
  case INITIAL: {
    break;
  }
  case CREATE_CONFIG: {
    // no retries here, as we couldnt create the attachment object.
    if (cleanupneeded) {
      // cleanup not needed if onFailure called inline in async client send.
      // this means that decodeHeaders is down the stack and will return Continue.
      doneSquashing();
    }
    break;
  }
  case CHECK_ATTACHMENT: {
    retry();
    break;
  }
  }
}

void SquashFilter::retry() {

  if (delay_timer_.get() == nullptr) {
    delay_timer_ =
        decoder_callbacks_->dispatcher().createTimer([this]() -> void { pollForAttachment(); });
  }
  delay_timer_->enableTimer(config_->attachment_poll_period());
}

void SquashFilter::pollForAttachment() {
  MessagePtr request(new RequestMessageImpl());
  request->headers().insertMethod().value().setReference(Headers::get().MethodValues.Get);
  request->headers().insertPath().value().setReference(debugConfigPath_);
  request->headers().insertHost().value().setReference(severAuthority());

  in_flight_request_ = cm_.httpAsyncClientForCluster(config_->cluster_name())
                           .send(std::move(request), *this, config_->request_timeout());
  // no need to check in_flight_request_ is null as onFailure will take care of
  // that.
}

FilterDataStatus SquashFilter::decodeData(Buffer::Instance&, bool) {
  if (state_ == INITIAL) {
    return FilterDataStatus::Continue;
  } else {
    return FilterDataStatus::StopIterationAndBuffer;
  }
}

FilterTrailersStatus SquashFilter::decodeTrailers(HeaderMap&) {
  if (state_ == INITIAL) {
    return FilterTrailersStatus::Continue;
  } else {
    return FilterTrailersStatus::StopIteration;
  }
}

void SquashFilter::setDecoderFilterCallbacks(StreamDecoderFilterCallbacks& callbacks) {
  decoder_callbacks_ = &callbacks;
}

const LowerCaseString& SquashFilter::squashHeaderKey() {
  static LowerCaseString* key = new LowerCaseString("x-squash-debug");
  return *key;
}

const std::string& SquashFilter::postAttachmentPath() {
  static std::string* val = new std::string("/api/v2/debugattachment");
  return *val;
}

const std::string& SquashFilter::severAuthority() {
  static std::string* val = new std::string("squash-server");
  return *val;
}

void SquashFilter::doneSquashing() {
  state_ = INITIAL;
  if (delay_timer_) {
    delay_timer_->disableTimer();
    delay_timer_.reset();
  }

  if (attachment_timeout_timer_) {
    attachment_timeout_timer_->disableTimer();
    attachment_timeout_timer_.reset();
  }

  if (in_flight_request_ != nullptr) {
    in_flight_request_->cancel();
    in_flight_request_ = nullptr;
  }

  decoder_callbacks_->continueDecoding();
}

} // namespace Http
} // namespace Envoy

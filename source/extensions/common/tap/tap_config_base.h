#pragma once

#include <fstream>

#include "absl/types/optional.h"
#include "envoy/buffer/buffer.h"
#include "envoy/grpc/async_client.h"
#include "envoy/local_info/local_info.h"
#include "envoy/service/tap/v2alpha/common.pb.h"
#include "envoy/service/tap/v2alpha/tap.pb.h"
#include "envoy/upstream/cluster_manager.h"

#include "extensions/common/tap/tap.h"
#include "extensions/common/tap/tap_matcher.h"
#include "common/grpc/typed_async_client.h"

namespace Envoy {
namespace Extensions {
namespace Common {
namespace Tap {

/**
 * Common utilities for tapping.
 */
class Utility {
public:
  /**
   * Add body data to a tapped body message, taking into account the maximum bytes to buffer.
   * @param output_body supplies the body message to buffer to.
   * @param max_buffered_bytes supplies the maximum bytes to store, if truncation occurs the
   *        truncation flag will be set.
   * @param data supplies the data to buffer.
   * @param buffer_start_offset supplies the offset within data to start buffering.
   * @param buffer_length_to_copy supplies the length of the data to buffer.
   * @return whether the buffered data was truncated or not.
   */
  static bool addBufferToProtoBytes(envoy::data::tap::v2alpha::Body& output_body,
                                    uint32_t max_buffered_bytes, const Buffer::Instance& data,
                                    uint32_t buffer_start_offset, uint32_t buffer_length_to_copy);

  /**
   * Swap body as bytes to body as string if necessary in a trace wrapper.
   */
  static void bodyBytesToString(envoy::data::tap::v2alpha::TraceWrapper& trace,
                                envoy::service::tap::v2alpha::OutputSink::Format sink_format);

  /**
   * Trim a container that contains buffer raw slices so that the slices start at an offset and
   * only contain a specific length. No slices are removed from the container, but their length
   * may be reduced to 0.
   * TODO(mattklein123): This is split out to ease testing and also because we should ultimately
   * move this directly into the buffer API. I would rather wait until the new buffer code merges
   * before we do that.
   */
  template <typename T> static void trimSlices(T& slices, uint32_t start_offset, uint32_t length) {
    for (auto& slice : slices) {
      const uint32_t start_offset_trim = std::min<uint32_t>(start_offset, slice.len_);
      slice.len_ -= start_offset_trim;
      start_offset -= start_offset_trim;
      slice.mem_ = static_cast<char*>(slice.mem_) + start_offset_trim;

      const uint32_t final_length = std::min<uint32_t>(length, slice.len_);
      slice.len_ = final_length;
      length -= final_length;
    }
  }
};

/**
 * Base class for all tap configurations.
 * TODO(mattklein123): This class will handle common functionality such as rate limiting, etc.
 */
class TapConfigBaseImpl : public virtual TapConfig {
public:
  // A wrapper for a per tap sink handle and trace submission. If in the future we support
  // multiple sinks we can easily do it here.
  class PerTapSinkHandleManagerImpl : public PerTapSinkHandleManager {
  public:
    PerTapSinkHandleManagerImpl(TapConfigBaseImpl& parent, uint64_t trace_id)
        : parent_(parent), handle_(parent.sink_to_use_->createPerTapSinkHandle(trace_id)) {}

    // PerTapSinkHandleManager
    void submitTrace(TraceWrapperPtr&& trace) override;

  private:
    TapConfigBaseImpl& parent_;
    PerTapSinkHandlePtr handle_;
  };

  // TapConfig
  PerTapSinkHandleManagerPtr createPerTapSinkHandleManager(uint64_t trace_id) override {
    if (enabled(trace_id)) {
      return std::make_unique<PerTapSinkHandleManagerImpl>(*this, trace_id);
    }
    return {};
  }
  uint32_t maxBufferedRxBytes() const override { return max_buffered_rx_bytes_; }
  uint32_t maxBufferedTxBytes() const override { return max_buffered_tx_bytes_; }
  Matcher::MatchStatusVector createMatchStatusVector() const override {
    return Matcher::MatchStatusVector(matchers_.size());
  }
  const Matcher& rootMatcher() const override;
  bool streaming() const override { return streaming_; }

protected:
  TapConfigBaseImpl(envoy::service::tap::v2alpha::TapConfig&& proto_config,
                    Runtime::Loader& loader,
                    Common::Tap::Sink* admin_streamer,
                    Upstream::ClusterManager& cluster_manager, Stats::Scope& scope,
                    const LocalInfo::LocalInfo& local_info);

private:
  // This is the default setting for both RX/TX max buffered bytes. (This means that per tap, the
  // maximum amount that can be buffered is 2x this value).
  static constexpr uint32_t DefaultMaxBufferedBytes = 1024;

  bool enabled(uint64_t random_value) const;

  Runtime::Loader& loader_;

  const uint32_t max_buffered_rx_bytes_;
  const uint32_t max_buffered_tx_bytes_;
  const bool streaming_;
  Sink* sink_to_use_;
  SinkPtr sink_;
  absl::optional<envoy::api::v2::core::RuntimeFractionalPercent> filter_enabled_;
  envoy::service::tap::v2alpha::OutputSink::Format sink_format_;
  std::vector<MatcherPtr> matchers_;
};

/**
 * A tap sink that writes each tap trace to a discrete output file.
 */
class FilePerTapSink : public Sink {
public:
  FilePerTapSink(const envoy::service::tap::v2alpha::FilePerTapSink& config) : config_(config) {}

  // Sink
  PerTapSinkHandlePtr createPerTapSinkHandle(uint64_t trace_id) override {
    return std::make_unique<FilePerTapSinkHandle>(*this, trace_id);
  }

private:
  struct FilePerTapSinkHandle : public PerTapSinkHandle {
    FilePerTapSinkHandle(FilePerTapSink& parent, uint64_t trace_id)
        : parent_(parent), trace_id_(trace_id) {}

    // PerTapSinkHandle
    void submitTrace(TraceWrapperPtr&& trace,
                     envoy::service::tap::v2alpha::OutputSink::Format format) override;

    FilePerTapSink& parent_;
    const uint64_t trace_id_;
    std::ofstream output_file_;
  };

  const envoy::service::tap::v2alpha::FilePerTapSink config_;
};

using GrpcTapSinkAsyncCallbacks = Grpc::AsyncStreamCallbacks<envoy::service::tap::v2alpha::StreamTapsResponse>;

/**
 * A tap sink that writes each tap trace to a discrete output file.
 *
add stat of inflight request
success
and failure

 */
class GrpcTapSink
    : public Sink,
      public GrpcTapSinkAsyncCallbacks {
public:
  GrpcTapSink(const envoy::api::v2::core::GrpcService& grpc_service,
              std::string tap_id,
              Upstream::ClusterManager& cluster_manager, Stats::Scope& scope,
              const LocalInfo::LocalInfo& local_info)
      : local_info_(local_info), tap_id_(std::move(tap_id)), 
  service_method_(*Protobuf::DescriptorPool::generated_pool()->FindMethodByName(
          "envoy.service.tap.v2alpha.TapService.StreamTaps")) {
    const auto async_client_factory =
        cluster_manager.grpcAsyncClientManager().factoryForGrpcService(grpc_service, scope, true);
    client_ = async_client_factory->create();
  }

  virtual ~GrpcTapSink() {
    if (stream_ != nullptr) {
      stream_->resetStream();
      stream_ = nullptr;
    }
  }

  // Sink
  PerTapSinkHandlePtr createPerTapSinkHandle(uint64_t trace_id) override {

    return std::make_unique<GrpcPerTapSinkHandle>(*this, trace_id);
  }

  // Grpc::AsyncStreamCallbacks

  void onCreateInitialMetadata(Http::HeaderMap&) override {}
  void onReceiveInitialMetadata(Http::HeaderMapPtr&&) override {}
  void onReceiveMessage(std::unique_ptr<envoy::service::tap::v2alpha::StreamTapsResponse>&&) override {}
  void onReceiveTrailingMetadata(Http::HeaderMapPtr&&) override {}

  void onRemoteClose(Grpc::Status::GrpcStatus , const std::string&) override  { stream_ = nullptr; };

private:
  struct GrpcPerTapSinkHandle : public PerTapSinkHandle {
    GrpcPerTapSinkHandle(GrpcTapSink& parent, uint64_t trace_id)
        : parent_(parent), trace_id_(trace_id) {}

    // PerTapSinkHandle
    void submitTrace(TraceWrapperPtr&& trace,
                     envoy::service::tap::v2alpha::OutputSink::Format format) override;

    GrpcTapSink& parent_;
    const uint64_t trace_id_;
    std::ofstream output_file_;
  };
  const LocalInfo::LocalInfo& local_info_;
  std::string tap_id_;

// service_method_, *this, Http::AsyncClient::StreamOptions());
  const Protobuf::MethodDescriptor& service_method_;
  Grpc::AsyncClient<envoy::service::tap::v2alpha::StreamTapsRequest,
                    envoy::service::tap::v2alpha::StreamTapsResponse>
      client_;
  Grpc::AsyncStream<envoy::service::tap::v2alpha::StreamTapsRequest>
      stream_{};
};

} // namespace Tap
} // namespace Common
} // namespace Extensions
} // namespace Envoy

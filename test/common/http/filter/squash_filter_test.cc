
#include <chrono>

#include "squash_filter.h"
#include "squash_filter_config.h"

#include "test/mocks/upstream/mocks.h"
#include "test/mocks/server/mocks.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::Invoke;
using testing::NiceMock;
using testing::_;

namespace Solo {
namespace Squash {

namespace {
SquashFilterConfig constructSquashFilterConfigFromJson(
    const Envoy::Json::Object &json, Envoy::Server::Configuration::FactoryContext &context) {
  solo::squash::pb::SquashConfig proto_config;
  Configuration::SquashFilterConfigFactory::translateSquashFilter(json,
                                                                  proto_config);
  return SquashFilterConfig(proto_config, context);
}
} // namespace

TEST(SoloFilterConfigTest, NoCluster) {
  std::string json = R"EOF(
    {
      "squash_cluster" : "fake_cluster"
    }
    )EOF";

  Envoy::Json::ObjectSharedPtr config = Envoy::Json::Factory::loadFromString(json);
  NiceMock<Envoy::Server::Configuration::MockFactoryContext> factory_context;

  EXPECT_CALL(factory_context.cluster_manager_, get("fake_cluster")).WillOnce(Return(nullptr));

  EXPECT_THROW(constructSquashFilterConfigFromJson(*config, factory_context),
               Envoy::EnvoyException);
}

TEST(SoloFilterConfigTest, ParsesEnvironment) {
  std::string json = R"EOF(
    {
      "squash_cluster" : "squash",
      "attachment_template" : "{\"a\":\"{{ MISSING_ENV }}\"}"
    }
    )EOF";
  std::string expected_json = "{\"a\":\"\"}";

  Envoy::Json::ObjectSharedPtr json_config = Envoy::Json::Factory::loadFromString(json);
  NiceMock<Envoy::Server::Configuration::MockFactoryContext> factory_context;
  auto config = constructSquashFilterConfigFromJson(*json_config, factory_context);
  EXPECT_EQ(expected_json, config.attachment_json());
}


TEST(SoloFilterConfigTest, ParsesAndEscapesEnvironment) {
  ::setenv("ESCAPE_ENV", "\"", 1);

  std::string json = R"EOF(
    {
      "squash_cluster" : "squash",
      "attachment_template" : "{\"a\" : \"{{ ESCAPE_ENV }}\"}"
    }
    )EOF";

  std::string expected_json = "{\"a\" : \"\\\"\"}";

  Envoy::Json::ObjectSharedPtr json_config = Envoy::Json::Factory::loadFromString(json);
  NiceMock<Envoy::Server::Configuration::MockFactoryContext> factory_context;
  auto config = constructSquashFilterConfigFromJson(*json_config, factory_context);
  EXPECT_EQ(expected_json, config.attachment_json());
}

TEST(SoloFilterConfigTest, ParsesDefaultEnvironment) {
  ::setenv("POD_NAME", "pod1", 1);
  ::setenv("POD_NAMESPACE", "namespace1", 1);

  std::string json = R"EOF(
    {
      "squash_cluster" : "squash"
    }
    )EOF";

  Envoy::Json::ObjectSharedPtr json_config = Envoy::Json::Factory::loadFromString(json);
  NiceMock<Envoy::Server::Configuration::MockFactoryContext> factory_context;
  auto config = constructSquashFilterConfigFromJson(*json_config, factory_context);
  
  auto attachment_json = config.attachment_json();
  Envoy::Json::ObjectSharedPtr attachment_json_obj = Envoy::Json::Factory::
      loadFromString(attachment_json)->getObject("spec")->getObject("attachment");

  EXPECT_EQ("pod1", attachment_json_obj->getString("pod"));
  EXPECT_EQ("namespace1", attachment_json_obj->getString("namespace"));
}

class SquashFilterTest : public testing::Test {
public:
  SquashFilterTest() {
  }

protected:
  void SetUp() override {
  }

  NiceMock<Envoy::Http::MockStreamDecoderFilterCallbacks> filter_callbacks_;
  NiceMock<Envoy::Server::Configuration::MockFactoryContext> factory_context_;
  NiceMock<Envoy::Event::MockTimer>* attachment_timeout_timer_{};
  NiceMock<Envoy::Upstream::MockClusterManager> cm_;  
};

TEST_F(SquashFilterTest, DecodeHeaderContinuesOnClientFail) {

  solo::squash::pb::SquashConfig p;
  p.set_squash_cluster("squash");
  SquashFilterConfigSharedPtr config(new SquashFilterConfig(p, factory_context_));
  EXPECT_CALL(cm_, httpAsyncClientForCluster("squash"))
      .WillOnce(ReturnRef(cm_.async_client_));

  EXPECT_CALL(cm_.async_client_, send_(_, _, _))
      .WillOnce(Invoke([&](Envoy::Http::MessagePtr &,
                           Envoy::Http::AsyncClient::Callbacks &callbacks,
                           const Envoy::Optional<std::chrono::milliseconds> &)
                           -> Envoy::Http::AsyncClient::Request * {
        callbacks.onFailure(Envoy::Http::AsyncClient::FailureReason::Reset);
        return nullptr;
      }));

  SquashFilter filter(config, cm_);

  Envoy::Http::TestHeaderMapImpl headers{{":method", "GET"},
                                         {":authority", "www.solo.io"},
                                         {"x-squash-debug", "true"},
                                         {":path", "/getsomething"}};

  EXPECT_EQ(Envoy::Http::FilterHeadersStatus::Continue,
            filter.decodeHeaders(headers, false));
  EXPECT_EQ(Envoy::Http::FilterTrailersStatus::Continue,
            filter.decodeTrailers(headers));
}

TEST_F(SquashFilterTest, Timeout) {
  attachment_timeout_timer_ = new NiceMock<Envoy::Event::MockTimer>(&filter_callbacks_.dispatcher_);

  solo::squash::pb::SquashConfig p;
  p.set_squash_cluster("squash");
  SquashFilterConfigSharedPtr config(new SquashFilterConfig(p, factory_context_));

  EXPECT_CALL(cm_, httpAsyncClientForCluster("squash"))
      .WillOnce(ReturnRef(cm_.async_client_));

  Envoy::Http::AsyncClient::Callbacks *callbacks;
  Envoy::Http::MockAsyncClientRequest request(&cm_.async_client_);

  EXPECT_CALL(cm_.async_client_, send_(_, _, _))
      .WillOnce(Invoke([&](Envoy::Http::MessagePtr &,
                           Envoy::Http::AsyncClient::Callbacks &cb,
                           const Envoy::Optional<std::chrono::milliseconds> &)
                           -> Envoy::Http::AsyncClient::Request * {
        callbacks = &cb;
        return &request;
      }));

  SquashFilter filter(config, cm_);
  filter.setDecoderFilterCallbacks(filter_callbacks_);

  Envoy::Http::TestHeaderMapImpl headers{{":method", "GET"},
                                         {":authority", "www.solo.io"},
                                         {"x-squash-debug", "true"},
                                         {":path", "/getsomething"}};
  EXPECT_EQ(Envoy::Http::FilterHeadersStatus::StopIteration,
            filter.decodeHeaders(headers, false));
  
  // invoke timeout
  Envoy::Buffer::OwnedImpl buffer("nothing here");

  EXPECT_EQ(Envoy::Http::FilterDataStatus::StopIterationAndBuffer,
            filter.decodeData(buffer, false));

  EXPECT_CALL(request, cancel());
  EXPECT_CALL(filter_callbacks_, continueDecoding());

  attachment_timeout_timer_->callback_();

  EXPECT_EQ(Envoy::Http::FilterDataStatus::Continue,
            filter.decodeData(buffer, false));
}

} // namespace Squash
} // namespace Solo
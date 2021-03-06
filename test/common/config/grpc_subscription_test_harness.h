#pragma once

#include "common/common/hash.h"
#include "common/config/grpc_subscription_impl.h"
#include "common/config/resources.h"

#include "test/common/config/subscription_test_harness.h"
#include "test/mocks/config/mocks.h"
#include "test/mocks/event/mocks.h"
#include "test/mocks/grpc/mocks.h"
#include "test/mocks/upstream/mocks.h"
#include "test/test_common/utility.h"

#include "api/eds.pb.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::Invoke;
using testing::Mock;
using testing::NiceMock;
using testing::Return;
using testing::_;

namespace Envoy {
namespace Config {

typedef Grpc::MockAsyncClient<envoy::api::v2::DiscoveryRequest, envoy::api::v2::DiscoveryResponse>
    SubscriptionMockAsyncClient;
typedef GrpcSubscriptionImpl<envoy::api::v2::ClusterLoadAssignment> GrpcEdsSubscriptionImpl;

class GrpcSubscriptionTestHarness : public SubscriptionTestHarness {
public:
  GrpcSubscriptionTestHarness()
      : method_descriptor_(Protobuf::DescriptorPool::generated_pool()->FindMethodByName(
            "envoy.api.v2.EndpointDiscoveryService.StreamEndpoints")),
        async_client_(new SubscriptionMockAsyncClient()), timer_(new Event::MockTimer()) {
    node_.set_id("fo0");
    EXPECT_CALL(dispatcher_, createTimer_(_)).WillOnce(Invoke([this](Event::TimerCb timer_cb) {
      timer_cb_ = timer_cb;
      return timer_;
    }));
    subscription_.reset(new GrpcEdsSubscriptionImpl(
        node_, std::unique_ptr<SubscriptionMockAsyncClient>(async_client_), dispatcher_,
        *method_descriptor_, stats_));
  }

  ~GrpcSubscriptionTestHarness() { EXPECT_CALL(async_stream_, sendMessage(_, false)); }

  void expectSendMessage(const std::vector<std::string>& cluster_names,
                         const std::string& version) override {
    envoy::api::v2::DiscoveryRequest expected_request;
    expected_request.mutable_node()->CopyFrom(node_);
    for (const auto& cluster : cluster_names) {
      expected_request.add_resource_names(cluster);
    }
    if (!version.empty()) {
      expected_request.set_version_info(version);
    }
    expected_request.set_response_nonce(last_response_nonce_);
    expected_request.set_type_url(Config::TypeUrl::get().ClusterLoadAssignment);
    EXPECT_CALL(async_stream_, sendMessage(ProtoEq(expected_request), false));
  }

  void startSubscription(const std::vector<std::string>& cluster_names) override {
    EXPECT_CALL(*async_client_, start(_, _)).WillOnce(Return(&async_stream_));
    last_cluster_names_ = cluster_names;
    expectSendMessage(last_cluster_names_, "");
    subscription_->start(cluster_names, callbacks_);
    // These are just there to add coverage to the null implementations of these
    // callbacks.
    Http::HeaderMapPtr response_headers{new Http::TestHeaderMapImpl{}};
    subscription_->grpcMux().onReceiveInitialMetadata(std::move(response_headers));
    Http::TestHeaderMapImpl request_headers;
    subscription_->grpcMux().onCreateInitialMetadata(request_headers);
  }

  void deliverConfigUpdate(const std::vector<std::string> cluster_names, const std::string& version,
                           bool accept) override {
    std::unique_ptr<envoy::api::v2::DiscoveryResponse> response(
        new envoy::api::v2::DiscoveryResponse());
    response->set_version_info(version);
    last_response_nonce_ = std::to_string(HashUtil::xxHash64(version));
    response->set_nonce(last_response_nonce_);
    response->set_type_url(Config::TypeUrl::get().ClusterLoadAssignment);
    Protobuf::RepeatedPtrField<envoy::api::v2::ClusterLoadAssignment> typed_resources;
    for (const auto& cluster : cluster_names) {
      if (std::find(last_cluster_names_.begin(), last_cluster_names_.end(), cluster) !=
          last_cluster_names_.end()) {
        envoy::api::v2::ClusterLoadAssignment* load_assignment = typed_resources.Add();
        load_assignment->set_cluster_name(cluster);
        response->add_resources()->PackFrom(*load_assignment);
      }
    }
    EXPECT_CALL(callbacks_, onConfigUpdate(RepeatedProtoEq(typed_resources)))
        .WillOnce(ThrowOnRejectedConfig(accept));
    if (accept) {
      expectSendMessage(last_cluster_names_, version);
      version_ = version;
    } else {
      EXPECT_CALL(callbacks_, onConfigUpdateFailed(_));
      expectSendMessage(last_cluster_names_, version_);
    }
    subscription_->grpcMux().onReceiveMessage(std::move(response));
    EXPECT_EQ(version_, subscription_->versionInfo());
    Mock::VerifyAndClearExpectations(&async_stream_);
  }

  void updateResources(const std::vector<std::string>& cluster_names) override {
    std::vector<std::string> cluster_superset = cluster_names;
    cluster_superset.insert(cluster_superset.end(), last_cluster_names_.begin(),
                            last_cluster_names_.end());
    expectSendMessage(cluster_superset, version_);
    expectSendMessage(cluster_names, version_);
    subscription_->updateResources(cluster_names);
    last_cluster_names_ = cluster_names;
  }

  std::string version_;
  const Protobuf::MethodDescriptor* method_descriptor_;
  SubscriptionMockAsyncClient* async_client_;
  NiceMock<Upstream::MockClusterManager> cm_;
  Event::MockDispatcher dispatcher_;
  Event::MockTimer* timer_;
  Event::TimerCb timer_cb_;
  envoy::api::v2::Node node_;
  Config::MockSubscriptionCallbacks<envoy::api::v2::ClusterLoadAssignment> callbacks_;
  Grpc::MockAsyncStream<envoy::api::v2::DiscoveryRequest> async_stream_;
  std::unique_ptr<GrpcEdsSubscriptionImpl> subscription_;
  std::string last_response_nonce_;
  std::vector<std::string> last_cluster_names_;
};

// TODO(danielhochman): test with RDS and ensure version_info is same as what API returned

} // namespace Config
} // namespace Envoy

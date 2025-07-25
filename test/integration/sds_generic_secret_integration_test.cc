#include <string>

#include "envoy/config/bootstrap/v3/bootstrap.pb.h"
#include "envoy/config/core/v3/grpc_service.pb.h"
#include "envoy/http/filter.h"
#include "envoy/secret/secret_provider.h"
#include "envoy/service/discovery/v3/discovery.pb.h"

#include "source/common/config/datasource.h"
#include "source/common/grpc/common.h"

#include "test/config/v2_link_hacks.h"
#include "test/extensions/filters/http/common/empty_http_filter_config.h"
#include "test/integration/http_integration.h"
#include "test/integration/utility.h"
#include "test/test_common/registry.h"
#include "test/test_common/utility.h"

namespace Envoy {

// The filter fetches a generic secret from secret manager and attaches it to a header for
// validation.
class SdsGenericSecretTestFilter : public Http::StreamDecoderFilter {
public:
  SdsGenericSecretTestFilter(Api::Api& api,
                             Secret::GenericSecretConfigProviderSharedPtr config_provider)
      : api_(api), config_provider_(config_provider) {}

  // Http::StreamFilterBase
  void onDestroy() override {};

  // Http::StreamDecoderFilter
  Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap& headers, bool) override {
    if (config_provider_->secret()->has_secret()) {
      headers.addCopy(
          Http::LowerCaseString("secret"),
          Config::DataSource::read(config_provider_->secret()->secret(), true, api_).value());
    } else {
      for (const auto& [secret_name, datasource] : config_provider_->secret()->secrets()) {
        headers.addCopy(Http::LowerCaseString(secret_name),
                        Config::DataSource::read(datasource, true, api_).value());
      }
    }
    return Http::FilterHeadersStatus::Continue;
  }

  Http::FilterDataStatus decodeData(Buffer::Instance&, bool) override {
    return Http::FilterDataStatus::Continue;
  }

  Http::FilterTrailersStatus decodeTrailers(Http::RequestTrailerMap&) override {
    return Http::FilterTrailersStatus::Continue;
  }

  void setDecoderFilterCallbacks(Http::StreamDecoderFilterCallbacks& callbacks) override {
    decoder_callbacks_ = &callbacks;
  }

private:
  Api::Api& api_;
  Secret::GenericSecretConfigProviderSharedPtr config_provider_;
  Http::StreamDecoderFilterCallbacks* decoder_callbacks_;
};

class SdsGenericSecretTestFilterConfig
    : public Extensions::HttpFilters::Common::EmptyHttpFilterConfig {
public:
  SdsGenericSecretTestFilterConfig()
      : Extensions::HttpFilters::Common::EmptyHttpFilterConfig("sds-generic-secret-test") {}

  absl::StatusOr<Http::FilterFactoryCb>
  createFilter(const std::string&,
               Server::Configuration::FactoryContext& factory_context) override {
    auto secret_provider =
        factory_context.serverFactoryContext().secretManager().findOrCreateGenericSecretProvider(
            config_source_, "encryption_key", factory_context.serverFactoryContext(),
            factory_context.initManager());
    return
        [&factory_context, secret_provider](Http::FilterChainFactoryCallbacks& callbacks) -> void {
          callbacks.addStreamDecoderFilter(std::make_shared<::Envoy::SdsGenericSecretTestFilter>(
              factory_context.serverFactoryContext().api(), secret_provider));
        };
  }

protected:
  envoy::config::core::v3::ConfigSource config_source_;
};

class SdsGenericSecretApiConfigSourceTestFilterConfig : public SdsGenericSecretTestFilterConfig {
public:
  SdsGenericSecretApiConfigSourceTestFilterConfig() {
    config_source_.set_resource_api_version(envoy::config::core::v3::ApiVersion::V3);
    auto* api_config_source = config_source_.mutable_api_config_source();
    api_config_source->set_api_type(envoy::config::core::v3::ApiConfigSource::GRPC);
    api_config_source->set_transport_api_version(envoy::config::core::v3::V3);
    auto* grpc_service = api_config_source->add_grpc_services();
    grpc_service->mutable_envoy_grpc()->set_cluster_name("sds_cluster");
  }
};

class SdsGenericSecretApiConfigSourceIntegrationTest : public Grpc::GrpcClientIntegrationParamTest,
                                                       public HttpIntegrationTest {
public:
  SdsGenericSecretApiConfigSourceIntegrationTest()
      : HttpIntegrationTest(Http::CodecType::HTTP1, ipVersion()), registration_(factory_) {}

  void initialize() override {
    config_helper_.addConfigModifier([](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
      auto* sds_cluster = bootstrap.mutable_static_resources()->add_clusters();
      sds_cluster->MergeFrom(bootstrap.static_resources().clusters()[0]);
      sds_cluster->set_name("sds_cluster");
      ConfigHelper::setHttp2(*sds_cluster);
    });

    config_helper_.prependFilter("{ name: sds-generic-secret-test }");

    create_xds_upstream_ = true;
    HttpIntegrationTest::initialize();
  }

  void TearDown() override { cleanUpXdsConnection(); }

  void createSdsStream() {
    createXdsConnection();
    AssertionResult result = xds_connection_->waitForNewStream(*dispatcher_, xds_stream_);
    RELEASE_ASSERT(result, result.message());
    xds_stream_->startGrpcStream();
  }

  void sendSecret() {
    envoy::extensions::transport_sockets::tls::v3::Secret secret;
    secret.set_name("encryption_key");
    auto* generic_secret = secret.mutable_generic_secret();
    generic_secret->mutable_secret()->set_inline_string("DUMMY_AES_128_KEY");
    envoy::service::discovery::v3::DiscoveryResponse discovery_response;
    discovery_response.set_version_info("0");
    discovery_response.set_type_url(Config::TestTypeUrl::get().Secret);
    discovery_response.add_resources()->PackFrom(secret);
    xds_stream_->sendGrpcMessage(discovery_response);
  }

  SdsGenericSecretApiConfigSourceTestFilterConfig factory_;
  Registry::InjectFactory<Server::Configuration::NamedHttpFilterConfigFactory> registration_;
};

INSTANTIATE_TEST_SUITE_P(IpVersions, SdsGenericSecretApiConfigSourceIntegrationTest,
                         GRPC_CLIENT_INTEGRATION_PARAMS);

// A test that an SDS generic secret can be successfully fetched by a filter.
TEST_P(SdsGenericSecretApiConfigSourceIntegrationTest, FilterFetchSuccess) {
  on_server_init_function_ = [this]() {
    createSdsStream();
    sendSecret();
  };
  initialize();

  codec_client_ = makeHttpConnection((lookupPort("http")));
  Http::TestRequestHeaderMapImpl request_headers{
      {":method", "GET"}, {":path", "/"}, {":scheme", "http"}, {":authority", "host"}};
  sendRequestAndWaitForResponse(request_headers, 0, default_response_headers_, 0);

  EXPECT_TRUE(upstream_request_->complete());
  EXPECT_EQ(0U, upstream_request_->bodyLength());
  EXPECT_EQ("DUMMY_AES_128_KEY", upstream_request_->headers()
                                     .get(Http::LowerCaseString("secret"))[0]
                                     ->value()
                                     .getStringView());
}

class SdsGenericSecretPathConfigSourceTestFilterConfig : public SdsGenericSecretTestFilterConfig {
public:
  SdsGenericSecretPathConfigSourceTestFilterConfig() {
    TestEnvironment::writeStringToFileForTest("generic_secret.txt", "DUMMY_AES_128_KEY");

    auto secret = TestEnvironment::substitute(R"EOF(
resources:
- "@type": "type.googleapis.com/envoy.extensions.transport_sockets.tls.v3.Secret"
  name: encryption_key
  generic_secret:
    secret:
      filename: "{{ test_tmpdir }}/generic_secret.txt"
)EOF");
    TestEnvironment::writeStringToFileForTest("generic_secret.yaml", secret);
    auto secret_path = TestEnvironment::temporaryPath("generic_secret.yaml");
    config_source_.mutable_path_config_source()->set_path(secret_path);
  }
};

class SdsGenericSecretPathConfigSourceIntegrationTest : public Grpc::GrpcClientIntegrationParamTest,
                                                        public HttpIntegrationTest {
public:
  SdsGenericSecretPathConfigSourceIntegrationTest()
      : HttpIntegrationTest(Http::CodecType::HTTP1, ipVersion()), registration_(factory_) {}

  void initialize() override {
    config_helper_.prependFilter("{ name: sds-generic-secret-test }");
    HttpIntegrationTest::initialize();
  }

  SdsGenericSecretPathConfigSourceTestFilterConfig factory_;
  Registry::InjectFactory<Server::Configuration::NamedHttpFilterConfigFactory> registration_;
};

INSTANTIATE_TEST_SUITE_P(IpVersions, SdsGenericSecretPathConfigSourceIntegrationTest,
                         GRPC_CLIENT_INTEGRATION_PARAMS);

// This test verifies that a file specified in a SDS generic secret is watched and the secret
// provider is up-to-date.
TEST_P(SdsGenericSecretPathConfigSourceIntegrationTest, GenericSecretFileUpdate) {
  initialize();

  codec_client_ = makeHttpConnection((lookupPort("http")));
  Http::TestRequestHeaderMapImpl request_headers{
      {":method", "GET"}, {":path", "/"}, {":scheme", "http"}, {":authority", "host"}};
  sendRequestAndWaitForResponse(request_headers, 0, default_response_headers_, 0);

  EXPECT_TRUE(upstream_request_->complete());
  EXPECT_EQ(0U, upstream_request_->bodyLength());
  EXPECT_EQ("DUMMY_AES_128_KEY", upstream_request_->headers()
                                     .get(Http::LowerCaseString("secret"))[0]
                                     ->value()
                                     .getStringView());

  // Update contents of the file specified in the secret.
  TestEnvironment::writeStringToFileForTest("generic_secret.txt", "dummy_aes_128_key");
  sendRequestAndWaitForResponse(request_headers, 0, default_response_headers_, 0);

  EXPECT_TRUE(upstream_request_->complete());
  EXPECT_EQ(0U, upstream_request_->bodyLength());
  EXPECT_EQ("dummy_aes_128_key", upstream_request_->headers()
                                     .get(Http::LowerCaseString("secret"))[0]
                                     ->value()
                                     .getStringView());
}

class SdsGenericSecretsPathConfigSourceTestFilterConfig : public SdsGenericSecretTestFilterConfig {
public:
  SdsGenericSecretsPathConfigSourceTestFilterConfig() {
    TestEnvironment::writeStringToFileForTest("encryption_key.txt", "DUMMY_AES_128_KEY");
    TestEnvironment::writeStringToFileForTest("credential.txt", "PASSWORD");
    auto secret = TestEnvironment::substitute(R"EOF(
resources:
- "@type": "type.googleapis.com/envoy.extensions.transport_sockets.tls.v3.Secret"
  name: encryption_key
  generic_secret:
    secrets:
      encryption_key:
        filename: "{{ test_tmpdir }}/encryption_key.txt"
      credential:
        filename: "{{ test_tmpdir }}/credential.txt"
)EOF");
    TestEnvironment::writeStringToFileForTest("generic_secret.yaml", secret);
    auto secret_path = TestEnvironment::temporaryPath("generic_secret.yaml");
    config_source_.mutable_path_config_source()->set_path(secret_path);
  }
};

class SdsGenericSecretsPathConfigSourceIntegrationTest
    : public Grpc::GrpcClientIntegrationParamTest,
      public HttpIntegrationTest {
public:
  SdsGenericSecretsPathConfigSourceIntegrationTest()
      : HttpIntegrationTest(Http::CodecType::HTTP1, ipVersion()), registration_(factory_) {}

  void initialize() override {
    config_helper_.prependFilter("{ name: sds-generic-secret-test }");
    HttpIntegrationTest::initialize();
  }

  SdsGenericSecretsPathConfigSourceTestFilterConfig factory_;
  Registry::InjectFactory<Server::Configuration::NamedHttpFilterConfigFactory> registration_;
};

INSTANTIATE_TEST_SUITE_P(IpVersions, SdsGenericSecretsPathConfigSourceIntegrationTest,
                         GRPC_CLIENT_INTEGRATION_PARAMS);

// This test verifies that multiple files specified in a SDS generic secret are watched and the
// secret provider is up-to-date.
TEST_P(SdsGenericSecretsPathConfigSourceIntegrationTest, GenericSecretFileUpdate) {
  initialize();

  codec_client_ = makeHttpConnection((lookupPort("http")));
  Http::TestRequestHeaderMapImpl request_headers{
      {":method", "GET"}, {":path", "/"}, {":scheme", "http"}, {":authority", "host"}};
  sendRequestAndWaitForResponse(request_headers, 0, default_response_headers_, 0);

  EXPECT_TRUE(upstream_request_->complete());
  EXPECT_EQ(0U, upstream_request_->bodyLength());
  EXPECT_EQ("DUMMY_AES_128_KEY", upstream_request_->headers()
                                     .get(Http::LowerCaseString("encryption_key"))[0]
                                     ->value()
                                     .getStringView());
  EXPECT_EQ("PASSWORD", upstream_request_->headers()
                            .get(Http::LowerCaseString("credential"))[0]
                            ->value()
                            .getStringView());

  // Update contents of all files specified in the secret.
  TestEnvironment::writeStringToFileForTest("encryption_key.txt", "dummy_aes_128_key");
  TestEnvironment::writeStringToFileForTest("credential.txt", "password");
  sendRequestAndWaitForResponse(request_headers, 0, default_response_headers_, 0);

  EXPECT_TRUE(upstream_request_->complete());
  EXPECT_EQ(0U, upstream_request_->bodyLength());
  EXPECT_EQ("dummy_aes_128_key", upstream_request_->headers()
                                     .get(Http::LowerCaseString("encryption_key"))[0]
                                     ->value()
                                     .getStringView());
  EXPECT_EQ("password", upstream_request_->headers()
                            .get(Http::LowerCaseString("credential"))[0]
                            ->value()
                            .getStringView());
}

} // namespace Envoy

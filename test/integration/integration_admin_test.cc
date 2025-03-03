#include "test/integration/integration_admin_test.h"

#include <string>

#include "envoy/admin/v2alpha/config_dump.pb.h"
#include "envoy/config/metrics/v2/stats.pb.h"
#include "envoy/http/header_map.h"

#include "common/common/fmt.h"
#include "common/profiler/profiler.h"
#include "common/stats/stats_matcher_impl.h"

#include "test/common/stats/stat_test_utility.h"
#include "test/integration/utility.h"
#include "test/test_common/utility.h"

#include "gtest/gtest.h"
#include "spdlog/spdlog.h"

namespace Envoy {

INSTANTIATE_TEST_SUITE_P(Protocols, IntegrationAdminTest,
                         testing::ValuesIn(HttpProtocolIntegrationTest::getProtocolTestParams(
                             {Http::CodecClient::Type::HTTP1, Http::CodecClient::Type::HTTP2},
                             {FakeHttpConnection::Type::HTTP1})),
                         HttpProtocolIntegrationTest::protocolTestParamsToString);

TEST_P(IntegrationAdminTest, HealthCheck) {
  initialize();

  BufferingStreamDecoderPtr response = IntegrationUtil::makeSingleRequest(
      lookupPort("http"), "POST", "/healthcheck", "", downstreamProtocol(), version_);
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());

  response = IntegrationUtil::makeSingleRequest(lookupPort("admin"), "POST", "/healthcheck/fail",
                                                "", downstreamProtocol(), version_);
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());

  response = IntegrationUtil::makeSingleRequest(lookupPort("http"), "GET", "/healthcheck", "",
                                                downstreamProtocol(), version_);
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("503", response->headers().Status()->value().getStringView());

  response = IntegrationUtil::makeSingleRequest(lookupPort("admin"), "POST", "/healthcheck/ok", "",
                                                downstreamProtocol(), version_);
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());

  response = IntegrationUtil::makeSingleRequest(lookupPort("http"), "GET", "/healthcheck", "",
                                                downstreamProtocol(), version_);
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
}

TEST_P(IntegrationAdminTest, HealthCheckWithBufferFilter) {
  config_helper_.addFilter(ConfigHelper::DEFAULT_BUFFER_FILTER);
  initialize();

  BufferingStreamDecoderPtr response = IntegrationUtil::makeSingleRequest(
      lookupPort("http"), "GET", "/healthcheck", "", downstreamProtocol(), version_);
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
}

TEST_P(IntegrationAdminTest, AdminLogging) {
  initialize();

  BufferingStreamDecoderPtr response = IntegrationUtil::makeSingleRequest(
      lookupPort("admin"), "POST", "/logging", "", downstreamProtocol(), version_);
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());

  // Bad level
  response = IntegrationUtil::makeSingleRequest(lookupPort("admin"), "POST", "/logging?level=blah",
                                                "", downstreamProtocol(), version_);
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("404", response->headers().Status()->value().getStringView());

  // Bad logger
  response = IntegrationUtil::makeSingleRequest(lookupPort("admin"), "POST", "/logging?blah=info",
                                                "", downstreamProtocol(), version_);
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("404", response->headers().Status()->value().getStringView());

  // This is going to stomp over custom log levels that are set on the command line.
  response = IntegrationUtil::makeSingleRequest(
      lookupPort("admin"), "POST", "/logging?level=warning", "", downstreamProtocol(), version_);
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  for (const Logger::Logger& logger : Logger::Registry::loggers()) {
    EXPECT_EQ("warning", logger.levelString());
  }

  response = IntegrationUtil::makeSingleRequest(
      lookupPort("admin"), "POST", "/logging?assert=trace", "", downstreamProtocol(), version_);
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  EXPECT_EQ(spdlog::level::trace, Logger::Registry::getLog(Logger::Id::assert).level());

  spdlog::string_view_t level_name = spdlog::level::level_string_views[default_log_level_];
  response = IntegrationUtil::makeSingleRequest(lookupPort("admin"), "POST",
                                                fmt::format("/logging?level={}", level_name), "",
                                                downstreamProtocol(), version_);
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  for (const Logger::Logger& logger : Logger::Registry::loggers()) {
    EXPECT_EQ(level_name, logger.levelString());
  }
}

namespace {

std::string ContentType(const BufferingStreamDecoderPtr& response) {
  const Http::HeaderEntry* entry = response->headers().ContentType();
  if (entry == nullptr) {
    return "(null)";
  }
  return std::string(entry->value().getStringView());
}

} // namespace

TEST_P(IntegrationAdminTest, Admin) {
  Stats::TestUtil::SymbolTableCreatorTestPeer::setUseFakeSymbolTables(false);
  initialize();

  auto request = [this](absl::string_view request,
                        absl::string_view method) -> BufferingStreamDecoderPtr {
    return IntegrationUtil::makeSingleRequest(lookupPort("admin"), std::string(method),
                                              std::string(request), "", downstreamProtocol(),
                                              version_);
  };

  BufferingStreamDecoderPtr response = request("/notfound", "GET");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("404", response->headers().Status()->value().getStringView());
  EXPECT_EQ("text/plain; charset=UTF-8", ContentType(response));
  EXPECT_NE(std::string::npos, response->body().find("invalid path. admin commands are:"))
      << response->body();

  response = request("/help", "GET");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  EXPECT_EQ("text/plain; charset=UTF-8", ContentType(response));
  EXPECT_NE(std::string::npos, response->body().find("admin commands are:")) << response->body();

  response = request("/", "GET");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  EXPECT_EQ("text/html; charset=UTF-8", ContentType(response));
  EXPECT_NE(std::string::npos, response->body().find("<title>Envoy Admin</title>"))
      << response->body();

  response = request("/server_info", "GET");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  EXPECT_EQ("application/json", ContentType(response));

  response = request("/ready", "GET");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  EXPECT_EQ("text/plain; charset=UTF-8", ContentType(response));

  response = request("/stats", "GET");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  EXPECT_EQ("text/plain; charset=UTF-8", ContentType(response));

  // Our first attempt to get recent lookups will get the error message as they
  // are off by default.
  response = request("/stats/recentlookups", "GET");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  EXPECT_EQ("text/plain; charset=UTF-8", ContentType(response));
  EXPECT_THAT(response->body(), testing::HasSubstr("Lookup tracking is not enabled"));

  // Now enable recent-lookups tracking and check that we get a count.
  request("/stats/recentlookups/enable", "POST");
  response = request("/stats/recentlookups", "GET");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  EXPECT_EQ("text/plain; charset=UTF-8", ContentType(response));
  EXPECT_TRUE(absl::StartsWith(response->body(), "   Count Lookup\n")) << response->body();
  EXPECT_LT(30, response->body().size());

  // Now disable recent-lookups tracking and check that we get the error again.
  request("/stats/recentlookups/disable", "POST");
  response = request("/stats/recentlookups", "GET");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  EXPECT_EQ("text/plain; charset=UTF-8", ContentType(response));
  EXPECT_THAT(response->body(), testing::HasSubstr("Lookup tracking is not enabled"));

  response = request("/stats?usedonly", "GET");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  EXPECT_EQ("text/plain; charset=UTF-8", ContentType(response));

  // Testing a filter with no matches
  response = request("/stats?filter=foo", "GET");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  EXPECT_EQ("text/plain; charset=UTF-8", ContentType(response));

  // Testing a filter with matches
  response = request("/stats?filter=server", "GET");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  EXPECT_EQ("text/plain; charset=UTF-8", ContentType(response));

  response = request("/stats?filter=server&usedonly", "GET");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  EXPECT_EQ("text/plain; charset=UTF-8", ContentType(response));

  response = request("/stats?format=json&usedonly", "GET");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  EXPECT_EQ("application/json", ContentType(response));
  validateStatsJson(response->body(), 0);

  response = request("/stats?format=blah", "GET");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("404", response->headers().Status()->value().getStringView());
  EXPECT_EQ("text/plain; charset=UTF-8", ContentType(response));

  response = request("/stats?format=json", "GET");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("application/json", ContentType(response));
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  validateStatsJson(response->body(), 1);

  // Filtering stats by a regex with one match should return just that match.
  response = request("/stats?format=json&filter=^server\\.version$", "GET");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("application/json", ContentType(response));
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  validateStatsJson(response->body(), 0);
  EXPECT_THAT(response->body(),
              testing::Eq("{\"stats\":[{\"name\":\"server.version\",\"value\":0}]}"));

  // Filtering stats by a non-full-string regex should also return just that match.
  response = request("/stats?format=json&filter=server\\.version", "GET");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("application/json", ContentType(response));
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  validateStatsJson(response->body(), 0);
  EXPECT_THAT(response->body(),
              testing::Eq("{\"stats\":[{\"name\":\"server.version\",\"value\":0}]}"));

  // Filtering stats by a regex with no matches (".*not_intended_to_appear.*") should return a
  // valid, empty, stats array.
  response = request("/stats?format=json&filter=not_intended_to_appear", "GET");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("application/json", ContentType(response));
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  validateStatsJson(response->body(), 0);
  EXPECT_THAT(response->body(), testing::Eq("{\"stats\":[]}"));

  response = request("/stats?format=prometheus", "GET");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  EXPECT_THAT(response->body(),
              testing::HasSubstr(
                  "envoy_http_downstream_rq_xx{envoy_response_code_class=\"4\",envoy_http_conn_"
                  "manager_prefix=\"admin\"} 2\n"));
  EXPECT_THAT(response->body(), testing::HasSubstr("# TYPE envoy_http_downstream_rq_xx counter\n"));
  EXPECT_THAT(response->body(),
              testing::HasSubstr(
                  "envoy_listener_admin_http_downstream_rq_xx{envoy_response_code_class=\"4\","
                  "envoy_http_conn_manager_prefix=\"admin\"} 2\n"));
  EXPECT_THAT(response->body(),
              testing::HasSubstr("# TYPE envoy_cluster_upstream_cx_active gauge\n"));
  EXPECT_THAT(
      response->body(),
      testing::HasSubstr("envoy_cluster_upstream_cx_active{envoy_cluster_name=\"cluster_0\"} 0\n"));

  response = request("/stats/prometheus", "GET");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  EXPECT_THAT(response->body(),
              testing::HasSubstr(
                  "envoy_http_downstream_rq_xx{envoy_response_code_class=\"4\",envoy_http_conn_"
                  "manager_prefix=\"admin\"} 2\n"));
  EXPECT_THAT(response->body(), testing::HasSubstr("# TYPE envoy_http_downstream_rq_xx counter\n"));
  EXPECT_THAT(response->body(),
              testing::HasSubstr(
                  "envoy_listener_admin_http_downstream_rq_xx{envoy_response_code_class=\"4\","
                  "envoy_http_conn_manager_prefix=\"admin\"} 2\n"));
  EXPECT_THAT(response->body(),
              testing::HasSubstr("# TYPE envoy_cluster_upstream_cx_active gauge\n"));
  EXPECT_THAT(
      response->body(),
      testing::HasSubstr("envoy_cluster_upstream_cx_active{envoy_cluster_name=\"cluster_0\"} 0\n"));

  response = request("/clusters", "GET");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  EXPECT_THAT(response->body(), testing::HasSubstr("added_via_api"));
  EXPECT_EQ("text/plain; charset=UTF-8", ContentType(response));

  response = request("/clusters?format=json", "GET");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  EXPECT_EQ("application/json", ContentType(response));
  EXPECT_NO_THROW(Json::Factory::loadFromString(response->body()));

  response = request("/cpuprofiler", "POST");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("400", response->headers().Status()->value().getStringView());
  EXPECT_EQ("text/plain; charset=UTF-8", ContentType(response));

  response = request("/hot_restart_version", "GET");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  EXPECT_EQ("text/plain; charset=UTF-8", ContentType(response));

  response = request("/reset_counters", "POST");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  EXPECT_EQ("text/plain; charset=UTF-8", ContentType(response));

  request("/stats/recentlookups/enable", "POST");
  request("/stats/recentlookups/clear", "POST");
  response = request("/stats/recentlookups", "GET");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  EXPECT_EQ("text/plain; charset=UTF-8", ContentType(response));

  // TODO(#8324): "http1.metadata_not_supported_error" should not still be in
  // the 'recent lookups' output after reset_counters.
  switch (GetParam().downstream_protocol) {
  case Http::CodecClient::Type::HTTP1:
    EXPECT_EQ("   Count Lookup\n"
              "       1 http1.metadata_not_supported_error\n"
              "\n"
              "total: 1\n",
              response->body());
    break;
  case Http::CodecClient::Type::HTTP2:
    EXPECT_EQ("   Count Lookup\n"
              "       1 http2.header_overflow\n"
              "       1 http2.headers_cb_no_stream\n"
              "       1 http2.inbound_empty_frames_flood\n"
              "       1 http2.inbound_priority_frames_flood\n"
              "       1 http2.inbound_window_update_frames_flood\n"
              "       1 http2.outbound_control_flood\n"
              "       1 http2.outbound_flood\n"
              "       1 http2.rx_messaging_error\n"
              "       1 http2.rx_reset\n"
              "       1 http2.too_many_header_frames\n"
              "       1 http2.trailers\n"
              "       1 http2.tx_reset\n"
              "\n"
              "total: 12\n",
              response->body());
    break;
  case Http::CodecClient::Type::HTTP3:
    NOT_IMPLEMENTED_GCOVR_EXCL_LINE;
  }

  response = request("/certs", "GET");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  EXPECT_EQ("application/json", ContentType(response));

  response = request("/runtime", "GET");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  EXPECT_EQ("application/json", ContentType(response));

  response = request("/runtime_modify?foo=bar&foo1=bar1", "POST");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());

  response = request("/runtime?format=json", "GET");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  EXPECT_EQ("application/json", ContentType(response));

  Json::ObjectSharedPtr json = Json::Factory::loadFromString(response->body());
  auto entries = json->getObject("entries");
  auto foo_obj = entries->getObject("foo");
  EXPECT_EQ("bar", foo_obj->getString("final_value"));
  auto foo1_obj = entries->getObject("foo1");
  EXPECT_EQ("bar1", foo1_obj->getString("final_value"));

  response = request("/listeners", "GET");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  EXPECT_EQ("text/plain; charset=UTF-8", ContentType(response));
  auto listeners = test_server_->server().listenerManager().listeners();
  auto listener_it = listeners.cbegin();
  for (; listener_it != listeners.end(); ++listener_it) {
    EXPECT_THAT(response->body(), testing::HasSubstr(fmt::format(
                                      "{}::{}", listener_it->get().name(),
                                      listener_it->get().socket().localAddress()->asString())));
  }

  response = request("/listeners?format=json", "GET");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  EXPECT_EQ("application/json", ContentType(response));

  json = Json::Factory::loadFromString(response->body());
  std::vector<Json::ObjectSharedPtr> listener_info = json->getObjectArray("listener_statuses");
  auto listener_info_it = listener_info.cbegin();
  listeners = test_server_->server().listenerManager().listeners();
  listener_it = listeners.cbegin();
  for (; listener_info_it != listener_info.end() && listener_it != listeners.end();
       ++listener_info_it, ++listener_it) {
    auto local_address = (*listener_info_it)->getObject("local_address");
    auto socket_address = local_address->getObject("socket_address");
    EXPECT_EQ(listener_it->get().socket().localAddress()->ip()->addressAsString(),
              socket_address->getString("address"));
    EXPECT_EQ(listener_it->get().socket().localAddress()->ip()->port(),
              socket_address->getInteger("port_value"));
  }

  response = request("/config_dump", "GET");
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  EXPECT_EQ("application/json", ContentType(response));
  json = Json::Factory::loadFromString(response->body());
  size_t index = 0;
  const std::string expected_types[] = {
      "type.googleapis.com/envoy.admin.v2alpha.BootstrapConfigDump",
      "type.googleapis.com/envoy.admin.v2alpha.ClustersConfigDump",
      "type.googleapis.com/envoy.admin.v2alpha.ListenersConfigDump",
      "type.googleapis.com/envoy.admin.v2alpha.ScopedRoutesConfigDump",
      "type.googleapis.com/envoy.admin.v2alpha.RoutesConfigDump",
      "type.googleapis.com/envoy.admin.v2alpha.SecretsConfigDump"};

  for (const Json::ObjectSharedPtr& obj_ptr : json->getObjectArray("configs")) {
    EXPECT_TRUE(expected_types[index].compare(obj_ptr->getString("@type")) == 0);
    index++;
  }

  // Validate we can parse as proto.
  envoy::admin::v2alpha::ConfigDump config_dump;
  TestUtility::loadFromJson(response->body(), config_dump);
  EXPECT_EQ(6, config_dump.configs_size());

  // .. and that we can unpack one of the entries.
  envoy::admin::v2alpha::RoutesConfigDump route_config_dump;
  config_dump.configs(4).UnpackTo(&route_config_dump);
  EXPECT_EQ("route_config_0", route_config_dump.static_route_configs(0).route_config().name());

  envoy::admin::v2alpha::SecretsConfigDump secret_config_dump;
  config_dump.configs(5).UnpackTo(&secret_config_dump);
  EXPECT_EQ("secret_static_0", secret_config_dump.static_secrets(0).name());

  // Validate that the "inboundonly" does not stop the default listener.
  response = IntegrationUtil::makeSingleRequest(lookupPort("admin"), "POST",
                                                "/drain_listeners?inboundonly", "",
                                                downstreamProtocol(), version_);
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  EXPECT_EQ("text/plain; charset=UTF-8", ContentType(response));
  EXPECT_EQ("OK\n", response->body());

  // Validate that the listener stopped stat is not used and still zero.
  EXPECT_FALSE(test_server_->counter("listener_manager.listener_stopped")->used());
  EXPECT_EQ(0, test_server_->counter("listener_manager.listener_stopped")->value());

  // Now validate that the drain_listeners stops the listeners.
  response = IntegrationUtil::makeSingleRequest(lookupPort("admin"), "POST", "/drain_listeners", "",
                                                downstreamProtocol(), version_);
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  EXPECT_EQ("text/plain; charset=UTF-8", ContentType(response));
  EXPECT_EQ("OK\n", response->body());

  test_server_->waitForCounterEq("listener_manager.listener_stopped", 1);
}

// Validates that the "inboundonly" drains inbound listeners.
TEST_P(IntegrationAdminTest, AdminDrainInboundOnly) {
  config_helper_.addConfigModifier([&](envoy::config::bootstrap::v2::Bootstrap& bootstrap) -> void {
    auto* inbound_listener = bootstrap.mutable_static_resources()->mutable_listeners(0);
    inbound_listener->set_traffic_direction(envoy::api::v2::core::TrafficDirection::INBOUND);
    inbound_listener->set_name("inbound_0");
  });
  initialize();

  BufferingStreamDecoderPtr response = IntegrationUtil::makeSingleRequest(
      lookupPort("admin"), "POST", "/drain_listeners?inboundonly", "", downstreamProtocol(),
      version_);
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  EXPECT_EQ("text/plain; charset=UTF-8", ContentType(response));
  EXPECT_EQ("OK\n", response->body());

  // Validate that the inbound listener has been stopped.
  test_server_->waitForCounterEq("listener_manager.listener_stopped", 1);
}

TEST_P(IntegrationAdminTest, AdminOnDestroyCallbacks) {
  initialize();
  bool test = true;

  // add an handler which adds a callback to the list of callback called when connection is dropped.
  auto callback = [&test](absl::string_view, Http::HeaderMap&, Buffer::Instance&,
                          Server::AdminStream& admin_stream) -> Http::Code {
    auto on_destroy_callback = [&test]() { test = false; };

    // Add the on_destroy_callback to the admin_filter list of callbacks.
    admin_stream.addOnDestroyCallback(std::move(on_destroy_callback));
    return Http::Code::OK;
  };

  EXPECT_TRUE(
      test_server_->server().admin().addHandler("/foo/bar", "hello", callback, true, false));

  // As part of the request, on destroy() should be called and the on_destroy_callback invoked.
  BufferingStreamDecoderPtr response = IntegrationUtil::makeSingleRequest(
      lookupPort("admin"), "GET", "/foo/bar", "", downstreamProtocol(), version_);

  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  // Check that the added callback was invoked.
  EXPECT_EQ(test, false);

  // Small test to cover new statsFlushInterval() on Instance.h.
  EXPECT_EQ(test_server_->server().statsFlushInterval(), std::chrono::milliseconds(5000));
}

TEST_P(IntegrationAdminTest, AdminCpuProfilerStart) {
  config_helper_.addConfigModifier([&](envoy::config::bootstrap::v2::Bootstrap& bootstrap) -> void {
    auto* admin = bootstrap.mutable_admin();
    admin->set_profile_path(TestEnvironment::temporaryPath("/envoy.prof"));
  });

  initialize();
  BufferingStreamDecoderPtr response = IntegrationUtil::makeSingleRequest(
      lookupPort("admin"), "POST", "/cpuprofiler?enable=y", "", downstreamProtocol(), version_);
  EXPECT_TRUE(response->complete());
#ifdef PROFILER_AVAILABLE
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
#else
  EXPECT_EQ("500", response->headers().Status()->value().getStringView());
#endif

  response = IntegrationUtil::makeSingleRequest(
      lookupPort("admin"), "POST", "/cpuprofiler?enable=n", "", downstreamProtocol(), version_);
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
}

class IntegrationAdminIpv4Ipv6Test : public testing::Test, public HttpIntegrationTest {
public:
  IntegrationAdminIpv4Ipv6Test()
      : HttpIntegrationTest(Http::CodecClient::Type::HTTP1, Network::Address::IpVersion::v4) {}

  void initialize() override {
    config_helper_.addConfigModifier(
        [&](envoy::config::bootstrap::v2::Bootstrap& bootstrap) -> void {
          auto* socket_address =
              bootstrap.mutable_admin()->mutable_address()->mutable_socket_address();
          socket_address->set_ipv4_compat(true);
          socket_address->set_address("::");
        });
    HttpIntegrationTest::initialize();
  }
};

// Verify an IPv4 client can connect to the admin interface listening on :: when
// IPv4 compat mode is enabled.
TEST_F(IntegrationAdminIpv4Ipv6Test, Ipv4Ipv6Listen) {
  if (TestEnvironment::shouldRunTestForIpVersion(Network::Address::IpVersion::v4) &&
      TestEnvironment::shouldRunTestForIpVersion(Network::Address::IpVersion::v6)) {
    initialize();
    BufferingStreamDecoderPtr response = IntegrationUtil::makeSingleRequest(
        lookupPort("admin"), "GET", "/server_info", "", downstreamProtocol(), version_);
    EXPECT_TRUE(response->complete());
    EXPECT_EQ("200", response->headers().Status()->value().getStringView());
  }
}

// Testing the behavior of StatsMatcher, which allows/denies the  instantiation of stats based on
// restrictions on their names.
//
// Note: using 'Event::TestUsingSimulatedTime' appears to conflict with LDS in
// StatsMatcherIntegrationTest.IncludeExact, which manifests in a coverage test
// crash, which is really difficult to debug. See #7215. It's possible this is
// due to a bad interaction between the wait-for constructs in the integration
// test framework with sim-time.
class StatsMatcherIntegrationTest
    : public testing::Test,
      public HttpIntegrationTest,
      public testing::WithParamInterface<Network::Address::IpVersion> {
public:
  StatsMatcherIntegrationTest() : HttpIntegrationTest(Http::CodecClient::Type::HTTP1, GetParam()) {}

  void initialize() override {
    config_helper_.addConfigModifier(
        [this](envoy::config::bootstrap::v2::Bootstrap& bootstrap) -> void {
          *bootstrap.mutable_stats_config()->mutable_stats_matcher() = stats_matcher_;
        });
    HttpIntegrationTest::initialize();
  }
  void makeRequest() {
    response_ = IntegrationUtil::makeSingleRequest(lookupPort("admin"), "GET", "/stats", "",
                                                   downstreamProtocol(), version_);
    ASSERT_TRUE(response_->complete());
    EXPECT_EQ("200", response_->headers().Status()->value().getStringView());
  }

  BufferingStreamDecoderPtr response_;
  envoy::config::metrics::v2::StatsMatcher stats_matcher_;
};
INSTANTIATE_TEST_SUITE_P(IpVersions, StatsMatcherIntegrationTest,
                         testing::ValuesIn(TestEnvironment::getIpVersionsForTest()),
                         TestUtility::ipTestParamsToString);

// Verify that StatsMatcher prevents the printing of uninstantiated stats.
TEST_P(StatsMatcherIntegrationTest, ExcludePrefixServerDot) {
  stats_matcher_.mutable_exclusion_list()->add_patterns()->set_prefix("server.");
  initialize();
  makeRequest();
  EXPECT_THAT(response_->body(), testing::Not(testing::HasSubstr("server.")));
}

TEST_P(StatsMatcherIntegrationTest, DEPRECATED_FEATURE_TEST(ExcludeRequests)) {
  stats_matcher_.mutable_exclusion_list()->add_patterns()->set_regex(".*requests.*");
  initialize();
  makeRequest();
  EXPECT_THAT(response_->body(), testing::Not(testing::HasSubstr("requests")));
}

TEST_P(StatsMatcherIntegrationTest, DEPRECATED_FEATURE_TEST(ExcludeExact)) {
  stats_matcher_.mutable_exclusion_list()->add_patterns()->set_exact("server.concurrency");
  initialize();
  makeRequest();
  EXPECT_THAT(response_->body(), testing::Not(testing::HasSubstr("server.concurrency")));
}

TEST_P(StatsMatcherIntegrationTest, DEPRECATED_FEATURE_TEST(ExcludeMultipleExact)) {
  stats_matcher_.mutable_exclusion_list()->add_patterns()->set_exact("server.concurrency");
  stats_matcher_.mutable_exclusion_list()->add_patterns()->set_regex(".*live");
  initialize();
  makeRequest();
  EXPECT_THAT(response_->body(), testing::Not(testing::HasSubstr("server.concurrency")));
  EXPECT_THAT(response_->body(), testing::Not(testing::HasSubstr("server.live")));
}

// TODO(ambuc): Find a cleaner way to test this. This test has an unfortunate compromise:
// `listener_manager.listener_create_success` must be instantiated, because BaseIntegrationTest
// blocks on its creation (see waitForCounterGe and the suite of waitFor* functions).
// If this invariant is changed, this test must be rewritten.
TEST_P(StatsMatcherIntegrationTest, DEPRECATED_FEATURE_TEST(IncludeExact)) {
  // Stats matching does not play well with LDS, at least in test. See #7215.
  use_lds_ = false;
  stats_matcher_.mutable_inclusion_list()->add_patterns()->set_exact(
      "listener_manager.listener_create_success");
  initialize();
  makeRequest();
  EXPECT_EQ(response_->body(), "listener_manager.listener_create_success: 1\n");
}

} // namespace Envoy

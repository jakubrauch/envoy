#include "test/tools/router_check/router.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "common/network/utility.h"
#include "common/protobuf/message_validator_impl.h"
#include "common/protobuf/utility.h"
#include "common/runtime/runtime_impl.h"
#include "common/stream_info/stream_info_impl.h"

#include "test/test_common/printers.h"

namespace Envoy {
// static
ToolConfig ToolConfig::create(const Json::ObjectSharedPtr check_config) {
  Json::ObjectSharedPtr input = check_config->getObject("input");
  int random_value = input->getInteger("random_value", 0);

  // Add header field values
  std::unique_ptr<Http::TestHeaderMapImpl> headers(new Http::TestHeaderMapImpl());
  headers->addCopy(":authority", input->getString(":authority", ""));
  headers->addCopy(":path", input->getString(":path", ""));
  headers->addCopy(":method", input->getString(":method", "GET"));
  headers->addCopy("x-forwarded-proto", input->getBoolean("ssl", false) ? "https" : "http");

  if (input->getBoolean("internal", false)) {
    headers->addCopy("x-envoy-internal", "true");
  }

  if (input->hasObject("additional_headers")) {
    for (const Json::ObjectSharedPtr& header_config : input->getObjectArray("additional_headers")) {
      headers->addCopy(header_config->getString("field"), header_config->getString("value"));
    }
  }

  return ToolConfig(std::move(headers), random_value);
}

ToolConfig ToolConfig::create(const envoy::RouterCheckToolSchema::ValidationItem& check_config) {
  // Add header field values
  std::unique_ptr<Http::TestHeaderMapImpl> headers(new Http::TestHeaderMapImpl());
  headers->addCopy(":authority", check_config.input().authority());
  headers->addCopy(":path", check_config.input().path());
  headers->addCopy(":method", check_config.input().method());
  headers->addCopy("x-forwarded-proto", check_config.input().ssl() ? "https" : "http");

  if (check_config.input().internal()) {
    headers->addCopy("x-envoy-internal", "true");
  }

  if (check_config.input().additional_headers().data()) {
    for (const envoy::api::v2::core::HeaderValue& header_config :
         check_config.input().additional_headers()) {
      headers->addCopy(header_config.key(), header_config.value());
    }
  }

  return ToolConfig(std::move(headers), check_config.input().random_value());
}

ToolConfig::ToolConfig(std::unique_ptr<Http::TestHeaderMapImpl> headers, int random_value)
    : headers_(std::move(headers)), random_value_(random_value) {}

// static
RouterCheckTool RouterCheckTool::create(const std::string& router_config_file,
                                        const bool disable_deprecation_check) {
  // TODO(hennna): Allow users to load a full config and extract the route configuration from it.
  envoy::api::v2::RouteConfiguration route_config;
  auto stats = std::make_unique<Stats::IsolatedStoreImpl>();
  auto api = Api::createApiForTest(*stats);
  TestUtility::loadFromFile(router_config_file, route_config, *api);
  assignUniqueRouteNames(route_config);

  auto factory_context =
      std::make_unique<NiceMock<Server::Configuration::MockServerFactoryContext>>();
  auto config = std::make_unique<Router::ConfigImpl>(
      route_config, *factory_context, ProtobufMessage::getNullValidationVisitor(), false);
  if (!disable_deprecation_check) {
    MessageUtil::checkForUnexpectedFields(route_config,
                                          ProtobufMessage::getStrictValidationVisitor(),
                                          &factory_context->runtime_loader_);
  }

  return RouterCheckTool(std::move(factory_context), std::move(config), std::move(stats),
                         std::move(api), Coverage(route_config));
}

void RouterCheckTool::assignUniqueRouteNames(envoy::api::v2::RouteConfiguration& route_config) {
  Runtime::RandomGeneratorImpl random;
  for (auto& host : *route_config.mutable_virtual_hosts()) {
    for (auto& route : *host.mutable_routes()) {
      route.set_name(random.uuid());
    }
  }
}

RouterCheckTool::RouterCheckTool(
    std::unique_ptr<NiceMock<Server::Configuration::MockServerFactoryContext>> factory_context,
    std::unique_ptr<Router::ConfigImpl> config, std::unique_ptr<Stats::IsolatedStoreImpl> stats,
    Api::ApiPtr api, Coverage coverage)
    : factory_context_(std::move(factory_context)), config_(std::move(config)),
      stats_(std::move(stats)), api_(std::move(api)), coverage_(std::move(coverage)) {
  ON_CALL(factory_context_->runtime_loader_.snapshot_,
          featureEnabled(_, testing::An<const envoy::type::FractionalPercent&>(),
                         testing::An<uint64_t>()))
      .WillByDefault(testing::Invoke(this, &RouterCheckTool::runtimeMock));
}

// TODO(jyotima): Remove this code path once the json schema code path is deprecated.
bool RouterCheckTool::compareEntriesInJson(const std::string& expected_route_json) {
  Json::ObjectSharedPtr loader = Json::Factory::loadFromFile(expected_route_json, *api_);
  loader->validateSchema(Json::ToolSchema::routerCheckSchema());
  bool no_failures = true;
  for (const Json::ObjectSharedPtr& check_config : loader->asObjectArray()) {
    headers_finalized_ = false;
    Envoy::StreamInfo::StreamInfoImpl stream_info(Envoy::Http::Protocol::Http11,
                                                  factory_context_->dispatcher().timeSource());
    ToolConfig tool_config = ToolConfig::create(check_config);
    tool_config.route_ =
        config_->route(*tool_config.headers_, stream_info, tool_config.random_value_);
    std::string test_name = check_config->getString("test_name", "");
    tests_.emplace_back(test_name, std::vector<std::string>{});
    Json::ObjectSharedPtr validate = check_config->getObject("validate");
    using CheckerFunc = std::function<bool(ToolConfig&, const std::string&)>;
    const std::unordered_map<std::string, CheckerFunc> checkers = {
        {"cluster_name",
         [this](auto&... params) -> bool { return this->compareCluster(params...); }},
        {"virtual_cluster_name",
         [this](auto&... params) -> bool { return this->compareVirtualCluster(params...); }},
        {"virtual_host_name",
         [this](auto&... params) -> bool { return this->compareVirtualHost(params...); }},
        {"path_rewrite",
         [this](auto&... params) -> bool { return this->compareRewritePath(params...); }},
        {"host_rewrite",
         [this](auto&... params) -> bool { return this->compareRewriteHost(params...); }},
        {"path_redirect",
         [this](auto&... params) -> bool { return this->compareRedirectPath(params...); }},
    };
    // Call appropriate function for each match case.
    for (const auto& test : checkers) {
      if (validate->hasObject(test.first)) {
        const std::string& expected = validate->getString(test.first);
        if (tool_config.route_ == nullptr) {
          compareResults("", expected, test.first);
        } else {
          if (!test.second(tool_config, expected)) {
            no_failures = false;
          }
        }
      }
    }
    if (validate->hasObject("header_fields")) {
      for (const Json::ObjectSharedPtr& header_field : validate->getObjectArray("header_fields")) {
        if (!compareHeaderField(tool_config, header_field->getString("field"),
                                header_field->getString("value"))) {
          no_failures = false;
        }
      }
    }
    if (validate->hasObject("custom_header_fields")) {
      for (const Json::ObjectSharedPtr& header_field :
           validate->getObjectArray("custom_header_fields")) {
        if (!compareCustomHeaderField(tool_config, header_field->getString("field"),
                                      header_field->getString("value"))) {
          no_failures = false;
        }
      }
    }
  }
  printResults();
  return no_failures;
}

bool RouterCheckTool::compareEntries(const std::string& expected_routes) {
  envoy::RouterCheckToolSchema::Validation validation_config;
  auto stats = std::make_unique<Stats::IsolatedStoreImpl>();
  auto api = Api::createApiForTest(*stats);
  const std::string contents = api->fileSystem().fileReadToEnd(expected_routes);
  TestUtility::loadFromFile(expected_routes, validation_config, *api);
  TestUtility::validate(validation_config);

  bool no_failures = true;
  for (const envoy::RouterCheckToolSchema::ValidationItem& check_config :
       validation_config.tests()) {
    active_runtime_ = check_config.input().runtime();
    headers_finalized_ = false;
    Envoy::StreamInfo::StreamInfoImpl stream_info(Envoy::Http::Protocol::Http11,
                                                  factory_context_->dispatcher().timeSource());

    ToolConfig tool_config = ToolConfig::create(check_config);
    tool_config.route_ =
        config_->route(*tool_config.headers_, stream_info, tool_config.random_value_);

    const std::string& test_name = check_config.test_name();
    tests_.emplace_back(test_name, std::vector<std::string>{});
    const envoy::RouterCheckToolSchema::ValidationAssert& validate = check_config.validate();

    using CheckerFunc =
        std::function<bool(ToolConfig&, const envoy::RouterCheckToolSchema::ValidationAssert&)>;
    CheckerFunc checkers[] = {
        [this](auto&... params) -> bool { return this->compareCluster(params...); },
        [this](auto&... params) -> bool { return this->compareVirtualCluster(params...); },
        [this](auto&... params) -> bool { return this->compareVirtualHost(params...); },
        [this](auto&... params) -> bool { return this->compareRewritePath(params...); },
        [this](auto&... params) -> bool { return this->compareRewriteHost(params...); },
        [this](auto&... params) -> bool { return this->compareRedirectPath(params...); },
        [this](auto&... params) -> bool { return this->compareHeaderField(params...); },
        [this](auto&... params) -> bool { return this->compareCustomHeaderField(params...); },
    };

    // Call appropriate function for each match case.
    for (const auto& test : checkers) {
      if (!test(tool_config, validate)) {
        no_failures = false;
      }
    }
  }
  printResults();
  return no_failures;
}

bool RouterCheckTool::compareCluster(ToolConfig& tool_config, const std::string& expected) {
  std::string actual = "";

  if (tool_config.route_->routeEntry() != nullptr) {
    actual = tool_config.route_->routeEntry()->clusterName();
  }
  const bool matches = compareResults(actual, expected, "cluster_name");
  if (matches && tool_config.route_->routeEntry() != nullptr) {
    coverage_.markClusterCovered(*tool_config.route_->routeEntry());
  }
  return matches;
}

bool RouterCheckTool::compareCluster(
    ToolConfig& tool_config, const envoy::RouterCheckToolSchema::ValidationAssert& expected) {
  if (!expected.has_cluster_name()) {
    return true;
  }
  if (tool_config.route_ == nullptr) {
    return compareResults("", expected.cluster_name().value(), "cluster_name");
  }
  return compareCluster(tool_config, expected.cluster_name().value());
}

bool RouterCheckTool::compareVirtualCluster(ToolConfig& tool_config, const std::string& expected) {
  std::string actual = "";

  if (tool_config.route_->routeEntry() != nullptr &&
      tool_config.route_->routeEntry()->virtualCluster(*tool_config.headers_) != nullptr) {
    Stats::StatName stat_name =
        tool_config.route_->routeEntry()->virtualCluster(*tool_config.headers_)->statName();
    actual = tool_config.symbolTable().toString(stat_name);
  }
  const bool matches = compareResults(actual, expected, "virtual_cluster_name");
  if (matches && tool_config.route_->routeEntry() != nullptr) {
    coverage_.markVirtualClusterCovered(*tool_config.route_->routeEntry());
  }
  return matches;
}

bool RouterCheckTool::compareVirtualCluster(
    ToolConfig& tool_config, const envoy::RouterCheckToolSchema::ValidationAssert& expected) {
  if (!expected.has_virtual_cluster_name()) {
    return true;
  }
  if (tool_config.route_ == nullptr) {
    return compareResults("", expected.virtual_cluster_name().value(), "virtual_cluster_name");
  }
  return compareVirtualCluster(tool_config, expected.virtual_cluster_name().value());
}

bool RouterCheckTool::compareVirtualHost(ToolConfig& tool_config, const std::string& expected) {
  std::string actual = "";
  if (tool_config.route_->routeEntry() != nullptr) {
    Stats::StatName stat_name = tool_config.route_->routeEntry()->virtualHost().statName();
    actual = tool_config.symbolTable().toString(stat_name);
  }
  const bool matches = compareResults(actual, expected, "virtual_host_name");
  if (matches && tool_config.route_->routeEntry() != nullptr) {
    coverage_.markVirtualHostCovered(*tool_config.route_->routeEntry());
  }
  return matches;
}

bool RouterCheckTool::compareVirtualHost(
    ToolConfig& tool_config, const envoy::RouterCheckToolSchema::ValidationAssert& expected) {
  if (!expected.has_virtual_host_name()) {
    return true;
  }
  if (tool_config.route_ == nullptr) {
    return compareResults("", expected.virtual_host_name().value(), "virtual_host_name");
  }
  return compareVirtualHost(tool_config, expected.virtual_host_name().value());
}

bool RouterCheckTool::compareRewritePath(ToolConfig& tool_config, const std::string& expected) {
  std::string actual = "";
  Envoy::StreamInfo::StreamInfoImpl stream_info(Envoy::Http::Protocol::Http11,
                                                factory_context_->dispatcher().timeSource());
  if (tool_config.route_->routeEntry() != nullptr) {
    if (!headers_finalized_) {
      tool_config.route_->routeEntry()->finalizeRequestHeaders(*tool_config.headers_, stream_info,
                                                               true);
      headers_finalized_ = true;
    }

    actual = tool_config.headers_->get_(Http::Headers::get().Path);
  }
  const bool matches = compareResults(actual, expected, "path_rewrite");
  if (matches && tool_config.route_->routeEntry() != nullptr) {
    coverage_.markPathRewriteCovered(*tool_config.route_->routeEntry());
  }
  return matches;
}

bool RouterCheckTool::compareRewritePath(
    ToolConfig& tool_config, const envoy::RouterCheckToolSchema::ValidationAssert& expected) {
  if (!expected.has_path_rewrite()) {
    return true;
  }
  if (tool_config.route_ == nullptr) {
    return compareResults("", expected.path_rewrite().value(), "path_rewrite");
  }
  return compareRewritePath(tool_config, expected.path_rewrite().value());
}

bool RouterCheckTool::compareRewriteHost(ToolConfig& tool_config, const std::string& expected) {
  std::string actual = "";
  Envoy::StreamInfo::StreamInfoImpl stream_info(Envoy::Http::Protocol::Http11,
                                                factory_context_->dispatcher().timeSource());
  if (tool_config.route_->routeEntry() != nullptr) {
    if (!headers_finalized_) {
      tool_config.route_->routeEntry()->finalizeRequestHeaders(*tool_config.headers_, stream_info,
                                                               true);
      headers_finalized_ = true;
    }

    actual = tool_config.headers_->get_(Http::Headers::get().Host);
  }
  const bool matches = compareResults(actual, expected, "host_rewrite");
  if (matches && tool_config.route_->routeEntry() != nullptr) {
    coverage_.markHostRewriteCovered(*tool_config.route_->routeEntry());
  }
  return matches;
}

bool RouterCheckTool::compareRewriteHost(
    ToolConfig& tool_config, const envoy::RouterCheckToolSchema::ValidationAssert& expected) {
  if (!expected.has_host_rewrite()) {
    return true;
  }
  if (tool_config.route_ == nullptr) {
    return compareResults("", expected.host_rewrite().value(), "host_rewrite");
  }
  return compareRewriteHost(tool_config, expected.host_rewrite().value());
}

bool RouterCheckTool::compareRedirectPath(ToolConfig& tool_config, const std::string& expected) {
  std::string actual = "";
  if (tool_config.route_->directResponseEntry() != nullptr) {
    actual = tool_config.route_->directResponseEntry()->newPath(*tool_config.headers_);
  }

  const bool matches = compareResults(actual, expected, "path_redirect");
  if (matches && tool_config.route_->routeEntry() != nullptr) {
    coverage_.markRedirectPathCovered(*tool_config.route_->routeEntry());
  }
  return matches;
}

bool RouterCheckTool::compareRedirectPath(
    ToolConfig& tool_config, const envoy::RouterCheckToolSchema::ValidationAssert& expected) {
  if (!expected.has_path_redirect()) {
    return true;
  }
  if (tool_config.route_ == nullptr) {
    return compareResults("", expected.path_redirect().value(), "path_redirect");
  }
  return compareRedirectPath(tool_config, expected.path_redirect().value());
}

bool RouterCheckTool::compareHeaderField(
    ToolConfig& tool_config, const envoy::RouterCheckToolSchema::ValidationAssert& expected) {
  bool no_failures = true;
  if (expected.header_fields().data()) {
    for (const envoy::api::v2::core::HeaderValue& header : expected.header_fields()) {
      if (!compareHeaderField(tool_config, header.key(), header.value())) {
        no_failures = false;
      }
    }
  }
  return no_failures;
}

bool RouterCheckTool::compareHeaderField(ToolConfig& tool_config, const std::string& field,
                                         const std::string& expected) {
  std::string actual = tool_config.headers_->get_(field);
  return compareResults(actual, expected, "check_header");
}

bool RouterCheckTool::compareCustomHeaderField(ToolConfig& tool_config, const std::string& field,
                                               const std::string& expected) {
  std::string actual = "";
  Envoy::StreamInfo::StreamInfoImpl stream_info(Envoy::Http::Protocol::Http11,
                                                factory_context_->dispatcher().timeSource());
  stream_info.setDownstreamRemoteAddress(Network::Utility::getCanonicalIpv4LoopbackAddress());
  if (tool_config.route_->routeEntry() != nullptr) {
    tool_config.route_->routeEntry()->finalizeRequestHeaders(*tool_config.headers_, stream_info,
                                                             true);
    actual = tool_config.headers_->get_(field);
  }
  return compareResults(actual, expected, "custom_header");
}

bool RouterCheckTool::compareCustomHeaderField(
    ToolConfig& tool_config, const envoy::RouterCheckToolSchema::ValidationAssert& expected) {
  bool no_failures = true;
  if (expected.custom_header_fields().data()) {
    for (const envoy::api::v2::core::HeaderValue& header : expected.custom_header_fields()) {
      if (!compareCustomHeaderField(tool_config, header.key(), header.value())) {
        no_failures = false;
      }
    }
  }
  return no_failures;
}

bool RouterCheckTool::compareResults(const std::string& actual, const std::string& expected,
                                     const std::string& test_type) {
  if (expected == actual) {
    return true;
  }
  tests_.back().second.emplace_back("expected: [" + expected + "], actual: [" + actual +
                                    "], test type: " + test_type);
  return false;
}

void RouterCheckTool::printResults() {
  // Output failure details to stdout if details_ flag is set to true
  for (const auto& test_result : tests_) {
    // All test names are printed if the details_ flag is true unless only_show_failures_ is also
    // true.
    if ((details_ && !only_show_failures_) ||
        (only_show_failures_ && !test_result.second.empty())) {
      std::cout << test_result.first << std::endl;
      for (const auto& failure : test_result.second) {
        std::cerr << failure << std::endl;
      }
    }
  }
}

// The Mock for runtime value checks.
// This is a simple implementation to mimic the actual runtime checks in Snapshot.featureEnabled
bool RouterCheckTool::runtimeMock(const std::string& key,
                                  const envoy::type::FractionalPercent& default_value,
                                  uint64_t random_value) {
  return !active_runtime_.empty() && active_runtime_.compare(key) == 0 &&
         ProtobufPercentHelper::evaluateFractionalPercent(default_value, random_value);
}

Options::Options(int argc, char** argv) {
  TCLAP::CmdLine cmd("router_check_tool", ' ', "none", true);
  TCLAP::SwitchArg is_proto("p", "useproto", "Use Proto test file schema", cmd, false);
  TCLAP::SwitchArg is_detailed("d", "details", "Show detailed test execution results", cmd, false);
  TCLAP::SwitchArg only_show_failures("", "only-show-failures", "Only display failing tests", cmd,
                                      false);
  TCLAP::SwitchArg disable_deprecation_check("", "disable-deprecation-check",
                                             "Disable deprecated fields check", cmd, false);
  TCLAP::ValueArg<double> fail_under("f", "fail-under",
                                     "Fail if test coverage is under a specified amount", false,
                                     0.0, "float", cmd);
  TCLAP::SwitchArg comprehensive_coverage(
      "", "covall", "Measure coverage by checking all route fields", cmd, false);
  TCLAP::ValueArg<std::string> config_path("c", "config-path", "Path to configuration file.", false,
                                           "", "string", cmd);
  TCLAP::ValueArg<std::string> test_path("t", "test-path", "Path to test file.", false, "",
                                         "string", cmd);
  TCLAP::UnlabeledMultiArg<std::string> unlabelled_configs(
      "unlabelled-configs", "unlabelled configs", false, "unlabelledConfigStrings", cmd);
  try {
    cmd.parse(argc, argv);
  } catch (TCLAP::ArgException& e) {
    std::cerr << "error: " << e.error() << std::endl;
    exit(EXIT_FAILURE);
  }

  is_proto_ = is_proto.getValue();
  is_detailed_ = is_detailed.getValue();
  only_show_failures_ = only_show_failures.getValue();
  fail_under_ = fail_under.getValue();
  comprehensive_coverage_ = comprehensive_coverage.getValue();
  disable_deprecation_check_ = disable_deprecation_check.getValue();

  if (is_proto_) {
    config_path_ = config_path.getValue();
    test_path_ = test_path.getValue();
    if (config_path_.empty() || test_path_.empty()) {
      std::cerr << "error: "
                << "Both --config-path/c and --test-path/t are mandatory with --useproto"
                << std::endl;
      exit(EXIT_FAILURE);
    }
  } else {
    if (!unlabelled_configs.getValue().empty()) {
      unlabelled_config_path_ = unlabelled_configs.getValue()[0];
      unlabelled_test_path_ = unlabelled_configs.getValue()[1];
    }
  }
}
} // namespace Envoy

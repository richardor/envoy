#include "test/integration/lua_integration_test.h"

namespace Envoy {

INSTANTIATE_TEST_CASE_P(IpVersions, LuaIntegrationTest,
                        testing::ValuesIn(TestEnvironment::getIpVersionsForTest()));

TEST_P(LuaIntegrationTest, RouterRequestAndResponseWithBodyNoBuffer) {
  const std::string FILTER_AND_CODE =
      R"EOF(
name: envoy.lua
config:
  deprecated_v1: true
  value:
    inline_code: |
      function envoy_on_request(request_handle)
        request_handle:headers():add("request_body_size", request_handle:body():byteSize())
      end

      function envoy_on_response(response_handle)
        response_handle:headers():add("response_body_size", response_handle:body():byteSize())
        response_handle:headers():remove("foo")
      end
)EOF";

  config_helper_.addFilter(FILTER_AND_CODE);

  initialize();
  codec_client_ = makeHttpConnection(makeClientConnection(lookupPort("http")));
  Http::TestHeaderMapImpl request_headers{{":method", "POST"},
                                          {":path", "/test/long/url"},
                                          {":scheme", "http"},
                                          {":authority", "host"},
                                          {"x-forwarded-for", "10.0.0.1"}};

  Http::StreamEncoder& encoder = codec_client_->startRequest(request_headers, *response_);
  Buffer::OwnedImpl request_data1("hello");
  encoder.encodeData(request_data1, false);
  Buffer::OwnedImpl request_data2("world");
  encoder.encodeData(request_data2, true);

  waitForNextUpstreamRequest();
  EXPECT_STREQ("10", upstream_request_->headers()
                         .get(Http::LowerCaseString("request_body_size"))
                         ->value()
                         .c_str());

  Http::TestHeaderMapImpl response_headers{{":status", "200"}, {"foo", "bar"}};
  upstream_request_->encodeHeaders(default_response_headers_, false);
  Buffer::OwnedImpl response_data1("good");
  upstream_request_->encodeData(response_data1, false);
  Buffer::OwnedImpl response_data2("bye");
  upstream_request_->encodeData(response_data2, true);

  response_->waitForEndStream();

  EXPECT_STREQ(
      "7", response_->headers().get(Http::LowerCaseString("response_body_size"))->value().c_str());
  EXPECT_EQ(nullptr, response_->headers().get(Http::LowerCaseString("foo")));
}

} // namespace Envoy

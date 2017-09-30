#include "common/buffer/buffer_impl.h"
#include "common/http/filter/lua/lua_filter.h"

#include "test/mocks/http/mocks.h"
#include "test/mocks/thread_local/mocks.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"

using testing::InSequence;
using testing::Invoke;
using testing::StrEq;
using testing::_;

namespace Envoy {
namespace Http {
namespace Filter {
namespace Lua {

class TestFilter : public Filter {
public:
  using Filter::Filter;

  MOCK_METHOD2(scriptLog, void(int level, const char* message));
};

// fixfix body chunk out of loop test
// fixfix response body() test

class LuaHttpFilterTest : public testing::Test {
public:
  LuaHttpFilterTest() {
    ON_CALL(decoder_callbacks_, addDecodedData(_, _))
        .WillByDefault(Invoke([this](Buffer::Instance& data, bool) {
          if (decoder_callbacks_.buffer_ == nullptr) {
            decoder_callbacks_.buffer_.reset(new Buffer::OwnedImpl());
          }
          decoder_callbacks_.buffer_->move(data);
        }));
  }

  ~LuaHttpFilterTest() { filter_->onDestroy(); }

  void setup(const std::string& lua_code) {
    config_.reset(new FilterConfig(lua_code, tls_));
    filter_.reset(new TestFilter(config_));
    filter_->setDecoderFilterCallbacks(decoder_callbacks_);
    filter_->setEncoderFilterCallbacks(encoder_callbacks_);
  }

  NiceMock<ThreadLocal::MockInstance> tls_;
  std::shared_ptr<FilterConfig> config_;
  std::unique_ptr<TestFilter> filter_;
  NiceMock<MockStreamDecoderFilterCallbacks> decoder_callbacks_;
  MockStreamEncoderFilterCallbacks encoder_callbacks_;

  const std::string HEADER_ONLY_SCRIPT{R"EOF(
    function envoy_on_request(request_handle)
      request_handle:log(0, request_handle:headers():get(":path"))
    end
  )EOF"};

  const std::string BODY_CHUNK_SCRIPT{R"EOF(
    function envoy_on_request(request_handle)
      request_handle:log(0, request_handle:headers():get(":path"))

      for chunk in request_handle:bodyChunks() do
        request_handle:log(0, chunk:byteSize())
      end

      request_handle:log(0, "done")
    end
  )EOF"};

  const std::string TRAILERS_SCRIPT{R"EOF(
    function envoy_on_request(request_handle)
      request_handle:log(0, request_handle:headers():get(":path"))

      for chunk in request_handle:bodyChunks() do
        request_handle:log(0, chunk:byteSize())
      end

      local trailers = request_handle:trailers()
      if trailers ~= nil then
        request_handle:log(0, trailers:get("foo"))
      else
        request_handle:log(0, "no trailers")
      end
    end
  )EOF"};

  const std::string TRAILERS_NO_BODY_SCRIPT{R"EOF(
    function envoy_on_request(request_handle)
      request_handle:log(0, request_handle:headers():get(":path"))

      if request_handle:trailers() ~= nil then
        request_handle:log(0, request_handle:trailers():get("foo"))
      else
        request_handle:log(0, "no trailers")
      end
    end
  )EOF"};

  const std::string BODY_SCRIPT{R"EOF(
    function envoy_on_request(request_handle)
      request_handle:log(0, request_handle:headers():get(":path"))

      if request_handle:body() ~= nil then
        request_handle:log(0, request_handle:body():byteSize())
      else
        request_handle:log(0, "no body")
      end
    end
  )EOF"};

  const std::string BODY_TRAILERS_SCRIPT{R"EOF(
    function envoy_on_request(request_handle)
      request_handle:log(0, request_handle:headers():get(":path"))

      if request_handle:body() ~= nil then
        request_handle:log(0, request_handle:body():byteSize())
      else
        request_handle:log(0, "no body")
      end

      if request_handle:trailers() ~= nil then
        request_handle:log(0, request_handle:trailers():get("foo"))
      else
        request_handle:log(0, "no trailers")
      end
    end
  )EOF"};
};

// Bad code in initial config.
TEST(LuaHttpFilterConfigTest, BadCode) {
  const std::string SCRIPT{R"EOF(
    bad
  )EOF"};

  NiceMock<ThreadLocal::MockInstance> tls;
  EXPECT_THROW_WITH_MESSAGE(FilterConfig(SCRIPT, tls), Envoy::Lua::LuaException,
                            "script load error: [string \"...\"]:3: '=' expected near '<eof>'");
}

// Script touching headers only, request that is headers only.
TEST_F(LuaHttpFilterTest, ScriptHeadersOnlyRequestHeadersOnly) {
  InSequence s;
  setup(HEADER_ONLY_SCRIPT);

  TestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("/")));
  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
}

// Script touching headers only, request that has body.
TEST_F(LuaHttpFilterTest, ScriptHeadersOnlyRequestBody) {
  InSequence s;
  setup(HEADER_ONLY_SCRIPT);

  TestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("/")));
  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));

  Buffer::OwnedImpl data("hello");
  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data, true));
}

// Script touching headers only, request that has body and trailers.
TEST_F(LuaHttpFilterTest, ScriptHeadersOnlyRequestBodyTrailers) {
  InSequence s;
  setup(HEADER_ONLY_SCRIPT);

  TestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("/")));
  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));

  Buffer::OwnedImpl data("hello");
  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data, false));

  TestHeaderMapImpl request_trailers{{"foo", "bar"}};
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers));
}

// Script asking for body chunks, request that is headers only.
TEST_F(LuaHttpFilterTest, ScriptBodyChunksRequestHeadersOnly) {
  InSequence s;
  setup(BODY_CHUNK_SCRIPT);

  TestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("/")));
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("done")));
  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
}

// Script asking for body chunks, request that has body.
TEST_F(LuaHttpFilterTest, ScriptBodyChunksRequestBody) {
  InSequence s;
  setup(BODY_CHUNK_SCRIPT);

  TestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("/")));
  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));

  Buffer::OwnedImpl data("hello");
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("5")));
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("done")));
  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data, true));
}

// Script asking for body chunks, request that has body and trailers.
TEST_F(LuaHttpFilterTest, ScriptBodyChunksRequestBodyTrailers) {
  InSequence s;
  setup(BODY_CHUNK_SCRIPT);

  TestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("/")));
  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));

  Buffer::OwnedImpl data("hello");
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("5")));
  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data, false));

  TestHeaderMapImpl request_trailers{{"foo", "bar"}};
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("done")));
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers));
}

// Script asking for trailers, request is headers only.
TEST_F(LuaHttpFilterTest, ScriptTrailersRequestHeadersOnly) {
  InSequence s;
  setup(TRAILERS_SCRIPT);

  TestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("/")));
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("no trailers")));
  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
}

// Script asking for trailers, request that has a body.
TEST_F(LuaHttpFilterTest, ScriptTrailersRequestBody) {
  InSequence s;
  setup(TRAILERS_SCRIPT);

  TestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("/")));
  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));

  Buffer::OwnedImpl data("hello");
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("5")));
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("no trailers")));
  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data, true));
}

// Script asking for trailers, request that has body and trailers.
TEST_F(LuaHttpFilterTest, ScriptTrailersRequestBodyTrailers) {
  InSequence s;
  setup(TRAILERS_SCRIPT);

  TestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("/")));
  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));

  Buffer::OwnedImpl data("hello");
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("5")));
  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data, false));

  TestHeaderMapImpl request_trailers{{"foo", "bar"}};
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("bar")));
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers));
}

// Script asking for trailers without body, request is headers only.
TEST_F(LuaHttpFilterTest, ScriptTrailersNoBodyRequestHeadersOnly) {
  InSequence s;
  setup(TRAILERS_NO_BODY_SCRIPT);

  TestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("/")));
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("no trailers")));
  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
}

// Script asking for trailers without body, request that has a body.
TEST_F(LuaHttpFilterTest, ScriptTrailersNoBodyRequestBody) {
  InSequence s;
  setup(TRAILERS_NO_BODY_SCRIPT);

  TestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("/")));
  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));

  Buffer::OwnedImpl data("hello");
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("no trailers")));
  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data, true));
}

// Script asking for trailers without body, request that has a body and trailers.
TEST_F(LuaHttpFilterTest, ScriptTrailersNoBodyRequestBodyTrailers) {
  InSequence s;
  setup(TRAILERS_NO_BODY_SCRIPT);

  TestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("/")));
  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));

  Buffer::OwnedImpl data("hello");
  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data, false));

  TestHeaderMapImpl request_trailers{{"foo", "bar"}};
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("bar")));
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers));
}

// Script asking for blocking body, request that is headers only.
TEST_F(LuaHttpFilterTest, ScriptBodyRequestHeadersOnly) {
  InSequence s;
  setup(BODY_SCRIPT);

  TestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("/")));
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("no body")));
  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
}

// Script asking for blocking body, request that has a body.
TEST_F(LuaHttpFilterTest, ScriptBodyRequestBody) {
  InSequence s;
  setup(BODY_SCRIPT);

  TestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("/")));
  EXPECT_EQ(FilterHeadersStatus::StopIteration, filter_->decodeHeaders(request_headers, false));

  Buffer::OwnedImpl data("hello");
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("5")));
  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data, true));
}

// Script asking for blocking body, request that has a body in multiple frames.
TEST_F(LuaHttpFilterTest, ScriptBodyRequestBodyTwoFrames) {
  InSequence s;
  setup(BODY_SCRIPT);

  TestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("/")));
  EXPECT_EQ(FilterHeadersStatus::StopIteration, filter_->decodeHeaders(request_headers, false));

  Buffer::OwnedImpl data("hello");
  EXPECT_EQ(FilterDataStatus::StopIterationAndBuffer, filter_->decodeData(data, false));
  decoder_callbacks_.addDecodedData(data, false);

  Buffer::OwnedImpl data2("world");
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("10")));
  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data2, true));
}

// Scripting asking for blocking body, request that has a body in multiple frames follows by
// trailers.
TEST_F(LuaHttpFilterTest, ScriptBodyRequestBodyTwoFramesTrailers) {
  InSequence s;
  setup(BODY_SCRIPT);

  TestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("/")));
  EXPECT_EQ(FilterHeadersStatus::StopIteration, filter_->decodeHeaders(request_headers, false));

  Buffer::OwnedImpl data("hello");
  EXPECT_EQ(FilterDataStatus::StopIterationAndBuffer, filter_->decodeData(data, false));
  decoder_callbacks_.addDecodedData(data, false);

  Buffer::OwnedImpl data2("world");
  EXPECT_EQ(FilterDataStatus::StopIterationAndBuffer, filter_->decodeData(data2, false));
  decoder_callbacks_.addDecodedData(data2, false);

  TestHeaderMapImpl request_trailers{{"foo", "bar"}};
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("10")));
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers));
}

// Script asking for blocking body and trailers, request that is headers only.
TEST_F(LuaHttpFilterTest, ScriptBodyTrailersRequestHeadersOnly) {
  InSequence s;
  setup(BODY_TRAILERS_SCRIPT);

  TestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("/")));
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("no body")));
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("no trailers")));
  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
}

// Script asking for blocking body and trailers, request that has a body.
TEST_F(LuaHttpFilterTest, ScriptBodyTrailersRequestBody) {
  InSequence s;
  setup(BODY_TRAILERS_SCRIPT);

  TestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("/")));
  EXPECT_EQ(FilterHeadersStatus::StopIteration, filter_->decodeHeaders(request_headers, false));

  Buffer::OwnedImpl data("hello");
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("5")));
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("no trailers")));
  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data, true));
}

// Script asking for blocking body and trailers, request that has a body and trailers.
TEST_F(LuaHttpFilterTest, ScriptBodyTrailersRequestBodyTrailers) {
  InSequence s;
  setup(BODY_TRAILERS_SCRIPT);

  TestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("/")));
  EXPECT_EQ(FilterHeadersStatus::StopIteration, filter_->decodeHeaders(request_headers, false));

  Buffer::OwnedImpl data("hello");
  EXPECT_EQ(FilterDataStatus::StopIterationAndBuffer, filter_->decodeData(data, false));
  decoder_callbacks_.addDecodedData(data, false);

  TestHeaderMapImpl request_trailers{{"foo", "bar"}};
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("5")));
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("bar")));
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers));
}

// Script that should not be run.
TEST_F(LuaHttpFilterTest, ScriptRandomRequestBodyTrailers) {
  const std::string SCRIPT{R"EOF(
    function some_random_function()
      print("don't run me")
    end
  )EOF"};

  InSequence s;
  setup(SCRIPT);

  TestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));

  Buffer::OwnedImpl data("hello");
  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data, false));

  TestHeaderMapImpl request_trailers{{"foo", "bar"}};
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers));
}

// Script that has an error during headers processing.
TEST_F(LuaHttpFilterTest, ScriptErrorHeadersRequestBodyTrailers) {
  const std::string SCRIPT{R"EOF(
    function envoy_on_request(request_handle)
      local foo = nil
      foo["bar"] = "baz"
    end
  )EOF"};

  InSequence s;
  setup(SCRIPT);

  TestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_CALL(
      *filter_,
      scriptLog(2, StrEq("[string \"...\"]:4: attempt to index local 'foo' (a nil value)")));
  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));

  Buffer::OwnedImpl data("hello");
  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data, false));

  TestHeaderMapImpl request_trailers{{"foo", "bar"}};
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers));
}

// Script that tries to store a local variable to a global and then use it.
TEST_F(LuaHttpFilterTest, ThreadEnvironments) {
  const std::string SCRIPT{R"EOF(
    function envoy_on_request(request_handle)
      if global_request_handle == nil then
        global_request_handle = request_handle
      else
        global_request_handle:log(0, "should not work")
      end
    end
  )EOF"};

  InSequence s;
  setup(SCRIPT);

  TestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));

  TestFilter filter2(config_);
  EXPECT_CALL(filter2,
              scriptLog(2, StrEq("[string \"...\"]:6: object used outside of proper scope")));
  filter2.decodeHeaders(request_headers, true);
}

// Script that yields on its own.
TEST_F(LuaHttpFilterTest, UnexpectedYield) {
  const std::string SCRIPT{R"EOF(
    function envoy_on_request(request_handle)
      coroutine.yield()
    end
  )EOF"};

  InSequence s;
  setup(SCRIPT);

  TestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_CALL(*filter_, scriptLog(2, StrEq("script performed an unexpected yield")));
  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
}

// Script that has an error during a callback from C into Lua.
TEST_F(LuaHttpFilterTest, ErrorDuringCallback) {
  const std::string SCRIPT(R"EOF(
    function envoy_on_request(request_handle)
      request_handle:headers():iterate(
        function(key, value)
          local foo = nil
          foo["bar"] = "baz"
        end
      )
    end
  )EOF");

  InSequence s;
  setup(SCRIPT);

  TestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_CALL(
      *filter_,
      scriptLog(2, StrEq("[string \"...\"]:6: attempt to index local 'foo' (a nil value)")));
  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
}

// Combo request and response script.
TEST_F(LuaHttpFilterTest, RequestAndResponse) {
  const std::string SCRIPT{R"EOF(
    function envoy_on_request(request_handle)
      request_handle:log(0, request_handle:headers():get(":path"))

      for chunk in request_handle:bodyChunks() do
        request_handle:log(0, chunk:byteSize())
      end

      request_handle:log(0, request_handle:trailers():get("foo"))
    end

    function envoy_on_response(response_handle)
      response_handle:log(0, response_handle:headers():get(":status"))

      for chunk in response_handle:bodyChunks() do
        response_handle:log(0, chunk:byteSize())
      end

      response_handle:log(0, response_handle:trailers():get("hello"))
    end
  )EOF"};

  InSequence s;
  setup(SCRIPT);

  TestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("/")));
  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));

  Buffer::OwnedImpl data("hello");
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("5")));
  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data, false));

  TestHeaderMapImpl request_trailers{{"foo", "bar"}};
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("bar")));
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers));

  TestHeaderMapImpl response_headers{{":status", "200"}};
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("200")));
  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, false));

  Buffer::OwnedImpl data2("helloworld");
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("10")));
  EXPECT_EQ(FilterDataStatus::Continue, filter_->encodeData(data2, false));

  TestHeaderMapImpl response_trailers{{"hello", "world"}};
  EXPECT_CALL(*filter_, scriptLog(0, StrEq("world")));
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers));
}

} // namespace Lua
} // namespace Filter
} // namespace Http
} // namespace Envoy

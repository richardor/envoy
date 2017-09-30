#pragma once

#include "envoy/http/filter.h"

#include "common/http/filter/lua/wrappers.h"
#include "common/lua/wrappers.h"

namespace Envoy {
namespace Http {
namespace Filter {
namespace Lua {

/**
 * Interface used for script logging. This is primarily used during unit testing so observe
 * script behavior.
 */
class ScriptLogger {
public:
  virtual ~ScriptLogger() {}

  /**
   * Log a message.
   * @param level supplies the log level.
   * @param message supplies the message.
   */
  virtual void scriptLog(int level, const char* message) PURE;
};

/**
 * Callbacks used by a a strem handler to access the filter.
 */
class FilterCallbacks {
public:
  virtual ~FilterCallbacks() {}

  /**
   * @return ScriptLogger& the logger.
   */
  virtual ScriptLogger& logger() PURE;

  /**
   * Add data to the connection manager buffer.
   * @param data supplies the data to add.
   */
  virtual void addData(Buffer::Instance& data) PURE;

  /**
   * @return const Buffer::Instance* the currently buffered body.
   */
  virtual const Buffer::Instance* bufferedBody() PURE;
};

/**
 * A wrapper for a currently running request/response. This is the primary handle passed to Lua.
 * The script interacts with Envoy entirely through this handle.
 */
class StreamHandleWrapper : public Envoy::Lua::BaseLuaObject<StreamHandleWrapper> {
public:
  enum class State { Running, WaitForBodyChunk, WaitForBody, WaitForTrailers };

  StreamHandleWrapper(Envoy::Lua::CoroutinePtr&& coroutine, HeaderMap& headers, bool end_stream,
                      FilterCallbacks& callbacks);

  FilterHeadersStatus start(int function_ref);
  FilterDataStatus onData(Buffer::Instance& data, bool end_stream);
  void onTrailers(HeaderMap& trailers);

  static ExportedFunctions exportedFunctions() {
    return {{"headers", static_luaHeaders},
            {"body", static_luaBody},
            {"bodyChunks", static_luaBodyChunks},
            {"trailers", static_luaTrailers},
            {"log", static_luaLog},
            {"httpCall", static_luaHttpCall}};
  }

private:
  /**
   *
   */
  DECLARE_LUA_FUNCTION(StreamHandleWrapper, luaHttpCall);

  /**
   * @return a handle to the headers.
   */
  DECLARE_LUA_FUNCTION(StreamHandleWrapper, luaHeaders);

  /**
   * @return a handle to the full body or nil if there is no body. This call will cause the script
   *         to yield until the entire body is received (or if there is no body will return nil
   *         right away).
   *         NOTE: This call causes Envoy to buffer the body. The max buffer size is configured
   *         based on the currently active flow control settings.
   */
  DECLARE_LUA_FUNCTION(StreamHandleWrapper, luaBody);

  /**
   * @return an iterator that allows the script to iterate through all body chunks as they are
   *         received. The iterator will yield between body chunks. Envoy *will not* buffer
   *         the body chunks in this case, but the script can look at them as they go by.
   */
  DECLARE_LUA_FUNCTION(StreamHandleWrapper, luaBodyChunks);

  /**
   * @return a handle to the trailers or nil if there are no trailers. This call will cause the
   *         script to yield of Envoy does not yet know if there are trailers or not.
   */
  DECLARE_LUA_FUNCTION(StreamHandleWrapper, luaTrailers);

  /**
   * Log a message to the Envoy log.
   * @param 1 (int): The log level.
   * @param 2 (string): The log message.
   */
  DECLARE_LUA_FUNCTION(StreamHandleWrapper, luaLog);

  /**
   * This is the closure/iterator returned by luaBodyChunks() above.
   */
  DECLARE_LUA_CLOSURE(StreamHandleWrapper, luaBodyIterator);

  // Envoy::Lua::BaseLuaObject
  void onMarkDead() override {
    headers_wrapper_.markDead();
    body_wrapper_.markDead();
    trailers_wrapper_.markDead();
  }

  void onMarkLive() override {
    headers_wrapper_.markLive();
    body_wrapper_.markLive();
    trailers_wrapper_.markLive();
  }

  Envoy::Lua::CoroutinePtr coroutine_;
  HeaderMap& headers_;
  bool end_stream_;
  FilterCallbacks& callbacks_;
  HeaderMap* trailers_{};
  Envoy::Lua::LuaDeathRef<HeaderMapWrapper> headers_wrapper_;
  Envoy::Lua::LuaDeathRef<Envoy::Lua::BufferWrapper> body_wrapper_;
  Envoy::Lua::LuaDeathRef<HeaderMapWrapper> trailers_wrapper_;
  State state_{State::Running};
  std::function<void()> yield_callback_;
};

/**
 * Global configuration for the filter.
 */
class FilterConfig : Logger::Loggable<Logger::Id::lua> {
public:
  FilterConfig(const std::string& lua_code, ThreadLocal::SlotAllocator& tls);
  Envoy::Lua::CoroutinePtr createCoroutine() { return lua_state_.createCoroutine(); }
  int requestFunctionRef() { return lua_state_.getGlobalRef(request_function_slot_); }
  int responseFunctionRef() { return lua_state_.getGlobalRef(response_function_slot_); }

private:
  Envoy::Lua::ThreadLocalState lua_state_;
  uint64_t request_function_slot_;
  uint64_t response_function_slot_;
};

typedef std::shared_ptr<FilterConfig> FilterConfigConstSharedPtr;

/**
 * The HTTP Lua filter. Allows scripts to run in both the request an response flow.
 */
class Filter : public StreamFilter, public ScriptLogger, Logger::Loggable<Logger::Id::lua> {
public:
  Filter(FilterConfigConstSharedPtr config) : config_(config) {}

  // Http::StreamFilterBase
  void onDestroy() override {}

  // Http::StreamDecoderFilter
  FilterHeadersStatus decodeHeaders(HeaderMap& headers, bool end_stream) override {
    return doHeaders(request_stream_wrapper_, decoder_callbacks_, config_->requestFunctionRef(),
                     headers, end_stream);
  }
  FilterDataStatus decodeData(Buffer::Instance& data, bool end_stream) override {
    return doData(request_stream_wrapper_, data, end_stream);
  }
  FilterTrailersStatus decodeTrailers(HeaderMap& trailers) override {
    return doTrailers(request_stream_wrapper_, trailers);
  }
  void setDecoderFilterCallbacks(StreamDecoderFilterCallbacks& callbacks) override {
    decoder_callbacks_.callbacks_ = &callbacks;
  }

  // Http::StreamEncoderFilter
  FilterHeadersStatus encodeHeaders(HeaderMap& headers, bool end_stream) override {
    return doHeaders(response_stream_wrapper_, encoder_callbacks_, config_->responseFunctionRef(),
                     headers, end_stream);
  }
  FilterDataStatus encodeData(Buffer::Instance& data, bool end_stream) override {
    return doData(response_stream_wrapper_, data, end_stream);
  };
  FilterTrailersStatus encodeTrailers(HeaderMap& trailers) override {
    return doTrailers(response_stream_wrapper_, trailers);
  };
  void setEncoderFilterCallbacks(StreamEncoderFilterCallbacks& callbacks) override {
    encoder_callbacks_.callbacks_ = &callbacks;
  };

private:
  struct DecoderCallbacks : public FilterCallbacks {
    DecoderCallbacks(Filter& parent) : parent_(parent) {}

    // FilterCallbacks
    ScriptLogger& logger() override { return parent_; }
    void addData(Buffer::Instance& data) override {
      return callbacks_->addDecodedData(data, false);
    }
    const Buffer::Instance* bufferedBody() override { return callbacks_->decodingBuffer(); }

    Filter& parent_;
    StreamDecoderFilterCallbacks* callbacks_{};
  };

  struct EncoderCallbacks : public FilterCallbacks {
    EncoderCallbacks(Filter& parent) : parent_(parent) {}

    // FilterCallbacks
    ScriptLogger& logger() override { return parent_; }
    void addData(Buffer::Instance& data) override {
      return callbacks_->addEncodedData(data, false);
    }
    const Buffer::Instance* bufferedBody() override { return callbacks_->encodingBuffer(); }

    Filter& parent_;
    StreamEncoderFilterCallbacks* callbacks_{};
  };

  typedef Envoy::Lua::LuaDeathRef<StreamHandleWrapper> StreamHandleRef;

  FilterHeadersStatus doHeaders(StreamHandleRef& handle, FilterCallbacks& callbacks,
                                int function_ref, HeaderMap& headers, bool end_stream);
  FilterDataStatus doData(StreamHandleRef& handle, Buffer::Instance& data, bool end_stream);
  FilterTrailersStatus doTrailers(StreamHandleRef& handle, HeaderMap& trailers);
  void handleScriptError(const Envoy::Lua::LuaException& e);

  // ScriptLogger
  void scriptLog(int level, const char* message) override;

  FilterConfigConstSharedPtr config_;
  DecoderCallbacks decoder_callbacks_{*this};
  EncoderCallbacks encoder_callbacks_{*this};
  StreamHandleRef request_stream_wrapper_;
  StreamHandleRef response_stream_wrapper_;
};

} // namespace Lua
} // namespace Filter
} // namespace Http
} // namespace Envoy

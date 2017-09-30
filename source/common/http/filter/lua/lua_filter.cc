#include "common/http/filter/lua/lua_filter.h"

#include "common/buffer/buffer_impl.h"
#include "common/common/assert.h"

namespace Envoy {
namespace Http {
namespace Filter {
namespace Lua {

StreamHandleWrapper::StreamHandleWrapper(Envoy::Lua::CoroutinePtr&& coroutine, HeaderMap& headers,
                                         bool end_stream, FilterCallbacks& callbacks)
    : coroutine_(std::move(coroutine)), headers_(headers), end_stream_(end_stream),
      callbacks_(callbacks), yield_callback_([this]() {
        if (state_ == State::Running) {
          throw Envoy::Lua::LuaException("script performed an unexpected yield");
        }
      }) {}

FilterHeadersStatus StreamHandleWrapper::start(int function_ref) {
  // We are on the top of the stack.
  coroutine_->start(function_ref, 1, yield_callback_);
  return state_ == State::WaitForBody ? FilterHeadersStatus::StopIteration
                                      : FilterHeadersStatus::Continue;
}

FilterDataStatus StreamHandleWrapper::onData(Buffer::Instance& data, bool end_stream) {
  ASSERT(!end_stream_);
  end_stream_ = end_stream;

  if (state_ == State::WaitForBodyChunk) {
    ENVOY_LOG(debug, "resuming for next body chunk");
    Envoy::Lua::LuaDeathRef<Envoy::Lua::BufferWrapper> wrapper(
        Envoy::Lua::BufferWrapper::create(coroutine_->luaState(), data), true);
    state_ = State::Running;
    coroutine_->resume(1, yield_callback_);
  } else if (state_ == State::WaitForBody && end_stream_) {
    ENVOY_LOG(debug, "resuming body due to end stream");
    callbacks_.addData(data);
    state_ = State::Running;
    coroutine_->resume(luaBody(coroutine_->luaState()), yield_callback_);
  } else if (state_ == State::WaitForBody && !end_stream_) {
    ENVOY_LOG(debug, "buffering body");
    return FilterDataStatus::StopIterationAndBuffer;
  } else if (state_ == State::WaitForTrailers && end_stream_) {
    ENVOY_LOG(debug, "resuming nil trailers due to end stream");
    state_ = State::Running;
    coroutine_->resume(0, yield_callback_);
  }

  return FilterDataStatus::Continue;
}

void StreamHandleWrapper::onTrailers(HeaderMap& trailers) {
  ASSERT(!end_stream_);
  end_stream_ = true;
  trailers_ = &trailers;

  if (state_ == State::WaitForBodyChunk) {
    ENVOY_LOG(debug, "resuming nil body chunk due to trailers");
    state_ = State::Running;
    coroutine_->resume(0, yield_callback_);
  } else if (state_ == State::WaitForBody) {
    ENVOY_LOG(debug, "resuming body due to trailers");
    state_ = State::Running;
    coroutine_->resume(luaBody(coroutine_->luaState()), yield_callback_);
  }

  if (state_ == State::WaitForTrailers) {
    // Mimic a call to trailers which will push the trailers onto the stack and then resume.
    state_ = State::Running;
    coroutine_->resume(luaTrailers(coroutine_->luaState()), yield_callback_);
  }
}

int StreamHandleWrapper::luaHttpCall(lua_State*) {
  ASSERT(false); // fixfix
  return 0;
}

int StreamHandleWrapper::luaHeaders(lua_State* state) {
  // fixfix don't allow modification after headers are continued.
  if (headers_wrapper_.get() != nullptr) {
    headers_wrapper_.pushStack();
  } else {
    headers_wrapper_.reset(HeaderMapWrapper::create(state, headers_), true);
  }
  return 1;
}

int StreamHandleWrapper::luaBody(lua_State* state) {
  // fixfix fail if body was not buffered.
  ASSERT(state_ == State::Running);

  if (end_stream_) {
    if (callbacks_.bufferedBody() == nullptr) {
      ENVOY_LOG(debug, "end stream. no body");
      return 0;
    } else {
      if (body_wrapper_.get() != nullptr) {
        body_wrapper_.pushStack();
      } else {
        body_wrapper_.reset(Envoy::Lua::BufferWrapper::create(state, *callbacks_.bufferedBody()),
                            true);
      }
      return 1;
    }
  } else {
    ENVOY_LOG(debug, "yielding for full body");
    state_ = State::WaitForBody;
    return lua_yield(state, 0);
  }
}

int StreamHandleWrapper::luaBodyChunks(lua_State* state) {
  ASSERT(state_ == State::Running);

  // We are currently at the top of the stack. Push a closure that has us as the upvalue.
  lua_pushcclosure(state, static_luaBodyIterator, 1);
  return 1;
}

int StreamHandleWrapper::luaBodyIterator(lua_State* state) {
  ASSERT(state_ == State::Running);

  if (end_stream_) {
    ENVOY_LOG(debug, "body complete. no more body chunks");
    return 0;
  } else {
    ENVOY_LOG(debug, "yielding for next body chunk");
    state_ = State::WaitForBodyChunk;
    return lua_yield(state, 0);
  }
}

int StreamHandleWrapper::luaTrailers(lua_State* state) {
  ASSERT(state_ == State::Running);

  if (end_stream_ && trailers_ == nullptr) {
    ENVOY_LOG(debug, "end stream. no trailers");
    return 0;
  } else if (trailers_ != nullptr) {
    if (trailers_wrapper_.get() != nullptr) {
      trailers_wrapper_.pushStack();
    } else {
      trailers_wrapper_.reset(HeaderMapWrapper::create(state, *trailers_), true);
    }
    return 1;
  } else {
    ENVOY_LOG(debug, "yielding for trailers");
    state_ = State::WaitForTrailers;
    return lua_yield(state, 0);
  }
}

int StreamHandleWrapper::luaLog(lua_State* state) {
  int level = luaL_checkint(state, 2);
  const char* message = luaL_checkstring(state, 3);
  callbacks_.logger().scriptLog(level, message);
  return 0;
}

FilterConfig::FilterConfig(const std::string& lua_code, ThreadLocal::SlotAllocator& tls)
    : lua_state_(lua_code, tls) {
  lua_state_.registerType<Envoy::Lua::BufferWrapper>();
  lua_state_.registerType<HeaderMapWrapper>();
  lua_state_.registerType<StreamHandleWrapper>();

  request_function_slot_ = lua_state_.registerGlobal("envoy_on_request");
  response_function_slot_ = lua_state_.registerGlobal("envoy_on_response");
}

FilterHeadersStatus Filter::doHeaders(StreamHandleRef& handle, FilterCallbacks& callbacks,
                                      int function_ref, HeaderMap& headers, bool end_stream) {
  if (function_ref == LUA_REFNIL) {
    return FilterHeadersStatus::Continue;
  }

  Envoy::Lua::CoroutinePtr coroutine = config_->createCoroutine();
  handle.reset(StreamHandleWrapper::create(coroutine->luaState(), std::move(coroutine), headers,
                                           end_stream, callbacks),
               true);

  try {
    FilterHeadersStatus status = handle.get()->start(function_ref);
    handle.markDead();
    return status;
  } catch (const Envoy::Lua::LuaException& e) {
    handleScriptError(e);
  }

  return FilterHeadersStatus::Continue;
}

FilterDataStatus Filter::doData(StreamHandleRef& handle, Buffer::Instance& data, bool end_stream) {
  if (handle.get() != nullptr) {
    try {
      handle.markLive();
      FilterDataStatus status = handle.get()->onData(data, end_stream);
      handle.markDead();
      return status;
    } catch (const Envoy::Lua::LuaException& e) {
      handleScriptError(e);
    }
  }

  return FilterDataStatus::Continue;
}

FilterTrailersStatus Filter::doTrailers(StreamHandleRef& handle, HeaderMap& trailers) {
  if (handle.get() != nullptr) {
    try {
      handle.markLive();
      handle.get()->onTrailers(trailers);
      handle.markDead();
    } catch (const Envoy::Lua::LuaException& e) {
      handleScriptError(e);
    }
  }

  return FilterTrailersStatus::Continue;
}

void Filter::handleScriptError(const Envoy::Lua::LuaException& e) {
  scriptLog(2, e.what());
  request_stream_wrapper_.reset();
  response_stream_wrapper_.reset();
}

void Filter::scriptLog(int level, const char* message) {
  // fixfix levels
  switch (level) {
  default: {
    ENVOY_LOG(debug, "script log: {}", message);
    break;
  }
  }
}

} // namespace Lua
} // namespace Filter
} // namespace Http
} // namespace Envoy

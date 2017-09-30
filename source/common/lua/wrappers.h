#pragma once

#include "envoy/buffer/buffer.h"

#include "common/lua/lua.h"

// fixfix tests

namespace Envoy {
namespace Lua {

/**
 * A wrapper for a constant buffer which cannot be modified by Lua.
 */
class BufferWrapper : public BaseLuaObject<BufferWrapper> {
public:
  BufferWrapper(const Buffer::Instance& data) : data_(data) {}

  static ExportedFunctions exportedFunctions() { return {{"byteSize", static_luaByteSize}}; }

private:
  /**
   * @return int the size in bytes of the buffer.
   */
  DECLARE_LUA_FUNCTION(BufferWrapper, luaByteSize);

  const Buffer::Instance& data_;
};

} // namespace Lua
} // namespace Envoy

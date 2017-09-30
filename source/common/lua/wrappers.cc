#include "common/lua/wrappers.h"

namespace Envoy {
namespace Lua {

int BufferWrapper::luaByteSize(lua_State* state) {
  lua_pushnumber(state, data_.length());
  return 1;
}

} // namespace Lua
} // namespace Envoy

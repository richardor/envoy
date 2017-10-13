#include "server/config/http/lua.h"

#include "envoy/registry/registry.h"

#include "common/http/filter/lua/lua_filter.h"

namespace Envoy {
namespace Server {
namespace Configuration {

HttpFilterFactoryCb LuaFilterConfig::createFilterFactory(const Json::Object& json_config,
                                                         const std::string&,
                                                         FactoryContext& context) {
  // fixfix schema, load from file
  Http::Filter::Lua::FilterConfigConstSharedPtr config(new Http::Filter::Lua::FilterConfig{
      json_config.getString("inline_code"), context.threadLocal()});
  return [config](Http::FilterChainFactoryCallbacks& callbacks) -> void {
    callbacks.addStreamFilter(std::make_shared<Http::Filter::Lua::Filter>(config));
  };
}

/**
 * Static registration for the Lua filter. @see RegisterFactory.
 */
static Registry::RegisterFactory<LuaFilterConfig, NamedHttpFilterConfigFactory> register_;

} // namespace Configuration
} // namespace Server
} // namespace Envoy

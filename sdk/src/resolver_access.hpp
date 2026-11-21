#pragma once
#include <bedrock/sdk/resolver.hpp>
#include <bedrock/profiles/resolver.hpp>
#include <memory>
namespace bedrock::sdk { struct SymbolResolverRuntimeAccess { static const bedrock::Resolver& resolver(const SymbolResolver&); }; }

#pragma once

// Single include boundary for the JSON library. Amanuensis
// (DeanWilsonDev/amanuensis) is Prism's first-party JSON serialiser and must
// not be included anywhere else outside exporter.cpp — keeping it here means
// the rest of the codebase stays free of any JSON-library types.
#include <amanuensis.hpp>

#include <string>
#include <utility>

#include "ast-node.hpp"

namespace Prism {

inline Amanuensis::JsonValue ToJsonValue(std::string text)
{
  return Amanuensis::JsonValue{std::move(text)};
}

inline Amanuensis::JsonValue ToJsonValue(bool flag)
{
  return Amanuensis::JsonValue{flag};
}

inline Amanuensis::JsonValue ToJsonValue(int number)
{
  return Amanuensis::JsonValue{static_cast<long long>(number)};
}

inline Amanuensis::JsonValue ToJsonValue(double number)
{
  return Amanuensis::JsonValue{number};
}

/// NodeKind rendered as the string form used in analysis.json, wrapped as an
/// Amanuensis value. Mirrors ToStringFromNodeKind() so both paths agree.
inline Amanuensis::JsonValue NodeKindToValue(NodeKind kind)
{
  return ToJsonValue(std::string(ToStringFromNodeKind(kind)));
}

}  // namespace Prism

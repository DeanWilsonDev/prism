#pragma once

// Single include boundary for the JSON library. Amanuensis
// (DeanWilsonDev/amanuensis) is Prism's first-party JSON serialiser and must
// not be included anywhere else outside exporter.cpp — keeping it here means
// the rest of the codebase stays free of any JSON-library types.
#include <amanuensis.hpp>

#include "ast-node.hpp"

namespace Prism {

/// NodeKind rendered as the string form used in analysis.json, wrapped as an
/// Amanuensis value. Mirrors ToStringFromNodeKind() so both paths agree.
inline Amanuensis::Value NodeKindToValue(NodeKind kind)
{
  return Amanuensis::Value(std::string(ToStringFromNodeKind(kind)));
}

}  // namespace Prism

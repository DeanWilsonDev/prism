#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "graph-node.hpp"

namespace Prism {

enum EdgeKind { IncludeDependency, Inheritance, SymbolUsage, FunctionCall, Composition };

inline constexpr std::pair<EdgeKind, std::string_view> edgeKindNames[] = {
    {EdgeKind::IncludeDependency, "include_dependency"},
    {EdgeKind::Inheritance, "inheritance"},
    {EdgeKind::SymbolUsage, "symbol_usage"},
    {EdgeKind::FunctionCall, "function_call"},
    {EdgeKind::Composition, "composition"},
};

inline std::string_view ToStringFromEdgeKind(EdgeKind kind)
{
  for (const auto& [enumValue, name] : edgeKindNames) {
    if (enumValue == kind) {
      return name;
    }
  }
  return "unknown";
}

struct Edge {
 public:
  std::string sourceId;
  std::string targetId;
  EdgeKind kind;
  int weight = 1;
};

/// Central data model passed between the graph builder, metrics engine, and
/// exporter. Owns the full set of nodes and edges for a project.
class DependencyGraph {
 public:
  void AddNode(GraphNode node);
  void AddEdge(Edge edge);

  GraphNode* FindNodeById(const std::string& id);
  const GraphNode* FindNodeById(const std::string& id) const;

  std::vector<Edge> GetOutgoingEdges(const std::string& nodeId) const;
  std::vector<Edge> GetIncomingEdges(const std::string& nodeId) const;

  std::vector<GraphNode> nodes;
  std::vector<Edge> edges;
};

}  // namespace Prism

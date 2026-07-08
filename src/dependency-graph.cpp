#include "prism/dependency-graph.hpp"

namespace Prism {

void DependencyGraph::AddNode(GraphNode node)
{
  nodes.push_back(std::move(node));
}

void DependencyGraph::AddEdge(Edge edge)
{
  edges.push_back(std::move(edge));
}

GraphNode* DependencyGraph::FindNodeById(const std::string& id)
{
  for (GraphNode& node : nodes) {
    if (node.id == id) {
      return &node;
    }
  }
  return nullptr;
}

const GraphNode* DependencyGraph::FindNodeById(const std::string& id) const
{
  for (const GraphNode& node : nodes) {
    if (node.id == id) {
      return &node;
    }
  }
  return nullptr;
}

std::vector<Edge> DependencyGraph::GetOutgoingEdges(const std::string& nodeId) const
{
  std::vector<Edge> result;
  for (const Edge& edge : edges) {
    if (edge.sourceId == nodeId) {
      result.push_back(edge);
    }
  }
  return result;
}

std::vector<Edge> DependencyGraph::GetIncomingEdges(const std::string& nodeId) const
{
  std::vector<Edge> result;
  for (const Edge& edge : edges) {
    if (edge.targetId == nodeId) {
      result.push_back(edge);
    }
  }
  return result;
}

}  // namespace Prism

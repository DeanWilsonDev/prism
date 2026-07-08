#include "prism/metrics-engine.hpp"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Prism {

void MetricsEngine::Annotate(DependencyGraph& graph)
{
  ComputeStructuralMetrics(graph);
  ComputeCodeMetrics(graph);
  ComputeCppMetrics(graph);
}

namespace {

/// Nodes that participate in any directed cycle, found via Tarjan's SCC
/// algorithm. A node is on a cycle if its SCC has more than one member or it
/// has a self-loop.
std::unordered_set<std::string> FindCyclicNodes(const DependencyGraph& graph)
{
  std::unordered_map<std::string, std::vector<std::string>> adjacency;
  std::unordered_set<std::string> selfLoops;
  for (const GraphNode& node : graph.nodes) {
    adjacency.try_emplace(node.id);
  }
  for (const Edge& edge : graph.edges) {
    if (adjacency.count(edge.sourceId) == 0) {
      continue;
    }
    adjacency[edge.sourceId].push_back(edge.targetId);
    if (edge.sourceId == edge.targetId) {
      selfLoops.insert(edge.sourceId);
    }
  }

  std::unordered_map<std::string, int> indexOf;
  std::unordered_map<std::string, int> lowLink;
  std::unordered_set<std::string> onStack;
  std::vector<std::string> stack;
  std::unordered_set<std::string> cyclic = selfLoops;
  int counter = 0;

  // Iterative Tarjan to avoid deep recursion on large graphs.
  for (const GraphNode& start : graph.nodes) {
    if (indexOf.count(start.id) != 0) {
      continue;
    }
    std::vector<std::pair<std::string, std::size_t>> frames;
    frames.emplace_back(start.id, 0);
    while (!frames.empty()) {
      auto& [nodeId, childIndex] = frames.back();
      if (childIndex == 0) {
        indexOf[nodeId] = counter;
        lowLink[nodeId] = counter;
        ++counter;
        stack.push_back(nodeId);
        onStack.insert(nodeId);
      }
      const std::vector<std::string>& neighbours = adjacency[nodeId];
      if (childIndex < neighbours.size()) {
        const std::string neighbour = neighbours[childIndex];
        ++childIndex;
        if (indexOf.count(neighbour) == 0) {
          frames.emplace_back(neighbour, 0);
        }
        else if (onStack.count(neighbour) != 0) {
          lowLink[nodeId] = std::min(lowLink[nodeId], indexOf[neighbour]);
        }
        continue;
      }

      if (lowLink[nodeId] == indexOf[nodeId]) {
        std::vector<std::string> component;
        while (true) {
          std::string popped = stack.back();
          stack.pop_back();
          onStack.erase(popped);
          component.push_back(popped);
          if (popped == nodeId) {
            break;
          }
        }
        if (component.size() > 1) {
          for (const std::string& member : component) {
            cyclic.insert(member);
          }
        }
      }

      const std::string finished = nodeId;
      frames.pop_back();
      if (!frames.empty()) {
        const std::string& parent = frames.back().first;
        lowLink[parent] = std::min(lowLink[parent], lowLink[finished]);
      }
    }
  }

  return cyclic;
}

}  // namespace

void MetricsEngine::ComputeStructuralMetrics(DependencyGraph& graph)
{
  const std::unordered_set<std::string> cyclic = FindCyclicNodes(graph);
  const int totalNodes = static_cast<int>(graph.nodes.size());

  std::unordered_map<std::string, int> outgoing;
  std::unordered_map<std::string, int> incoming;
  for (const Edge& edge : graph.edges) {
    outgoing[edge.sourceId] += 1;
    incoming[edge.targetId] += 1;
  }

  for (GraphNode& node : graph.nodes) {
    const int dependencyCount = outgoing.count(node.id) ? outgoing[node.id] : 0;
    const int dependentCount = incoming.count(node.id) ? incoming[node.id] : 0;
    node.dependencyCount = dependencyCount;
    node.dependentCount = dependentCount;

    const int coupling = dependencyCount + dependentCount;
    if (coupling > 0) {
      node.instability = static_cast<float>(dependencyCount) / static_cast<float>(coupling);
    }

    if (totalNodes > 0) {
      node.couplingScore = static_cast<float>(dependencyCount) / static_cast<float>(totalNodes);
    }

    node.circularDependency = cyclic.count(node.id) != 0;
  }
}

void MetricsEngine::ComputeCodeMetrics(DependencyGraph& graph)
{
  // Method counts: number of Function children per logical container.
  std::unordered_map<std::string, int> functionChildren;
  for (const GraphNode& node : graph.nodes) {
    if (node.kind == NodeKind::Function) {
      functionChildren[node.logicalParent] += 1;
    }
  }

  // Inheritance edges as a class -> base adjacency for depth walking.
  std::unordered_map<std::string, std::vector<std::string>> baseClasses;
  for (const Edge& edge : graph.edges) {
    if (edge.kind == EdgeKind::Inheritance) {
      baseClasses[edge.sourceId].push_back(edge.targetId);
    }
  }

  std::function<int(const std::string&, std::unordered_set<std::string>&)> inheritanceDepth =
      [&](const std::string& id, std::unordered_set<std::string>& visiting) -> int {
    auto it = baseClasses.find(id);
    if (it == baseClasses.end() || it->second.empty()) {
      return 0;
    }
    if (!visiting.insert(id).second) {
      return 0;  // guard against inheritance cycles
    }
    int deepest = 0;
    for (const std::string& base : it->second) {
      deepest = std::max(deepest, 1 + inheritanceDepth(base, visiting));
    }
    visiting.erase(id);
    return deepest;
  };

  for (GraphNode& node : graph.nodes) {
    if (node.kind == NodeKind::Class || node.kind == NodeKind::Struct) {
      node.methodCount = functionChildren.count(node.id) ? functionChildren[node.id] : 0;
      std::unordered_set<std::string> visiting;
      node.inheritanceDepth = inheritanceDepth(node.id, visiting);
    }

    // linesOfCode is the inclusive span of the node's source extent (captured
    // by the parser as start line .. lineEnd). Left as std::nullopt for nodes
    // without a real multi-line span (e.g. synthesised project/module/file
    // nodes, or single-token declarations where lineEnd == line).
    if (node.lineEnd > node.line) {
      node.linesOfCode = node.lineEnd - node.line + 1;
    }

    // TODO: publicMethodCount needs visibility (public/private) tracking, which
    // ASTNode does not currently carry. Left as std::nullopt.
  }
}

void MetricsEngine::ComputeCppMetrics(DependencyGraph& graph)
{
  // Forward and reverse adjacency over IncludeDependency edges, restricted to
  // targets that are real file nodes in the graph.
  std::unordered_set<std::string> fileNodes;
  for (const GraphNode& node : graph.nodes) {
    if (node.kind == NodeKind::File) {
      fileNodes.insert(node.id);
    }
  }

  std::unordered_map<std::string, std::vector<std::string>> forward;
  std::unordered_map<std::string, std::vector<std::string>> reverse;
  for (const Edge& edge : graph.edges) {
    if (edge.kind != EdgeKind::IncludeDependency) {
      continue;
    }
    if (fileNodes.count(edge.sourceId) == 0 || fileNodes.count(edge.targetId) == 0) {
      continue;  // ignore external includes with no project file node
    }
    forward[edge.sourceId].push_back(edge.targetId);
    reverse[edge.targetId].push_back(edge.sourceId);
  }

  auto reachableCount = [](const std::string& start,
                           const std::unordered_map<std::string, std::vector<std::string>>& graph) {
    std::unordered_set<std::string> seen;
    std::vector<std::string> stack{start};
    while (!stack.empty()) {
      std::string current = stack.back();
      stack.pop_back();
      auto it = graph.find(current);
      if (it == graph.end()) {
        continue;
      }
      for (const std::string& next : it->second) {
        if (next != start && seen.insert(next).second) {
          stack.push_back(next);
        }
      }
    }
    return seen;
  };

  std::function<int(const std::string&, std::unordered_set<std::string>&)> depthFrom =
      [&](const std::string& id, std::unordered_set<std::string>& visiting) -> int {
    auto it = forward.find(id);
    if (it == forward.end() || it->second.empty()) {
      return 0;
    }
    if (!visiting.insert(id).second) {
      return 0;
    }
    int deepest = 0;
    for (const std::string& next : it->second) {
      deepest = std::max(deepest, 1 + depthFrom(next, visiting));
    }
    visiting.erase(id);
    return deepest;
  };

  for (GraphNode& node : graph.nodes) {
    if (node.kind != NodeKind::File) {
      continue;
    }
    std::unordered_set<std::string> visiting;
    node.includeDepth = depthFrom(node.id, visiting);
    node.transitiveIncludeCount = static_cast<int>(reachableCount(node.id, forward).size());
    node.compileImpact = static_cast<float>(reachableCount(node.id, reverse).size());
  }
}

}  // namespace Prism

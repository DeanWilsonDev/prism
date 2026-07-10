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

/// The graph models edges between files (includes) and between classes and
/// structs (inheritance, composition). FunctionCall and SymbolUsage edges are
/// not extracted yet — see the TODO in GraphBuilder::Build — so a function's
/// "zero dependencies" would describe what Prism does not look at rather than
/// anything about the code. Structural metrics are therefore reported only for
/// the kinds the edge model actually reaches; the rest are left unset. Extend
/// this predicate when new edge kinds start being emitted.
bool ParticipatesInEdgeModel(NodeKind kind)
{
  return kind == NodeKind::File || kind == NodeKind::Class || kind == NodeKind::Struct;
}

/// Edges whose endpoints are both nodes in the graph. An include of a system or
/// dependency header has no node to point at, so its target is left as the bare
/// header name; counting those as dependencies while `dependentCount`,
/// `includeDepth` and `compileImpact` all silently ignore them would make a
/// file's instability a ratio of two different populations.
std::vector<const Edge*> InternalEdges(const DependencyGraph& graph)
{
  std::unordered_set<std::string> nodeIds;
  nodeIds.reserve(graph.nodes.size());
  for (const GraphNode& node : graph.nodes) {
    nodeIds.insert(node.id);
  }

  std::vector<const Edge*> internal;
  for (const Edge& edge : graph.edges) {
    if (nodeIds.count(edge.sourceId) != 0 && nodeIds.count(edge.targetId) != 0) {
      internal.push_back(&edge);
    }
  }
  return internal;
}

using Adjacency = std::unordered_map<std::string, std::vector<std::string>>;

/// Longest path from `id` along `adjacency`, memoised across calls.
///
/// The memo is what makes this usable. Without it the walk costs one visit per
/// distinct *path*, and include graphs are diamonds, so the cost doubles with
/// every level: forty-six headers arranged as a twenty-two level diamond took
/// nine seconds, and a real dependency graph would never finish.
///
/// A back edge contributes 0, which breaks the cycle. `cycleHit` reports that
/// this happened somewhere below `id`; those results depend on where the walk
/// entered the cycle, so they are not memoised. An acyclic graph never sets it
/// and every node is memoised exactly once, making the whole walk linear.
int LongestPath(
    const std::string& id, const Adjacency& adjacency, std::unordered_map<std::string, int>& memo,
    std::unordered_set<std::string>& visiting, bool& cycleHit
)
{
  if (auto cached = memo.find(id); cached != memo.end()) {
    return cached->second;
  }
  auto neighbours = adjacency.find(id);
  if (neighbours == adjacency.end() || neighbours->second.empty()) {
    memo[id] = 0;
    return 0;
  }
  if (!visiting.insert(id).second) {
    cycleHit = true;  // back edge onto the current recursion stack
    return 0;
  }

  int deepest = 0;
  bool cycleBelow = false;
  for (const std::string& next : neighbours->second) {
    deepest = std::max(deepest, 1 + LongestPath(next, adjacency, memo, visiting, cycleBelow));
  }
  visiting.erase(id);

  if (!cycleBelow) {
    memo[id] = deepest;
  }
  cycleHit = cycleHit || cycleBelow;
  return deepest;
}

/// Longest path from `id`, discarding the per-walk cycle bookkeeping.
int LongestPath(
    const std::string& id, const Adjacency& adjacency, std::unordered_map<std::string, int>& memo
)
{
  std::unordered_set<std::string> visiting;
  bool cycleHit = false;
  return LongestPath(id, adjacency, memo, visiting, cycleHit);
}

/// Nodes that participate in any directed cycle, found via Tarjan's SCC
/// algorithm. A node is on a cycle if its SCC has more than one member or it
/// has a self-loop.
std::unordered_set<std::string> FindCyclicNodes(
    const DependencyGraph& graph, const std::vector<const Edge*>& edges
)
{
  std::unordered_map<std::string, std::vector<std::string>> adjacency;
  std::unordered_set<std::string> selfLoops;
  for (const GraphNode& node : graph.nodes) {
    adjacency.try_emplace(node.id);
  }
  for (const Edge* edge : edges) {
    adjacency[edge->sourceId].push_back(edge->targetId);
    if (edge->sourceId == edge->targetId) {
      selfLoops.insert(edge->sourceId);
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
  const std::vector<const Edge*> edges = InternalEdges(graph);
  const std::unordered_set<std::string> cyclic = FindCyclicNodes(graph, edges);
  const int totalNodes = static_cast<int>(graph.nodes.size());

  std::unordered_map<std::string, int> outgoing;
  std::unordered_map<std::string, int> incoming;
  for (const Edge* edge : edges) {
    outgoing[edge->sourceId] += 1;
    incoming[edge->targetId] += 1;
  }

  for (GraphNode& node : graph.nodes) {
    if (!ParticipatesInEdgeModel(node.kind)) {
      continue;
    }

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
  Adjacency baseClasses;
  for (const Edge& edge : graph.edges) {
    if (edge.kind == EdgeKind::Inheritance) {
      baseClasses[edge.sourceId].push_back(edge.targetId);
    }
  }
  std::unordered_map<std::string, int> inheritanceMemo;

  for (GraphNode& node : graph.nodes) {
    if (node.kind == NodeKind::Class || node.kind == NodeKind::Struct) {
      node.methodCount = functionChildren.count(node.id) ? functionChildren[node.id] : 0;
      node.inheritanceDepth = LongestPath(node.id, baseClasses, inheritanceMemo);
    }

    // Leaf lines of code: the inclusive span of the declaration's source extent.
    // Containers (file/module/namespace/project) are aggregated separately;
    // file nodes already carry their physical line count from the graph builder.
    // lineEnd >= line always holds (GraphBuilder falls back to line when the
    // parser has no real extent), so a single-line declaration is a genuine
    // span of 1, not missing data — it must count, not be omitted.
    const bool isLeaf = node.kind == NodeKind::Function || node.kind == NodeKind::Class ||
                        node.kind == NodeKind::Struct || node.kind == NodeKind::Field;
    if (isLeaf && node.lineEnd >= node.line) {
      node.linesOfCode = node.lineEnd - node.line + 1;
    }

    // cyclomaticComplexity is carried through from the parser, which is the only
    // stage with a function body to walk; GraphBuilder copies it onto the node.

    // TODO: publicMethodCount needs visibility (public/private) tracking, which
    // ASTNode does not currently carry. Left as std::nullopt.
    // TODO: moduleBoundaryViolations has no agreed definition yet — the spec
    // names the field but never says what counts as a violation. Left as
    // std::nullopt rather than guessing a rule the UI would then treat as fact.
    // TODO: testCoverage requires ingesting coverage data (gcov/llvm-cov), which
    // is out of scope for this pass. Left as std::nullopt.
  }

  AggregateLinesOfCode(graph);
}

void MetricsEngine::AggregateLinesOfCode(DependencyGraph& graph)
{
  // Container LOC is the sum of its children's LOC: modules and the project sum
  // their physical children (sub-modules + files), namespaces sum their logical
  // children (nested namespaces + classes/structs/free functions). Methods are
  // logical children of their class, not the namespace, so they are not double
  // counted. Leaf LOC and file LOC must already be assigned before this runs.
  std::unordered_map<std::string, GraphNode*> byId;
  std::unordered_map<std::string, std::vector<std::string>> physicalChildren;
  std::unordered_map<std::string, std::vector<std::string>> logicalChildren;
  for (GraphNode& node : graph.nodes) {
    byId[node.id] = &node;
    if (!node.physicalParent.empty()) {
      physicalChildren[node.physicalParent].push_back(node.id);
    }
    if (!node.logicalParent.empty()) {
      logicalChildren[node.logicalParent].push_back(node.id);
    }
  }

  std::unordered_map<std::string, std::optional<int>> physicalMemo;
  std::unordered_map<std::string, std::optional<int>> logicalMemo;

  std::function<std::optional<int>(const std::string&)> aggregatePhysical =
      [&](const std::string& id) -> std::optional<int> {
    if (auto cached = physicalMemo.find(id); cached != physicalMemo.end()) {
      return cached->second;
    }
    physicalMemo[id] = std::nullopt;  // guard against cycles
    GraphNode* node = byId[id];
    std::optional<int> total;
    for (const std::string& childId : physicalChildren[id]) {
      GraphNode* child = byId[childId];
      // Only files and sub-modules carry physical lines. A namespace is also a
      // physical child of whatever module it was re-anchored to, and its lines
      // are the ones already counted in those files, so adding it would double
      // count. (That is latent rather than live today only because the project
      // is nodes[0], so every module is summed before any namespace is.)
      std::optional<int> childLoc;
      if (child->kind == NodeKind::Module) {
        childLoc = aggregatePhysical(childId);
      }
      else if (child->kind == NodeKind::File) {
        childLoc = child->linesOfCode;
      }
      if (childLoc.has_value()) {
        total = total.value_or(0) + childLoc.value();
      }
    }
    if ((node->kind == NodeKind::Module || node->kind == NodeKind::Project) && total.has_value()) {
      node->linesOfCode = total;
    }
    physicalMemo[id] = total;
    return total;
  };

  std::function<std::optional<int>(const std::string&)> aggregateLogical =
      [&](const std::string& id) -> std::optional<int> {
    if (auto cached = logicalMemo.find(id); cached != logicalMemo.end()) {
      return cached->second;
    }
    logicalMemo[id] = std::nullopt;
    GraphNode* node = byId[id];
    std::optional<int> total;
    for (const std::string& childId : logicalChildren[id]) {
      GraphNode* child = byId[childId];
      std::optional<int> childLoc =
          (child->kind == NodeKind::Namespace) ? aggregateLogical(childId) : child->linesOfCode;
      if (childLoc.has_value()) {
        total = total.value_or(0) + childLoc.value();
      }
    }
    if (node->kind == NodeKind::Namespace && total.has_value()) {
      node->linesOfCode = total;
    }
    logicalMemo[id] = total;
    return total;
  };

  for (GraphNode& node : graph.nodes) {
    if (node.kind == NodeKind::Module || node.kind == NodeKind::Project) {
      aggregatePhysical(node.id);
    }
    else if (node.kind == NodeKind::Namespace) {
      aggregateLogical(node.id);
    }
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

  Adjacency forward;
  Adjacency reverse;
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

  auto reachableCount = [](const std::string& start, const Adjacency& graph) {
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

  std::unordered_map<std::string, int> depthMemo;

  for (GraphNode& node : graph.nodes) {
    if (node.kind != NodeKind::File) {
      continue;
    }
    node.includeDepth = LongestPath(node.id, forward, depthMemo);
    node.transitiveIncludeCount = static_cast<int>(reachableCount(node.id, forward).size());
    node.compileImpact = static_cast<float>(reachableCount(node.id, reverse).size());
  }
}

}  // namespace Prism

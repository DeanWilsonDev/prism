#include <cimmerian/test.hpp>

#include <string>
#include "prism/logging/log.hpp"
#include "prism/metrics-engine.hpp"

namespace {

Prism::GraphNode MakeNode(const std::string& id, Prism::NodeKind kind)
{
  Prism::GraphNode node;
  node.id = id;
  node.name = id;
  node.kind = kind;
  return node;
}

// IT() bodies are captureless lambdas; annotate once and share the graph.
const Prism::DependencyGraph& SharedGraph()
{
  static const Prism::DependencyGraph graph = []() {
    Prism::Logging::Log::Init();
    // A -> B -> C, plus a C -> A back edge forming a cycle over {A, B, C}.
    Prism::DependencyGraph built;
    built.AddNode(MakeNode("A", Prism::NodeKind::Class));
    built.AddNode(MakeNode("B", Prism::NodeKind::Class));
    built.AddNode(MakeNode("C", Prism::NodeKind::Class));
    built.AddEdge(Prism::Edge{"A", "B", Prism::EdgeKind::SymbolUsage, 1});
    built.AddEdge(Prism::Edge{"B", "C", Prism::EdgeKind::SymbolUsage, 1});
    built.AddEdge(Prism::Edge{"C", "A", Prism::EdgeKind::SymbolUsage, 1});
    Prism::MetricsEngine().Annotate(built);
    return built;
  }();
  return graph;
}

}  // namespace

DESCRIBE("MetricsEngine", {
  IT("counts outgoing edges as dependencyCount", {
    const Prism::GraphNode* a = SharedGraph().FindNodeById("A");
    ASSERT_TRUE(a != nullptr);
    ASSERT_TRUE(a != nullptr && a->dependencyCount.has_value());
    ASSERT_EQUAL(1, a->dependencyCount.value());
  });

  IT("counts incoming edges as dependentCount", {
    const Prism::GraphNode* a = SharedGraph().FindNodeById("A");
    ASSERT_TRUE(a != nullptr);
    ASSERT_TRUE(a != nullptr && a->dependentCount.has_value());
    ASSERT_EQUAL(1, a->dependentCount.value());
  });

  IT("flags nodes participating in a cycle", {
    const Prism::GraphNode* a = SharedGraph().FindNodeById("A");
    const Prism::GraphNode* b = SharedGraph().FindNodeById("B");
    ASSERT_TRUE(a != nullptr && a->circularDependency.has_value());
    ASSERT_TRUE(a != nullptr && a->circularDependency.value());
    ASSERT_TRUE(b != nullptr && b->circularDependency.value());
  });

  IT("computes a normalised coupling score", {
    const Prism::GraphNode* a = SharedGraph().FindNodeById("A");
    ASSERT_TRUE(a != nullptr && a->couplingScore.has_value());
  });
});

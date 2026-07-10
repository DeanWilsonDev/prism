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

// Project -> module(s) -> file(s), with file LOC set as the graph builder would.
// A namespace is re-anchored onto the module (as GraphBuilder does when its
// contents span several files), so it is a *physical* child of the module while
// its own LOC is the logical sum of its classes. Those are the same lines the
// file already counted, so the module must not add them twice. The nodes are
// deliberately ordered so the namespace is aggregated before the module: the
// old code only avoided double counting because the project came first.
const Prism::DependencyGraph& AggregationGraph()
{
  static const Prism::DependencyGraph graph = []() {
    Prism::Logging::Log::Init();
    Prism::DependencyGraph built;

    Prism::GraphNode space = MakeNode("P::core::Geometry", Prism::NodeKind::Namespace);
    space.physicalParent = "P::core";
    space.logicalParent = "P::core";
    built.AddNode(space);

    Prism::GraphNode shape = MakeNode("P::core::Geometry::Shape", Prism::NodeKind::Class);
    shape.physicalParent = "P::core::a.cpp";
    shape.logicalParent = "P::core::Geometry";
    shape.line = 3;
    shape.lineEnd = 12;
    built.AddNode(shape);

    built.AddNode(MakeNode("P", Prism::NodeKind::Project));

    Prism::GraphNode core = MakeNode("P::core", Prism::NodeKind::Module);
    core.physicalParent = "P";
    built.AddNode(core);

    Prism::GraphNode fileA = MakeNode("P::core::a.cpp", Prism::NodeKind::File);
    fileA.physicalParent = "P::core";
    fileA.linesOfCode = 100;
    built.AddNode(fileA);

    Prism::GraphNode fileB = MakeNode("P::core::b.cpp", Prism::NodeKind::File);
    fileB.physicalParent = "P::core";
    fileB.linesOfCode = 40;
    built.AddNode(fileB);

    Prism::MetricsEngine().Annotate(built);
    return built;
  }();
  return graph;
}

// A diamond include DAG `levels` deep: each level's two files include both files
// of the level below. The number of distinct paths from the top doubles every
// level, so walking paths rather than nodes costs 2^levels. At 30 levels that is
// a billion visits — this graph has 62 nodes and must annotate instantly.
const Prism::DependencyGraph& DiamondIncludeGraph()
{
  static const int levels = 30;
  static const Prism::DependencyGraph graph = []() {
    Prism::Logging::Log::Init();
    Prism::DependencyGraph built;
    auto name = [](char side, int level) {
      return std::string(1, side) + std::to_string(level) + ".hpp";
    };

    for (int level = 0; level <= levels; ++level) {
      built.AddNode(MakeNode(name('a', level), Prism::NodeKind::File));
      built.AddNode(MakeNode(name('b', level), Prism::NodeKind::File));
    }
    for (int level = 1; level <= levels; ++level) {
      for (const char side : {'a', 'b'}) {
        for (const char below : {'a', 'b'}) {
          built.AddEdge(
              Prism::Edge{
                  name(side, level), name(below, level - 1), Prism::EdgeKind::IncludeDependency, 1
              }
          );
        }
      }
    }
    Prism::MetricsEngine().Annotate(built);
    return built;
  }();
  return graph;
}

// One file including a project header and a system header. The system header has
// no node: nothing in the project declares it.
const Prism::DependencyGraph& ExternalIncludeGraph()
{
  static const Prism::DependencyGraph graph = []() {
    Prism::Logging::Log::Init();
    Prism::DependencyGraph built;
    built.AddNode(MakeNode("main.cpp", Prism::NodeKind::File));
    built.AddNode(MakeNode("util.hpp", Prism::NodeKind::File));
    built.AddNode(MakeNode("free", Prism::NodeKind::Function));
    built.AddEdge(Prism::Edge{"main.cpp", "util.hpp", Prism::EdgeKind::IncludeDependency, 1});
    built.AddEdge(Prism::Edge{"main.cpp", "vector", Prism::EdgeKind::IncludeDependency, 1});
    Prism::MetricsEngine().Annotate(built);
    return built;
  }();
  return graph;
}

}  // namespace

DESCRIBE("MetricsLinesOfCode", {
  IT("rolls file LOC up into the module", {
    const Prism::GraphNode* core = AggregationGraph().FindNodeById("P::core");
    ASSERT_TRUE(core != nullptr);
    ASSERT_TRUE(core != nullptr && core->linesOfCode.has_value());
    ASSERT_EQUAL(140, core->linesOfCode.value_or(-1));
  });

  IT("rolls module LOC up into the project", {
    const Prism::GraphNode* project = AggregationGraph().FindNodeById("P");
    ASSERT_TRUE(project != nullptr);
    ASSERT_TRUE(project != nullptr && project->linesOfCode.has_value());
    ASSERT_EQUAL(140, project->linesOfCode.value_or(-1));
  });

  IT("sums a namespace from its logical children", {
    const Prism::GraphNode* space = AggregationGraph().FindNodeById("P::core::Geometry");
    ASSERT_TRUE(space != nullptr);
    ASSERT_TRUE(space != nullptr && space->linesOfCode.has_value());
    ASSERT_EQUAL(10, space->linesOfCode.value_or(-1));
  });

  IT("does not count a namespace anchored to a module twice", {
    // 100 + 40 from the two files. The namespace's 10 lines live inside a.cpp
    // and are already part of its 100, so the module total must not include them.
    const Prism::GraphNode* core = AggregationGraph().FindNodeById("P::core");
    ASSERT_TRUE(core != nullptr);
    ASSERT_EQUAL(140, core != nullptr ? core->linesOfCode.value_or(-1) : -1);
  });

  IT("gives a single-line declaration a span of one", {
    Prism::DependencyGraph built;
    Prism::GraphNode inlineMethod = MakeNode("C::Get", Prism::NodeKind::Function);
    inlineMethod.line = 7;
    inlineMethod.lineEnd = 7;
    built.AddNode(inlineMethod);
    Prism::MetricsEngine().Annotate(built);

    const Prism::GraphNode* method = built.FindNodeById("C::Get");
    ASSERT_TRUE(method != nullptr && method->linesOfCode.has_value());
    ASSERT_EQUAL(1, method != nullptr ? method->linesOfCode.value_or(-1) : -1);
  });
});

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
    // One outgoing edge over three nodes in the graph.
    const Prism::GraphNode* a = SharedGraph().FindNodeById("A");
    ASSERT_TRUE(a != nullptr && a->couplingScore.has_value());
    ASSERT_TRUE(a != nullptr && a->couplingScore.value() > 0.33F);
    ASSERT_TRUE(a != nullptr && a->couplingScore.value() < 0.34F);
  });

  IT("flags a self-loop as a circular dependency", {
    Prism::DependencyGraph built;
    built.AddNode(MakeNode("Solo", Prism::NodeKind::Class));
    built.AddEdge(Prism::Edge{"Solo", "Solo", Prism::EdgeKind::Composition, 1});
    Prism::MetricsEngine().Annotate(built);

    const Prism::GraphNode* solo = built.FindNodeById("Solo");
    ASSERT_TRUE(solo != nullptr && solo->circularDependency.has_value());
    ASSERT_TRUE(solo != nullptr && solo->circularDependency.value());
  });

  IT("computes instability as the share of outgoing coupling", {
    Prism::DependencyGraph built;
    built.AddNode(MakeNode("Uses", Prism::NodeKind::Class));
    built.AddNode(MakeNode("Used", Prism::NodeKind::Class));
    built.AddEdge(Prism::Edge{"Uses", "Used", Prism::EdgeKind::Composition, 1});
    Prism::MetricsEngine().Annotate(built);

    const Prism::GraphNode* uses = built.FindNodeById("Uses");
    const Prism::GraphNode* used = built.FindNodeById("Used");
    ASSERT_TRUE(uses != nullptr && uses->instability.has_value());
    ASSERT_TRUE(uses != nullptr && uses->instability.value() == 1.0F);
    ASSERT_TRUE(used != nullptr && used->instability.has_value());
    ASSERT_TRUE(used != nullptr && used->instability.value() == 0.0F);
  });

  IT("omits structural metrics for kinds the edge model never reaches", {
    // No FunctionCall or SymbolUsage edges are extracted, so a function's zero
    // dependencies would describe Prism, not the code. It must stay unset.
    const Prism::GraphNode* free = ExternalIncludeGraph().FindNodeById("free");
    ASSERT_TRUE(free != nullptr);
    ASSERT_TRUE(free != nullptr && !free->dependencyCount.has_value());
    ASSERT_TRUE(free != nullptr && !free->couplingScore.has_value());
    ASSERT_TRUE(free != nullptr && !free->circularDependency.has_value());
  });

  IT("ignores includes of headers that have no node", {
    // main.cpp includes util.hpp and <vector>. Only util.hpp is a dependency the
    // graph can speak about, so instability must not count <vector> either.
    const Prism::GraphNode* main = ExternalIncludeGraph().FindNodeById("main.cpp");
    ASSERT_TRUE(main != nullptr && main->dependencyCount.has_value());
    ASSERT_EQUAL(1, main != nullptr ? main->dependencyCount.value_or(-1) : -1);
    ASSERT_TRUE(main != nullptr && main->instability.has_value());
    ASSERT_TRUE(main != nullptr && main->instability.value() == 1.0F);
  });

  IT("counts methods and inheritance depth for a class", {
    Prism::DependencyGraph built;
    built.AddNode(MakeNode("Base", Prism::NodeKind::Class));
    built.AddNode(MakeNode("Mid", Prism::NodeKind::Class));
    built.AddNode(MakeNode("Leaf", Prism::NodeKind::Class));
    Prism::GraphNode first = MakeNode("Leaf::One", Prism::NodeKind::Function);
    first.logicalParent = "Leaf";
    built.AddNode(first);
    Prism::GraphNode second = MakeNode("Leaf::Two", Prism::NodeKind::Function);
    second.logicalParent = "Leaf";
    built.AddNode(second);
    built.AddEdge(Prism::Edge{"Leaf", "Mid", Prism::EdgeKind::Inheritance, 1});
    built.AddEdge(Prism::Edge{"Mid", "Base", Prism::EdgeKind::Inheritance, 1});
    Prism::MetricsEngine().Annotate(built);

    const Prism::GraphNode* leaf = built.FindNodeById("Leaf");
    const Prism::GraphNode* mid = built.FindNodeById("Mid");
    const Prism::GraphNode* base = built.FindNodeById("Base");
    ASSERT_EQUAL(2, leaf != nullptr ? leaf->methodCount.value_or(-1) : -1);
    ASSERT_EQUAL(2, leaf != nullptr ? leaf->inheritanceDepth.value_or(-1) : -1);
    ASSERT_EQUAL(1, mid != nullptr ? mid->inheritanceDepth.value_or(-1) : -1);
    ASSERT_EQUAL(0, base != nullptr ? base->inheritanceDepth.value_or(-1) : -1);
  });

  IT("walks a diamond include graph in linear time", {
    // Un-memoised this is 2^30 visits and never returns.
    const Prism::GraphNode* top = DiamondIncludeGraph().FindNodeById("a30.hpp");
    ASSERT_TRUE(top != nullptr && top->includeDepth.has_value());
    ASSERT_EQUAL(30, top != nullptr ? top->includeDepth.value_or(-1) : -1);
  });

  IT("counts every unique header reachable through includes", {
    // Both files of each of the 30 levels below the top.
    const Prism::GraphNode* top = DiamondIncludeGraph().FindNodeById("a30.hpp");
    ASSERT_TRUE(top != nullptr && top->transitiveIncludeCount.has_value());
    ASSERT_EQUAL(60, top != nullptr ? top->transitiveIncludeCount.value_or(-1) : -1);
  });

  IT("counts the files a header forces a rebuild of as compileImpact", {
    // Everything above level 0: 30 levels of two files each.
    const Prism::GraphNode* bottom = DiamondIncludeGraph().FindNodeById("a0.hpp");
    ASSERT_TRUE(bottom != nullptr && bottom->compileImpact.has_value());
    ASSERT_TRUE(bottom != nullptr && bottom->compileImpact.value() == 60.0F);
    ASSERT_EQUAL(0, bottom != nullptr ? bottom->includeDepth.value_or(-1) : -1);
  });

  IT("survives a cycle in the include graph", {
    Prism::DependencyGraph built;
    built.AddNode(MakeNode("x.hpp", Prism::NodeKind::File));
    built.AddNode(MakeNode("y.hpp", Prism::NodeKind::File));
    built.AddEdge(Prism::Edge{"x.hpp", "y.hpp", Prism::EdgeKind::IncludeDependency, 1});
    built.AddEdge(Prism::Edge{"y.hpp", "x.hpp", Prism::EdgeKind::IncludeDependency, 1});
    Prism::MetricsEngine().Annotate(built);

    const Prism::GraphNode* x = built.FindNodeById("x.hpp");
    ASSERT_TRUE(x != nullptr && x->includeDepth.has_value());
    ASSERT_TRUE(x != nullptr && x->circularDependency.value_or(false));
  });
});

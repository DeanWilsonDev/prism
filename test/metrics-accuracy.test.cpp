#include <cimmerian/test.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include "prism/graph-builder.hpp"
#include "prism/logging/log.hpp"
#include "prism/metrics-engine.hpp"
#include "prism/parser.hpp"

#ifndef PRISM_TEST_EXAMPLE_DIR
#define PRISM_TEST_EXAMPLE_DIR "src/example"
#endif

// Metric accuracy against a fixture whose values are known by reading it:
// src/example/metrics.hpp and src/example/metrics.cpp. These run the real
// pipeline — parser, graph builder, metrics engine — because most of the ways
// these metrics go wrong are in what the parser hands the engine, not in the
// engine's arithmetic.
namespace {

std::filesystem::path WriteMetricsDatabase()
{
  const std::filesystem::path exampleDir = PRISM_TEST_EXAMPLE_DIR;
  const std::filesystem::path source = exampleDir / "metrics.cpp";
  const std::filesystem::path databaseDir =
      std::filesystem::temp_directory_path() / "prism-metrics-test";
  std::filesystem::create_directories(databaseDir);

  std::ofstream out(databaseDir / "compile_commands.json");
  out << "[\n"
      << "  {\n"
      << "    \"directory\": \"" << exampleDir.generic_string() << "\",\n"
      << "    \"file\": \"" << source.generic_string() << "\",\n"
      << "    \"command\": \"c++ -std=c++20 -c " << source.generic_string() << "\"\n"
      << "  }\n"
      << "]\n";
  return databaseDir;
}

// IT() bodies are captureless lambdas; run the pipeline once and share it.
const Prism::DependencyGraph& AnalysedGraph()
{
  static const Prism::DependencyGraph graph = []() {
    Prism::Logging::Log::Init();
    Prism::ParserConfig config;
    config.projectRoot = PRISM_TEST_EXAMPLE_DIR;
    config.compileCommandsPath = WriteMetricsDatabase() / "compile_commands.json";

    const Prism::ParseResult parsed = Prism::Parser(config).Parse();
    Prism::DependencyGraph built =
        Prism::GraphBuilder("Example").Build(parsed.nodes, parsed.fileLineCounts);
    Prism::MetricsEngine().Annotate(built);
    return built;
  }();
  return graph;
}

const Prism::GraphNode* FindByName(Prism::NodeKind kind, const std::string& name)
{
  for (const Prism::GraphNode& node : AnalysedGraph().nodes) {
    if (node.kind == kind && node.name == name) {
      return &node;
    }
  }
  return nullptr;
}

/// Weight of the edge between two structs, or 0 when there is none. The graph
/// builder accumulates one unit of weight per resolved reference.
int EdgeWeight(const std::string& sourceName, const std::string& targetName, Prism::EdgeKind kind)
{
  const Prism::GraphNode* source = FindByName(Prism::NodeKind::Struct, sourceName);
  const Prism::GraphNode* target = FindByName(Prism::NodeKind::Struct, targetName);
  if (source == nullptr || target == nullptr) {
    return 0;
  }
  for (const Prism::Edge& edge : AnalysedGraph().edges) {
    if (edge.kind == kind && edge.sourceId == source->id && edge.targetId == target->id) {
      return edge.weight;
    }
  }
  return 0;
}

bool HasEdge(const std::string& sourceName, const std::string& targetName, Prism::EdgeKind kind)
{
  return EdgeWeight(sourceName, targetName, kind) > 0;
}

}  // namespace

DESCRIBE("MetricAccuracy", {
  IT("parses the fixture without errors", {
    // Nothing below means anything if clang error-recovered: unresolved types
    // silently degrade to `int` and every type-driven edge disappears.
    ASSERT_TRUE(!AnalysedGraph().nodes.empty());
    ASSERT_TRUE(FindByName(Prism::NodeKind::Struct, "Shape") != nullptr);
  });

  IT("measures cyclomatic complexity of a function definition", {
    // if + && + for + two case labels, plus one.
    const Prism::GraphNode* classify = FindByName(Prism::NodeKind::Function, "Classify");
    ASSERT_TRUE(classify != nullptr);
    ASSERT_TRUE(classify != nullptr && classify->cyclomaticComplexity.has_value());
    ASSERT_EQUAL(6, classify != nullptr ? classify->cyclomaticComplexity.value_or(-1) : -1);
  });

  IT("composes a class from the type of a plain field",
     { ASSERT_TRUE(HasEdge("Shape", "Point", Prism::EdgeKind::Composition)); });

  IT("resolves composition through arrays and template arguments", {
    // Shape reaches Point three ways: `Point origin`, `Point corners[4]`, and
    // `Box<Point> boxed`. One unit of edge weight each.
    ASSERT_EQUAL(3, EdgeWeight("Shape", "Point", Prism::EdgeKind::Composition));
  });

  IT("does not compose a class with itself through a self-referencing field",
     { ASSERT_TRUE(!HasEdge("Shape", "Shape", Prism::EdgeKind::Composition)); });

  IT("does not treat ShapeKind as a reference to Shape", {
    // The substring resolver matched the enum `ShapeKind` against the class
    // `Shape` and reported the struct as circularly dependent on itself.
    const Prism::GraphNode* shape = FindByName(Prism::NodeKind::Struct, "Shape");
    ASSERT_TRUE(shape != nullptr && shape->circularDependency.has_value());
    ASSERT_TRUE(shape != nullptr && !shape->circularDependency.value_or(true));
  });

  IT("records inheritance depth through a base specifier", {
    const Prism::GraphNode* circle = FindByName(Prism::NodeKind::Struct, "Circle");
    const Prism::GraphNode* shape = FindByName(Prism::NodeKind::Struct, "Shape");
    ASSERT_EQUAL(1, circle != nullptr ? circle->inheritanceDepth.value_or(-1) : -1);
    ASSERT_EQUAL(0, shape != nullptr ? shape->inheritanceDepth.value_or(-1) : -1);
  });

  IT("counts a method declared on a struct", {
    const Prism::GraphNode* circle = FindByName(Prism::NodeKind::Struct, "Circle");
    ASSERT_EQUAL(1, circle != nullptr ? circle->methodCount.value_or(-1) : -1);
  });

  IT("gives the header a compileImpact of the source that includes it", {
    const Prism::GraphNode* header = FindByName(Prism::NodeKind::File, "metrics.hpp");
    ASSERT_TRUE(header != nullptr && header->compileImpact.has_value());
    ASSERT_TRUE(header != nullptr && header->compileImpact.value_or(-1.0F) == 1.0F);
  });

  IT("omits structural metrics from a function node", {
    const Prism::GraphNode* classify = FindByName(Prism::NodeKind::Function, "Classify");
    ASSERT_TRUE(classify != nullptr && !classify->dependencyCount.has_value());
    ASSERT_TRUE(classify != nullptr && !classify->circularDependency.has_value());
  });

  IT("leaves undefined metrics unset rather than zero", {
    const Prism::GraphNode* classify = FindByName(Prism::NodeKind::Function, "Classify");
    ASSERT_TRUE(classify != nullptr && !classify->publicMethodCount.has_value());
    ASSERT_TRUE(classify != nullptr && !classify->moduleBoundaryViolations.has_value());
    ASSERT_TRUE(classify != nullptr && !classify->testCoverage.has_value());
  });
});

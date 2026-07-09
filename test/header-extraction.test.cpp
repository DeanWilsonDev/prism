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

namespace {

// Compile database with a single entry for orbit.cpp (which includes orbit.hpp).
std::filesystem::path WriteOrbitDatabase()
{
  const std::filesystem::path exampleDir = PRISM_TEST_EXAMPLE_DIR;
  const std::filesystem::path source = exampleDir / "orbit.cpp";
  const std::filesystem::path databaseDir =
      std::filesystem::temp_directory_path() / "prism-header-test";
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

Prism::ParseResult ParseOrbit(bool includeExternal, const std::filesystem::path& projectRoot)
{
  Prism::Logging::Log::Init();
  const std::filesystem::path databaseDir = WriteOrbitDatabase();
  Prism::ParserConfig config;
  config.projectRoot = projectRoot;
  config.compileCommandsPath = databaseDir / "compile_commands.json";
  config.includeExternal = includeExternal;
  return Prism::Parser(config).Parse();
}

// Parse orbit.cpp scoped to the parent of the example dir (so its relative path
// is "example/orbit.cpp"), optionally excluding the "example" directory.
Prism::ParseResult ParseOrbitExcludingExample(bool excludeExample)
{
  Prism::Logging::Log::Init();
  const std::filesystem::path databaseDir = WriteOrbitDatabase();
  Prism::ParserConfig config;
  config.projectRoot = std::filesystem::path(PRISM_TEST_EXAMPLE_DIR).parent_path();
  config.compileCommandsPath = databaseDir / "compile_commands.json";
  if (excludeExample) {
    config.excludedDirectories = {"example"};
  }
  return Prism::Parser(config).Parse();
}

// Full pipeline (parse -> build -> metrics) against the orbit fixture, scoped to
// src/example, so LOC aggregation and file line counts are exercised too.
const Prism::DependencyGraph& SharedGraph()
{
  static const Prism::DependencyGraph graph = []() {
    Prism::ParseResult result = ParseOrbit(false, PRISM_TEST_EXAMPLE_DIR);
    Prism::DependencyGraph built =
        Prism::GraphBuilder("Demo").Build(result.nodes, result.fileLineCounts);
    Prism::MetricsEngine().Annotate(built);
    return built;
  }();
  return graph;
}

const Prism::GraphNode* FindByName(
    const Prism::DependencyGraph& graph, Prism::NodeKind kind, const std::string& name
)
{
  for (const Prism::GraphNode& node : graph.nodes) {
    if (node.kind == kind && node.name == name) {
      return &node;
    }
  }
  return nullptr;
}

bool ParseHasClass(const Prism::ParseResult& result, const std::string& name)
{
  for (const Prism::ASTNode& node : result.nodes) {
    if (node.kind == Prism::NodeKind::Class && node.name == name) {
      return true;
    }
  }
  return false;
}

}  // namespace

DESCRIBE("HeaderExtraction", {
  IT("captures a class declared in a header",
     { ASSERT_TRUE(FindByName(SharedGraph(), Prism::NodeKind::Class, "Orbit") != nullptr); });

  IT("creates a File node for the header",
     { ASSERT_TRUE(FindByName(SharedGraph(), Prism::NodeKind::File, "orbit.hpp") != nullptr); });

  IT("re-attaches an out-of-line method to its declaring class", {
    const Prism::GraphNode* orbit = FindByName(SharedGraph(), Prism::NodeKind::Class, "Orbit");
    const Prism::GraphNode* steps = FindByName(SharedGraph(), Prism::NodeKind::Function, "Steps");
    ASSERT_TRUE(orbit != nullptr);
    ASSERT_TRUE(steps != nullptr);
    ASSERT_EQUAL(orbit->id, steps->logicalParent);
  });

  IT("connects the internal include graph (.cpp -> .h)", {
    const Prism::GraphNode* source = FindByName(SharedGraph(), Prism::NodeKind::File, "orbit.cpp");
    const Prism::GraphNode* header = FindByName(SharedGraph(), Prism::NodeKind::File, "orbit.hpp");
    ASSERT_TRUE(source != nullptr);
    ASSERT_TRUE(header != nullptr);

    bool found = false;
    for (const Prism::Edge& edge : SharedGraph().edges) {
      if (edge.kind == Prism::EdgeKind::IncludeDependency && edge.sourceId == source->id &&
          edge.targetId == header->id) {
        found = true;
      }
    }
    ASSERT_TRUE(found);
  });

  IT("prefers the definition for a method's source span", {
    // Orbit::Steps is declared in orbit.hpp but defined in orbit.cpp, so its
    // node should carry the definition's file and lines of code.
    const Prism::GraphNode* steps = FindByName(SharedGraph(), Prism::NodeKind::Function, "Steps");
    ASSERT_TRUE(steps != nullptr);
    ASSERT_EQUAL(std::string("orbit.cpp"), steps->file);
    ASSERT_TRUE(steps != nullptr && steps->linesOfCode.has_value());
  });

  IT("gives file nodes their physical line count", {
    for (const Prism::GraphNode& node : SharedGraph().nodes) {
      if (node.kind == Prism::NodeKind::File) {
        ASSERT_TRUE(node.linesOfCode.has_value());
        ASSERT_TRUE(node.linesOfCode.value_or(0) > 0);
      }
    }
  });

  IT("aggregates project LOC from its files", {
    int fileTotal = 0;
    const Prism::GraphNode* project = nullptr;
    for (const Prism::GraphNode& node : SharedGraph().nodes) {
      if (node.kind == Prism::NodeKind::File) {
        fileTotal += node.linesOfCode.value_or(0);
      }
      if (node.kind == Prism::NodeKind::Project) {
        project = &node;
      }
    }
    ASSERT_TRUE(project != nullptr);
    ASSERT_TRUE(project != nullptr && project->linesOfCode.has_value());
    ASSERT_EQUAL(fileTotal, project->linesOfCode.value_or(-1));
  });
});

DESCRIBE("ProjectScope", {
  IT("excludes translation units outside the project root by default", {
    // Scope the parse to a subdirectory that does not contain orbit.cpp, so the
    // TU sits outside the root and is skipped.
    const std::filesystem::path narrowRoot =
        std::filesystem::path(PRISM_TEST_EXAMPLE_DIR) / "does-not-contain-orbit";
    Prism::ParseResult scoped = ParseOrbit(false, narrowRoot);
    ASSERT_FALSE(ParseHasClass(scoped, "Orbit"));
  });

  IT("includes out-of-root files when --include-external is set", {
    const std::filesystem::path narrowRoot =
        std::filesystem::path(PRISM_TEST_EXAMPLE_DIR) / "does-not-contain-orbit";
    Prism::ParseResult external = ParseOrbit(true, narrowRoot);
    ASSERT_TRUE(ParseHasClass(external, "Orbit"));
  });
});

DESCRIBE("DirectoryExclusion", {
  IT("skips files under an excluded directory name",
     { ASSERT_FALSE(ParseHasClass(ParseOrbitExcludingExample(true), "Orbit")); });

  IT("keeps files when the directory is not excluded",
     { ASSERT_TRUE(ParseHasClass(ParseOrbitExcludingExample(false), "Orbit")); });
});

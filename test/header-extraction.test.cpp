#include <cimmerian/test.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include "prism/graph-builder.hpp"
#include "prism/logging/log.hpp"
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

// Full parse + build against the orbit fixture, scoped to src/example.
const Prism::DependencyGraph& SharedGraph()
{
  static const Prism::DependencyGraph graph = []() {
    Prism::ParseResult result = ParseOrbit(false, PRISM_TEST_EXAMPLE_DIR);
    return Prism::GraphBuilder("Demo").Build(result.nodes);
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

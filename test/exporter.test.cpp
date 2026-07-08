#include <cimmerian/test.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include "prism/exporter.hpp"
#include "prism/logging/log.hpp"

namespace {

std::string ReadFile(const std::filesystem::path& path)
{
  std::ifstream input(path);
  std::stringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

Prism::DependencyGraph BuildMinimalGraph()
{
  Prism::DependencyGraph graph;

  Prism::GraphNode project;
  project.id = "Demo";
  project.name = "Demo";
  project.kind = Prism::NodeKind::Project;
  graph.AddNode(project);

  Prism::GraphNode module;
  module.id = "Demo::src";
  module.name = "src";
  module.kind = Prism::NodeKind::Module;
  module.physicalParent = "Demo";
  graph.AddNode(module);

  Prism::GraphNode klass;
  klass.id = "Demo::Shapes::Rectangle";
  klass.name = "Rectangle";
  klass.kind = Prism::NodeKind::Class;
  klass.physicalParent = "Demo::src::shapes.cpp";
  klass.logicalParent = "Demo::Shapes";
  klass.file = "shapes.cpp";
  klass.line = 12;
  klass.column = 3;
  klass.methodCount = 5;  // present optional
  // couplingScore intentionally left unset -> must be absent from JSON.
  graph.AddNode(klass);

  graph.AddEdge(
      Prism::Edge{
          "Demo::src::shapes.cpp", "Demo::src::other.hpp", Prism::EdgeKind::IncludeDependency, 1
      }
  );
  return graph;
}

// IT() bodies are captureless lambdas; export once and share the result.
struct ExportOutcome {
  bool exported;
  std::string content;
};

const ExportOutcome& SharedExport()
{
  static const ExportOutcome outcome = []() {
    Prism::Logging::Log::Init();
    const std::filesystem::path outputPath =
        std::filesystem::temp_directory_path() / "prism-exporter-test.json";

    Prism::ExporterConfig config;
    config.outputPath = outputPath;
    config.projectName = "Demo";
    config.prismVersion = "0.1.0";

    Prism::Exporter exporter(config);
    Prism::DependencyGraph graph = BuildMinimalGraph();
    const bool exported = exporter.Export(graph);
    return ExportOutcome{exported, ReadFile(outputPath)};
  }();
  return outcome;
}

}  // namespace

DESCRIBE("Exporter", {
  IT("writes the output file", {
    ASSERT_TRUE(SharedExport().exported);
    ASSERT_TRUE(!SharedExport().content.empty());
  });

  IT("emits the metadata block with correct counts", {
    const std::string& content = SharedExport().content;
    ASSERT_TRUE(content.find("\"metadata\"") != std::string::npos);
    ASSERT_TRUE(content.find("\"project\": \"Demo\"") != std::string::npos);
    ASSERT_TRUE(content.find("\"prism_version\": \"0.1.0\"") != std::string::npos);
    ASSERT_TRUE(content.find("\"node_count\": 3") != std::string::npos);
    ASSERT_TRUE(content.find("\"edge_count\": 1") != std::string::npos);
    ASSERT_TRUE(content.find("\"file_count\": 0") != std::string::npos);
  });

  IT("serialises node type as a string and includes present optionals", {
    const std::string& content = SharedExport().content;
    ASSERT_TRUE(content.find("\"type\": \"Class\"") != std::string::npos);
    ASSERT_TRUE(content.find("\"method_count\": 5") != std::string::npos);
  });

  IT("omits unset optional fields entirely", {
    const std::string& content = SharedExport().content;
    ASSERT_TRUE(content.find("coupling_score") == std::string::npos);
    ASSERT_TRUE(content.find("lines_of_code") == std::string::npos);
  });

  IT("serialises the edge with a snake_case type", {
    const std::string& content = SharedExport().content;
    ASSERT_TRUE(content.find("\"type\": \"include_dependency\"") != std::string::npos);
    ASSERT_TRUE(content.find("\"weight\": 1") != std::string::npos);
  });
});

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include "firefly/log.hpp"
#include "prism/exporter.hpp"
#include "prism/graph-builder.hpp"
#include "prism/logging/log.hpp"
#include "prism/metrics-engine.hpp"
#include "prism/parser.hpp"

namespace {

constexpr std::string_view kPrismVersion = "0.1.0";

struct CliOptions {
  std::filesystem::path projectRoot;
  std::filesystem::path outputPath = "./analysis.json";
  std::filesystem::path compileCommandsPath;
  bool verbose = false;
};

void PrintUsage(const char* program)
{
  std::cerr << "Usage: " << program
            << " --project <path> [--output <path>] [--compile-commands <path>] [--verbose]\n";
}

}  // namespace

int main(int argc, char** argv)
{
  Prism::Logging::Log::Init();

  CliOptions options;

  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    auto nextValue = [&](std::string_view flag) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "Error: " << flag << " requires a value\n";
        std::exit(1);
      }
      return argv[++i];
    };

    if (argument == "--project") {
      options.projectRoot = nextValue(argument);
    }
    else if (argument == "--output") {
      options.outputPath = nextValue(argument);
    }
    else if (argument == "--compile-commands") {
      options.compileCommandsPath = nextValue(argument);
    }
    else if (argument == "--verbose") {
      options.verbose = true;
    }
    else if (argument == "--help" || argument == "-h") {
      PrintUsage(argv[0]);
      return 0;
    }
    else {
      std::cerr << "Error: unknown argument '" << argument << "'\n";
      PrintUsage(argv[0]);
      return 1;
    }
  }

  if (options.projectRoot.empty()) {
    std::cerr << "Error: --project is required\n";
    PrintUsage(argv[0]);
    return 1;
  }

  std::error_code ec;
  if (!std::filesystem::is_directory(options.projectRoot, ec)) {
    std::cerr << "Error: --project path is not a directory: " << options.projectRoot << "\n";
    return 1;
  }

  if (options.compileCommandsPath.empty()) {
    options.compileCommandsPath = options.projectRoot / "compile_commands.json";
  }

  // weakly_canonical resolves "." / trailing separators so filename() yields the
  // real directory name (e.g. "prism") rather than an empty string.
  std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(options.projectRoot, ec);
  if (ec || canonicalRoot.empty()) {
    canonicalRoot = std::filesystem::absolute(options.projectRoot).lexically_normal();
  }
  std::string projectName = canonicalRoot.filename().string();
  if (projectName.empty()) {
    projectName = canonicalRoot.parent_path().filename().string();
  }
  if (projectName.empty()) {
    projectName = "Project";
  }

  if (options.verbose) {
    LOG_INFO("Prism {} analysing project [{}]", std::string(kPrismVersion), projectName);
    LOG_INFO("Project root: {}", options.projectRoot.string());
    LOG_INFO("Compile commands: {}", options.compileCommandsPath.string());
  }

  // Stage 1 — parse.
  Prism::ParserConfig parserConfig;
  parserConfig.projectRoot = options.projectRoot;
  parserConfig.compileCommandsPath = options.compileCommandsPath;
  parserConfig.verbose = options.verbose;

  Prism::Parser parser(parserConfig);
  Prism::ParseResult parseResult = parser.Parse();
  if (parseResult.hadErrors) {
    LOG_ERROR("Parsing failed; aborting.");
    return 1;
  }
  if (options.verbose) {
    LOG_INFO(
        "Parsed {} AST nodes ({} warnings)", parseResult.nodes.size(), parseResult.warnings.size()
    );
  }

  // Stage 2 — build the dependency graph.
  Prism::GraphBuilder builder(projectName);
  Prism::DependencyGraph graph = builder.Build(parseResult.nodes);
  if (options.verbose) {
    LOG_INFO("Built graph with {} nodes and {} edges", graph.nodes.size(), graph.edges.size());
  }

  // Stage 3 — annotate metrics.
  Prism::MetricsEngine metrics;
  metrics.Annotate(graph);
  if (options.verbose) {
    LOG_INFO("Metrics annotation complete");
  }

  // Stage 4 — export.
  Prism::ExporterConfig exporterConfig;
  exporterConfig.outputPath = options.outputPath;
  exporterConfig.projectName = projectName;
  exporterConfig.prismVersion = std::string(kPrismVersion);

  Prism::Exporter exporter(exporterConfig);
  if (!exporter.Export(graph)) {
    LOG_ERROR("Export failed.");
    return 1;
  }

  if (options.verbose) {
    LOG_INFO("Wrote analysis to {}", options.outputPath.string());
  }
  return 0;
}

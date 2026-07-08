#pragma once

#include <filesystem>
#include <string>
#include "dependency-graph.hpp"

namespace Prism {

struct ExporterConfig {
  std::filesystem::path outputPath;
  std::string projectName;
  std::string prismVersion;  // e.g. "0.1.0"
};

/// Stage 4. Serialises the annotated dependency graph to analysis.json. This is
/// the only component (besides json-serialization.hpp) that touches the JSON
/// library (Amanuensis).
class Exporter {
 public:
  explicit Exporter(ExporterConfig config);
  bool Export(const DependencyGraph& graph);

 private:
  ExporterConfig config_;
};

}  // namespace Prism

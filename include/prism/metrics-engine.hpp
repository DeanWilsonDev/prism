#pragma once

#include "dependency-graph.hpp"

namespace Prism {

/// Stage 3. Annotates every node in the graph with structural, code, and
/// C++-specific metrics. Metrics that cannot be derived from the available
/// data are left as std::nullopt rather than a sentinel value.
class MetricsEngine {
 public:
  void Annotate(DependencyGraph& graph);

 private:
  void ComputeStructuralMetrics(DependencyGraph& graph);
  void ComputeCodeMetrics(DependencyGraph& graph);
  void ComputeCppMetrics(DependencyGraph& graph);
};

}  // namespace Prism

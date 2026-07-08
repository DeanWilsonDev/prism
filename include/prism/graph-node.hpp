#pragma once

#include <optional>
#include <string>
#include "ast-node.hpp"

namespace Prism {

/// A fully constructed node in the dependency graph. Carries everything that
/// will appear in analysis.json for a single node. Metric fields are optional:
/// std::nullopt means "not computed" — never a sentinel value.
struct GraphNode {
 public:
  std::string id;              // unique stable identifier (see Node Identity)
  std::string name;            // display name
  NodeKind kind;               // from ast-node.hpp
  std::string physicalParent;  // id of parent in the physical tree
  std::string logicalParent;   // id of parent in the logical tree
  std::string file;            // filename this node was declared in
  int line = 0;                // declaration line (0 for project/module/namespace)
  int column = 0;              // declaration column (0 for project/module/namespace)
  int lineEnd = 0;             // last line spanned by the node (0 if unknown)
  std::string referencedName;  // for include/inheritance/composition nodes

  // Structural metrics
  std::optional<int> dependencyCount;
  std::optional<int> dependentCount;
  std::optional<float> couplingScore;
  std::optional<bool> circularDependency;
  std::optional<int> moduleBoundaryViolations;
  std::optional<float> instability;

  // Code metrics
  std::optional<int> linesOfCode;
  std::optional<int> cyclomaticComplexity;
  std::optional<int> methodCount;
  std::optional<int> publicMethodCount;
  std::optional<int> inheritanceDepth;

  // C++ specific metrics
  std::optional<int> includeDepth;
  std::optional<int> transitiveIncludeCount;
  std::optional<float> compileImpact;
  std::optional<float> testCoverage;
};

}  // namespace Prism

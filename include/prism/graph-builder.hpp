#pragma once

#include <string>
#include <vector>
#include "ast-node.hpp"
#include "dependency-graph.hpp"

namespace Prism {

/// Stage 2. Turns the flat list of ASTNodes produced by the parser into a
/// DependencyGraph: project/module/file scaffolding, one node per declaration,
/// and the include/inheritance/composition edges between them.
///
/// The builder expects each ASTNode to carry:
///   - file:           path relative to the project root (POSIX separators)
///   - logicalParent:  the *simple name* of the declaration's semantic parent
///                     (empty when the parent is the translation unit)
///   - referencedName: include target / base class / field type where relevant
class GraphBuilder {
 public:
  explicit GraphBuilder(std::string projectName);
  DependencyGraph Build(const std::vector<ASTNode>& astNodes);

 private:
  std::string projectName_;
};

}  // namespace Prism

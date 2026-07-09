#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

namespace Prism {

enum NodeKind {
  Project,
  Module,
  File,
  Namespace,
  Class,
  Struct,
  ParentClass,
  Include,
  Function,
  Field
};

static const std::unordered_map<std::string, NodeKind> kindMap = {
    {"Namespace", NodeKind::Namespace},
    {"ClassDecl", NodeKind::Class},
    {"StructDecl", NodeKind::Struct},
    {"C++ base class specifier", NodeKind::ParentClass},
    {"CXXMethod", NodeKind::Function},
    {"FunctionDecl", NodeKind::Function},
    {"inclusion directive", NodeKind::Include},
    {"FieldDecl", NodeKind::Field},
};

inline constexpr std::pair<NodeKind, std::string_view> nodeKindNames[] = {
    {NodeKind::Project, "Project"},
    {NodeKind::Module, "Module"},
    {NodeKind::File, "File"},
    {NodeKind::Namespace, "Namespace"},
    {NodeKind::Class, "Class"},
    {NodeKind::Struct, "Struct"},
    {NodeKind::ParentClass, "ParentClass"},
    {NodeKind::Include, "Include"},
    {NodeKind::Function, "Function"},
    {NodeKind::Field, "Field"},
};

inline std::string_view ToStringFromNodeKind(NodeKind kind)
{
  for (const auto& [enumValue, name] : nodeKindNames) {
    if (enumValue == kind) {
      return name;
    }
  }
  return "unknown";
}

struct ASTNode {
 public:
  int id;
  std::string name;
  std::string type;
  NodeKind kind;
  std::string file;
  int line;
  int column;
  int lineEnd = 0;  // last source line spanned by the cursor (0 if unknown)
  std::string physicalParent;
  std::string logicalParent;
  std::string referencedName;
  std::string usr;                // clang USR: stable identity across translation units
  std::string semanticParentUsr;  // USR of the semantic parent (empty at TU root)
};

}  // namespace Prism

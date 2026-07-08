#include "prism/graph-builder.hpp"

#include <filesystem>
#include <map>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include "firefly/log.hpp"

namespace Prism {

namespace {

/// Split a project-relative path into its directory segments plus the filename.
struct PathParts {
  std::vector<std::string> directories;
  std::string filename;
};

PathParts SplitPath(const std::string& relativePath)
{
  PathParts parts;
  std::filesystem::path path(relativePath);
  for (const std::filesystem::path& segment : path.parent_path()) {
    const std::string text = segment.string();
    if (text.empty() || text == "." || text == "..") {
      continue;
    }
    parts.directories.push_back(text);
  }
  parts.filename = path.filename().string();
  return parts;
}

bool IsLogicalContainer(NodeKind kind)
{
  return kind == NodeKind::Namespace || kind == NodeKind::Class || kind == NodeKind::Struct;
}

}  // namespace

GraphBuilder::GraphBuilder(std::string projectName) : projectName_(std::move(projectName)) {}

DependencyGraph GraphBuilder::Build(const std::vector<ASTNode>& astNodes)
{
  DependencyGraph graph;

  // 1. Project root.
  {
    GraphNode project;
    project.id = projectName_;
    project.name = projectName_;
    project.kind = NodeKind::Project;
    graph.AddNode(std::move(project));
  }

  std::unordered_set<std::string> physicalCreated{projectName_};
  std::unordered_set<std::string> declaredIds{projectName_};
  // Simple name -> logical id, so a child can find its semantic parent.
  std::unordered_map<std::string, std::string> logicalNameToId;
  // Filename (basename) -> file node id, for resolving include targets.
  std::unordered_map<std::string, std::string> filenameToFileId;
  // Class/struct simple name -> node id, for inheritance / composition targets.
  std::unordered_map<std::string, std::string> typeNameToId;

  // Ensure a file node (and its module chain) exist. Returns the file node id
  // and the deepest module id (project id when the file sits at the root).
  auto ensureFileNode =
      [&](const std::string& relativePath) -> std::pair<std::string, std::string> {
    const PathParts parts = SplitPath(relativePath);

    std::string accumulated = projectName_;
    std::string parentPhysical = projectName_;
    for (const std::string& segment : parts.directories) {
      accumulated += "::" + segment;
      if (physicalCreated.insert(accumulated).second) {
        GraphNode module;
        module.id = accumulated;
        module.name = segment;
        module.kind = NodeKind::Module;
        module.physicalParent = parentPhysical;
        module.logicalParent = parentPhysical;
        graph.AddNode(std::move(module));
      }
      parentPhysical = accumulated;
    }

    const std::string moduleId = parentPhysical;
    const std::string fileId =
        parts.filename.empty() ? parentPhysical : parentPhysical + "::" + parts.filename;
    if (!parts.filename.empty() && physicalCreated.insert(fileId).second) {
      GraphNode file;
      file.id = fileId;
      file.name = parts.filename;
      file.kind = NodeKind::File;
      file.physicalParent = moduleId;
      file.logicalParent = moduleId;
      file.file = parts.filename;
      graph.AddNode(std::move(file));
      filenameToFileId.emplace(parts.filename, fileId);
    }
    return {fileId, moduleId};
  };

  // Give an id that does not collide with one already emitted.
  auto uniqueId = [&](std::string candidate) -> std::string {
    if (declaredIds.insert(candidate).second) {
      return candidate;
    }
    for (int suffix = 2;; ++suffix) {
      std::string disambiguated = candidate + "::" + std::to_string(suffix);
      if (declaredIds.insert(disambiguated).second) {
        LOG_WARNING("Duplicate node id [{}], disambiguated to [{}]", candidate, disambiguated);
        return disambiguated;
      }
    }
  };

  // First pass: create every declaration node and remember the ids we need for
  // edge resolution. Namespaces/classes precede their children in extraction
  // order, so logicalNameToId is always populated before a child looks it up.
  struct PendingEdge {
    ASTNode source;       // the ASTNode carrying the relationship
    std::string ownerId;  // graph id of the node that owns the relationship
    std::string fileId;   // file the relationship was declared in
  };
  std::vector<PendingEdge> inheritanceEdges;
  std::vector<PendingEdge> compositionEdges;
  std::vector<PendingEdge> includeEdges;

  for (const ASTNode& node : astNodes) {
    const auto [fileId, moduleId] = ensureFileNode(node.file);

    // Resolve the logical parent id.
    std::string logicalParentId = moduleId;
    if (!node.logicalParent.empty()) {
      auto it = logicalNameToId.find(node.logicalParent);
      if (it != logicalNameToId.end()) {
        logicalParentId = it->second;
      }
    }

    GraphNode graphNode;
    graphNode.name = node.name;
    graphNode.kind = node.kind;
    graphNode.file = SplitPath(node.file).filename;
    graphNode.line = node.line;
    graphNode.column = node.column;
    graphNode.lineEnd = (node.lineEnd > 0) ? node.lineEnd : node.line;
    graphNode.referencedName = node.referencedName;
    graphNode.physicalParent = fileId;
    graphNode.logicalParent = logicalParentId;

    std::string baseId = logicalParentId + "::" + (node.name.empty() ? "anonymous" : node.name);
    graphNode.id = uniqueId(baseId);

    if (IsLogicalContainer(node.kind)) {
      logicalNameToId[node.name] = graphNode.id;
    }
    if (node.kind == NodeKind::Class || node.kind == NodeKind::Struct) {
      typeNameToId[node.name] = graphNode.id;
    }

    const std::string createdId = graphNode.id;
    graph.AddNode(std::move(graphNode));

    switch (node.kind) {
      case NodeKind::ParentClass:
        inheritanceEdges.push_back({node, logicalParentId, fileId});
        break;
      case NodeKind::Field:
        compositionEdges.push_back({node, logicalParentId, fileId});
        break;
      case NodeKind::Include:
        includeEdges.push_back({node, createdId, fileId});
        break;
      default:
        break;
    }
  }

  // Second pass: edges. Accumulate weights for repeated (source,target,kind).
  std::map<std::tuple<std::string, std::string, int>, Edge> edgeAccumulator;
  auto addEdge = [&](const std::string& source, const std::string& target, EdgeKind kind) {
    if (source.empty() || target.empty()) {
      return;
    }
    auto key = std::make_tuple(source, target, static_cast<int>(kind));
    auto it = edgeAccumulator.find(key);
    if (it == edgeAccumulator.end()) {
      edgeAccumulator.emplace(key, Edge{source, target, kind, 1});
    }
    else {
      it->second.weight += 1;
    }
  };

  // Include edges: file -> included file (a project file when known).
  for (const PendingEdge& pending : includeEdges) {
    const std::string includedName =
        std::filesystem::path(pending.source.referencedName).filename().string();
    std::string target = pending.source.referencedName;
    auto it = filenameToFileId.find(includedName);
    if (it != filenameToFileId.end()) {
      target = it->second;
    }
    addEdge(pending.fileId, target, EdgeKind::IncludeDependency);
  }

  // Inheritance edges: derived class -> base class (looked up by name).
  for (const PendingEdge& pending : inheritanceEdges) {
    auto it = typeNameToId.find(pending.source.referencedName);
    const std::string target =
        (it != typeNameToId.end()) ? it->second : pending.source.referencedName;
    addEdge(pending.ownerId, target, EdgeKind::Inheritance);
  }

  // Composition edges: owning class -> field type, when the type is a known class.
  for (const PendingEdge& pending : compositionEdges) {
    for (const auto& [typeName, typeId] : typeNameToId) {
      const std::string& referenced = pending.source.referencedName;
      // Match the type name as a whole token within the field's type spelling
      // (e.g. "Prism::TestEquals" or "TestEquals*" both reference TestEquals).
      if (referenced.find(typeName) != std::string::npos && !typeName.empty()) {
        addEdge(pending.ownerId, typeId, EdgeKind::Composition);
        break;
      }
    }
  }

  // TODO: FunctionCall and SymbolUsage edges require call-expression traversal
  // in the parser, which is deferred. No edges of those kinds are emitted yet.

  for (auto& [key, edge] : edgeAccumulator) {
    graph.AddEdge(edge);
  }

  return graph;
}

}  // namespace Prism

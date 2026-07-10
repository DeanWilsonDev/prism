#include "prism/graph-builder.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
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

/// Split a node id ("Project::dirA::dirB::file.cpp") into its "::" segments.
std::vector<std::string> SplitId(const std::string& id)
{
  std::vector<std::string> segments;
  std::size_t start = 0;
  while (true) {
    const std::size_t pos = id.find("::", start);
    if (pos == std::string::npos) {
      segments.push_back(id.substr(start));
      break;
    }
    segments.push_back(id.substr(start, pos - start));
    start = pos + 2;
  }
  return segments;
}

/// Longest shared "::"-delimited prefix of two node ids: their closest common
/// ancestor in the physical/module hierarchy (could be a file, module, or the
/// project root).
std::string CommonAncestorId(const std::string& lhs, const std::string& rhs)
{
  const std::vector<std::string> lhsParts = SplitId(lhs);
  const std::vector<std::string> rhsParts = SplitId(rhs);
  const std::size_t limit = std::min(lhsParts.size(), rhsParts.size());
  std::size_t shared = 0;
  while (shared < limit && lhsParts[shared] == rhsParts[shared]) {
    ++shared;
  }
  std::string joined;
  for (std::size_t index = 0; index < shared; ++index) {
    if (index > 0) {
      joined += "::";
    }
    joined += lhsParts[index];
  }
  return joined;
}

/// A namespace is reopened in every file that contributes to it, so pinning it
/// to whichever file the parser happened to visit first (the default from the
/// main node-creation pass) is arbitrary and can leave a namespace's logical
/// LOC total — gathered from every file that reopens it — nested under a
/// directory that physically holds only a fraction of that code. This second
/// pass re-anchors each namespace to the closest common ancestor (file, module,
/// or project) of everywhere its content actually lives, so the physical tree
/// and the logical tree agree on where a namespace's weight sits.
void ReanchorNamespaces(DependencyGraph& graph)
{
  std::unordered_map<std::string, GraphNode*> byId;
  std::unordered_map<std::string, std::vector<std::string>> logicalChildren;
  for (GraphNode& node : graph.nodes) {
    byId[node.id] = &node;
    if (!node.logicalParent.empty()) {
      logicalChildren[node.logicalParent].push_back(node.id);
    }
  }

  std::unordered_map<std::string, std::optional<std::string>> memo;
  std::function<std::optional<std::string>(const std::string&)> homeOf =
      [&](const std::string& id) -> std::optional<std::string> {
    if (auto cached = memo.find(id); cached != memo.end()) {
      return cached->second;
    }
    memo[id] = std::nullopt;  // guard against namespace-nesting cycles
    std::optional<std::string> home;
    for (const std::string& childId : logicalChildren[id]) {
      GraphNode* child = byId[childId];
      std::optional<std::string> childLocation = (child->kind == NodeKind::Namespace)
                                                     ? homeOf(childId)
                                                     : std::optional(child->physicalParent);
      if (childLocation.has_value() && !childLocation->empty()) {
        home = home.has_value() ? CommonAncestorId(*home, *childLocation) : childLocation;
      }
    }
    memo[id] = home;
    return home;
  };

  for (GraphNode& node : graph.nodes) {
    if (node.kind != NodeKind::Namespace) {
      continue;
    }
    const std::optional<std::string> home = homeOf(node.id);
    if (!home.has_value()) {
      continue;  // an empty namespace: leave its original anchor alone
    }
    node.physicalParent = *home;

    // Only reassign logicalParent when it currently points at a module/project
    // (i.e. this is a top-level namespace); a nested namespace's logicalParent
    // already correctly points at its enclosing namespace via USR and must
    // stay put. logicalParent must land on a module/project, never a file.
    auto currentParent = byId.find(node.logicalParent);
    if (currentParent != byId.end() && (currentParent->second->kind == NodeKind::Module ||
                                        currentParent->second->kind == NodeKind::Project)) {
      GraphNode* homeNode = byId.count(*home) ? byId[*home] : nullptr;
      node.logicalParent = (homeNode != nullptr && homeNode->kind == NodeKind::File)
                               ? homeNode->physicalParent
                               : *home;
    }
  }
}

}  // namespace

GraphBuilder::GraphBuilder(std::string projectName) : projectName_(std::move(projectName)) {}

DependencyGraph GraphBuilder::Build(
    const std::vector<ASTNode>& astNodes, const std::unordered_map<std::string, int>& fileLineCounts
)
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
  // USR -> node id: the primary, cross-TU-stable way to resolve a logical parent.
  std::unordered_map<std::string, std::string> usrToId;
  // Simple name -> logical id: fallback parent resolution when USRs are absent.
  std::unordered_map<std::string, std::string> logicalNameToId;
  // Project-relative path -> file node id, for resolving include targets.
  std::unordered_map<std::string, std::string> relativePathToFileId;
  // Class/struct simple name -> node id: fallback for inheritance / composition
  // targets when the parser supplied no USRs (hand-built ASTNodes in tests).
  std::unordered_map<std::string, std::string> typeNameToId;
  // Ids of the class/struct nodes, so an edge can be restricted to project types.
  std::unordered_set<std::string> typeIds;

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
      if (auto lines = fileLineCounts.find(relativePath);
          lines != fileLineCounts.end() && lines->second > 0) {
        file.linesOfCode = lines->second;
      }
      graph.AddNode(std::move(file));
    }
    if (!parts.filename.empty()) {
      relativePathToFileId[relativePath] = fileId;
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

    // Resolve the logical parent id: prefer the semantic parent's USR (stable
    // across translation units, so a method in a .cpp re-attaches to the class
    // declared in its .h), and fall back to matching the parent's simple name.
    std::string logicalParentId = moduleId;
    if (auto usrIt = usrToId.find(node.semanticParentUsr);
        !node.semanticParentUsr.empty() && usrIt != usrToId.end()) {
      logicalParentId = usrIt->second;
    }
    else if (
        auto nameIt = logicalNameToId.find(node.logicalParent);
        !node.logicalParent.empty() && nameIt != logicalNameToId.end()
    ) {
      logicalParentId = nameIt->second;
    }

    // Include directives belong to the file they appear in: scope them to the
    // file node so the same header included by many files yields distinct,
    // non-colliding node ids (e.g. src::renderer.h::vector, not src::vector).
    if (node.kind == NodeKind::Include) {
      logicalParentId = fileId;
    }

    const bool isAnonymousNamespace = node.kind == NodeKind::Namespace && node.name.empty();

    GraphNode graphNode;
    graphNode.name = isAnonymousNamespace ? "anonymous" : node.name;
    graphNode.kind = node.kind;
    graphNode.file = SplitPath(node.file).filename;
    graphNode.line = node.line;
    graphNode.column = node.column;
    graphNode.lineEnd = (node.lineEnd > 0) ? node.lineEnd : node.line;
    graphNode.referencedName = node.referencedName;
    graphNode.cyclomaticComplexity = node.cyclomaticComplexity;
    graphNode.physicalParent = fileId;
    graphNode.logicalParent = logicalParentId;

    // An anonymous namespace is file-local, and every .cpp in a directory has
    // its own. Scoping the id to the module would collide them all onto one id.
    std::string baseId =
        isAnonymousNamespace
            ? fileId + "::anonymous"
            : logicalParentId + "::" + (node.name.empty() ? "anonymous" : node.name);
    graphNode.id = uniqueId(baseId);

    if (!node.usr.empty()) {
      usrToId.emplace(node.usr, graphNode.id);
    }
    if (IsLogicalContainer(node.kind)) {
      logicalNameToId[node.name] = graphNode.id;
    }
    if (node.kind == NodeKind::Class || node.kind == NodeKind::Struct) {
      typeNameToId[node.name] = graphNode.id;
      typeIds.insert(graphNode.id);
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

  // Include edges: file -> included file. When the include target is a project
  // file that became a node (matched by its project-relative path), the edge
  // connects two file nodes; otherwise the target stays as the external name.
  for (const PendingEdge& pending : includeEdges) {
    std::string target = pending.source.referencedName;
    auto it = relativePathToFileId.find(pending.source.referencedName);
    if (it != relativePathToFileId.end()) {
      target = it->second;
    }
    addEdge(pending.fileId, target, EdgeKind::IncludeDependency);
  }

  // Ids of the class/struct nodes the parser resolved a reference to. Falls back
  // to matching the referenced *name* against known types, which is all a
  // hand-built ASTNode (no USRs) can offer. The name fallback is an exact match:
  // a substring test reports `EdgeKind` as a reference to `Edge`.
  auto resolveReferencedTypes = [&](const ASTNode& source) {
    std::vector<std::string> targets;
    for (const std::string& usr : source.referencedUsrs) {
      auto it = usrToId.find(usr);
      if (it != usrToId.end() && typeIds.count(it->second) != 0) {
        targets.push_back(it->second);
      }
    }
    if (targets.empty() && source.referencedUsrs.empty()) {
      if (auto it = typeNameToId.find(source.referencedName); it != typeNameToId.end()) {
        targets.push_back(it->second);
      }
    }
    return targets;
  };

  // Inheritance edges: derived class -> base class.
  for (const PendingEdge& pending : inheritanceEdges) {
    const std::vector<std::string> targets = resolveReferencedTypes(pending.source);
    if (targets.empty()) {
      // An unresolved base (a system or dependency type) keeps its bare name as
      // the edge target, so the relationship is still visible in the output.
      addEdge(pending.ownerId, pending.source.referencedName, EdgeKind::Inheritance);
      continue;
    }
    for (const std::string& target : targets) {
      addEdge(pending.ownerId, target, EdgeKind::Inheritance);
    }
  }

  // Composition edges: owning class -> each project type its field is built from.
  // A field of type std::vector<GraphNode> composes GraphNode; std::vector itself
  // resolves to no project node and drops out. A class is not composed of itself.
  for (const PendingEdge& pending : compositionEdges) {
    for (const std::string& target : resolveReferencedTypes(pending.source)) {
      if (target != pending.ownerId) {
        addEdge(pending.ownerId, target, EdgeKind::Composition);
      }
    }
  }

  // TODO: FunctionCall and SymbolUsage edges require call-expression traversal
  // in the parser, which is deferred. No edges of those kinds are emitted yet.

  for (auto& [key, edge] : edgeAccumulator) {
    graph.AddEdge(edge);
  }

  ReanchorNamespaces(graph);

  return graph;
}

}  // namespace Prism

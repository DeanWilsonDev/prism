#include "prism/exporter.hpp"

#include <chrono>
#include <ctime>
#include <format>
#include <string>
#include <type_traits>
#include <utility>
#include "firefly/log.hpp"
#include "prism/json-serialization.hpp"

namespace Prism {

namespace {

using Amanuensis::Json;
using Amanuensis::JsonValue;

/// Current UTC time as an ISO 8601 string. Built from std::chrono + std::format
/// on the calendar fields, avoiding reliance on chrono format specifiers that
/// are not yet available in every standard library.
std::string CurrentTimestamp()
{
  const std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &time);
#else
  gmtime_r(&time, &utc);
#endif
  return std::format(
      "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z",
      utc.tm_year + 1900,
      utc.tm_mon + 1,
      utc.tm_mday,
      utc.tm_hour,
      utc.tm_min,
      utc.tm_sec
  );
}

template <typename T>
void AddIfPresent(JsonValue& object, const char* key, const std::optional<T>& value)
{
  if (!value.has_value()) {
    return;
  }
  if constexpr (std::is_same_v<T, bool>) {
    Json::Insert(object, key, ToJsonValue(value.value()));
  }
  else if constexpr (std::is_floating_point_v<T>) {
    Json::Insert(object, key, ToJsonValue(static_cast<double>(value.value())));
  }
  else {
    Json::Insert(object, key, ToJsonValue(static_cast<int>(value.value())));
  }
}

JsonValue SerialiseNode(const GraphNode& node)
{
  JsonValue object = Json::MakeObject();
  Json::Insert(object, "id", ToJsonValue(node.id));
  Json::Insert(object, "name", ToJsonValue(node.name));
  Json::Insert(object, "type", NodeKindToValue(node.kind));
  Json::Insert(object, "physical_parent", ToJsonValue(node.physicalParent));
  Json::Insert(object, "logical_parent", ToJsonValue(node.logicalParent));
  Json::Insert(object, "file", ToJsonValue(node.file));
  Json::Insert(object, "line", ToJsonValue(node.line));
  Json::Insert(object, "column", ToJsonValue(node.column));
  if (!node.referencedName.empty()) {
    Json::Insert(object, "referenced_name", ToJsonValue(node.referencedName));
  }

  AddIfPresent(object, "lines_of_code", node.linesOfCode);
  AddIfPresent(object, "cyclomatic_complexity", node.cyclomaticComplexity);
  AddIfPresent(object, "method_count", node.methodCount);
  AddIfPresent(object, "public_method_count", node.publicMethodCount);
  AddIfPresent(object, "inheritance_depth", node.inheritanceDepth);
  AddIfPresent(object, "dependency_count", node.dependencyCount);
  AddIfPresent(object, "dependent_count", node.dependentCount);
  AddIfPresent(object, "coupling_score", node.couplingScore);
  AddIfPresent(object, "circular_dependency", node.circularDependency);
  AddIfPresent(object, "module_boundary_violations", node.moduleBoundaryViolations);
  AddIfPresent(object, "instability", node.instability);
  AddIfPresent(object, "include_depth", node.includeDepth);
  AddIfPresent(object, "transitive_include_count", node.transitiveIncludeCount);
  AddIfPresent(object, "compile_impact", node.compileImpact);
  AddIfPresent(object, "test_coverage", node.testCoverage);
  return object;
}

JsonValue SerialiseEdge(const Edge& edge)
{
  JsonValue object = Json::MakeObject();
  Json::Insert(object, "source", ToJsonValue(edge.sourceId));
  Json::Insert(object, "target", ToJsonValue(edge.targetId));
  Json::Insert(object, "type", ToJsonValue(std::string(ToStringFromEdgeKind(edge.kind))));
  Json::Insert(object, "weight", ToJsonValue(edge.weight));
  return object;
}

}  // namespace

Exporter::Exporter(ExporterConfig config) : config_(std::move(config)) {}

bool Exporter::Export(const DependencyGraph& graph)
{
  int fileCount = 0;
  for (const GraphNode& node : graph.nodes) {
    if (node.kind == NodeKind::File) {
      ++fileCount;
    }
  }

  JsonValue metadata = Json::MakeObject();
  Json::Insert(metadata, "project", ToJsonValue(config_.projectName));
  Json::Insert(metadata, "generated_at", ToJsonValue(CurrentTimestamp()));
  Json::Insert(metadata, "prism_version", ToJsonValue(config_.prismVersion));
  Json::Insert(metadata, "file_count", ToJsonValue(fileCount));
  Json::Insert(metadata, "node_count", ToJsonValue(static_cast<int>(graph.nodes.size())));
  Json::Insert(metadata, "edge_count", ToJsonValue(static_cast<int>(graph.edges.size())));

  JsonValue nodes = Json::MakeArray();
  for (const GraphNode& node : graph.nodes) {
    Json::PushBack(nodes, SerialiseNode(node));
  }

  JsonValue edges = Json::MakeArray();
  for (const Edge& edge : graph.edges) {
    Json::PushBack(edges, SerialiseEdge(edge));
  }

  JsonValue document = Json::MakeObject();
  Json::Insert(document, "metadata", std::move(metadata));
  Json::Insert(document, "nodes", std::move(nodes));
  Json::Insert(document, "edges", std::move(edges));

  // WriterOptions default to pretty printing with a two-space indent and a
  // trailing newline, matching Prism's previous output shape.
  if (!Amanuensis::Writer::WriteToFile(document, config_.outputPath)) {
    LOG_ERROR("Failed to write output file: {}", config_.outputPath.string());
    return false;
  }
  return true;
}

}  // namespace Prism

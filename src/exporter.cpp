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

using Amanuensis::Value;

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
void AddIfPresent(Value& object, const char* key, const std::optional<T>& value)
{
  if (!value.has_value()) {
    return;
  }
  if constexpr (std::is_same_v<T, bool>) {
    object.Insert(key, Value(value.value()));
  }
  else if constexpr (std::is_floating_point_v<T>) {
    object.Insert(key, Value(static_cast<double>(value.value())));
  }
  else {
    object.Insert(key, Value(static_cast<int>(value.value())));
  }
}

Value SerialiseNode(const GraphNode& node)
{
  Value object = Value::MakeObject();
  object.Insert("id", Value(node.id));
  object.Insert("name", Value(node.name));
  object.Insert("type", NodeKindToValue(node.kind));
  object.Insert("physical_parent", Value(node.physicalParent));
  object.Insert("logical_parent", Value(node.logicalParent));
  object.Insert("file", Value(node.file));
  object.Insert("line", Value(node.line));
  object.Insert("column", Value(node.column));
  if (!node.referencedName.empty()) {
    object.Insert("referenced_name", Value(node.referencedName));
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

Value SerialiseEdge(const Edge& edge)
{
  Value object = Value::MakeObject();
  object.Insert("source", Value(edge.sourceId));
  object.Insert("target", Value(edge.targetId));
  object.Insert("type", Value(std::string(ToStringFromEdgeKind(edge.kind))));
  object.Insert("weight", Value(edge.weight));
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

  Value metadata = Value::MakeObject();
  metadata.Insert("project", Value(config_.projectName));
  metadata.Insert("generated_at", Value(CurrentTimestamp()));
  metadata.Insert("prism_version", Value(config_.prismVersion));
  metadata.Insert("file_count", Value(fileCount));
  metadata.Insert("node_count", Value(static_cast<int>(graph.nodes.size())));
  metadata.Insert("edge_count", Value(static_cast<int>(graph.edges.size())));

  Value nodes = Value::MakeArray();
  for (const GraphNode& node : graph.nodes) {
    nodes.PushBack(SerialiseNode(node));
  }

  Value edges = Value::MakeArray();
  for (const Edge& edge : graph.edges) {
    edges.PushBack(SerialiseEdge(edge));
  }

  Value document = Value::MakeObject();
  document.Insert("metadata", std::move(metadata));
  document.Insert("nodes", std::move(nodes));
  document.Insert("edges", std::move(edges));

  // WriterOptions default to pretty printing with a two-space indent and a
  // trailing newline, matching Prism's previous output shape.
  if (!Amanuensis::Writer::WriteToFile(document, config_.outputPath)) {
    LOG_ERROR("Failed to write output file: {}", config_.outputPath.string());
    return false;
  }
  return true;
}

}  // namespace Prism

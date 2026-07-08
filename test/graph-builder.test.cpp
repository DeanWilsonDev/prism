#include <cimmerian/test.hpp>

#include <string>
#include <vector>
#include "prism/ast-node.hpp"
#include "prism/graph-builder.hpp"
#include "prism/logging/log.hpp"

namespace {

const Prism::GraphNode* FindByName(
    const Prism::DependencyGraph& graph, Prism::NodeKind kind, const std::string& name
)
{
  for (const Prism::GraphNode& node : graph.nodes) {
    if (node.kind == kind && node.name == name) {
      return &node;
    }
  }
  return nullptr;
}

Prism::DependencyGraph BuildFixtureGraph()
{
  // Two declarations in src/geometry/shapes.cpp: namespace Geometry and a class
  // Rectangle inside it. No parser involved.
  Prism::ASTNode namespaceNode;
  namespaceNode.id = 0;
  namespaceNode.name = "Geometry";
  namespaceNode.kind = Prism::NodeKind::Namespace;
  namespaceNode.file = "src/geometry/shapes.cpp";
  namespaceNode.logicalParent = "";  // translation-unit level

  Prism::ASTNode baseNode;
  baseNode.id = 1;
  baseNode.name = "Shape";
  baseNode.kind = Prism::NodeKind::Class;
  baseNode.file = "src/geometry/shapes.cpp";
  baseNode.logicalParent = "Geometry";

  Prism::ASTNode classNode;
  classNode.id = 2;
  classNode.name = "Rectangle";
  classNode.kind = Prism::NodeKind::Class;
  classNode.file = "src/geometry/shapes.cpp";
  classNode.logicalParent = "Geometry";
  classNode.line = 12;
  classNode.column = 3;

  // Rectangle : Shape — a base specifier whose traversal parent is Rectangle.
  Prism::ASTNode parentNode;
  parentNode.id = 3;
  parentNode.name = "Shape";
  parentNode.kind = Prism::NodeKind::ParentClass;
  parentNode.file = "src/geometry/shapes.cpp";
  parentNode.logicalParent = "Rectangle";
  parentNode.referencedName = "Shape";

  Prism::GraphBuilder builder("TestProject");
  return builder.Build({namespaceNode, baseNode, classNode, parentNode});
}

// IT() bodies are captureless lambdas; share the built graph via a static.
const Prism::DependencyGraph& SharedGraph()
{
  static const Prism::DependencyGraph graph = []() {
    Prism::Logging::Log::Init();
    return BuildFixtureGraph();
  }();
  return graph;
}

}  // namespace

DESCRIBE("GraphBuilder", {
  IT("creates a module node for each directory segment", {
    const Prism::GraphNode* src = SharedGraph().FindNodeById("TestProject::src");
    const Prism::GraphNode* geometry = SharedGraph().FindNodeById("TestProject::src::geometry");
    ASSERT_TRUE(src != nullptr);
    ASSERT_TRUE(geometry != nullptr);
    ASSERT_TRUE(src != nullptr && src->kind == Prism::NodeKind::Module);
    ASSERT_TRUE(geometry != nullptr && geometry->kind == Prism::NodeKind::Module);
  });

  IT("nests the file node under the deepest module", {
    const Prism::GraphNode* file = FindByName(SharedGraph(), Prism::NodeKind::File, "shapes.cpp");
    ASSERT_TRUE(file != nullptr);
    ASSERT_EQUAL(std::string("TestProject::src::geometry"), file->physicalParent);
  });

  IT("points a class physicalParent at its file node", {
    const Prism::GraphNode* file = FindByName(SharedGraph(), Prism::NodeKind::File, "shapes.cpp");
    const Prism::GraphNode* rectangle =
        FindByName(SharedGraph(), Prism::NodeKind::Class, "Rectangle");
    ASSERT_TRUE(file != nullptr);
    ASSERT_TRUE(rectangle != nullptr);
    ASSERT_EQUAL(file->id, rectangle->physicalParent);
  });

  IT("points a class logicalParent at its namespace node", {
    const Prism::GraphNode* geometry =
        FindByName(SharedGraph(), Prism::NodeKind::Namespace, "Geometry");
    const Prism::GraphNode* rectangle =
        FindByName(SharedGraph(), Prism::NodeKind::Class, "Rectangle");
    ASSERT_TRUE(geometry != nullptr);
    ASSERT_TRUE(rectangle != nullptr);
    ASSERT_EQUAL(geometry->id, rectangle->logicalParent);
  });

  IT("builds an inheritance edge from the derived class to its base", {
    const Prism::GraphNode* rectangle =
        FindByName(SharedGraph(), Prism::NodeKind::Class, "Rectangle");
    const Prism::GraphNode* shape = FindByName(SharedGraph(), Prism::NodeKind::Class, "Shape");
    ASSERT_TRUE(rectangle != nullptr);
    ASSERT_TRUE(shape != nullptr);

    bool found = false;
    for (const Prism::Edge& edge : SharedGraph().edges) {
      if (edge.kind == Prism::EdgeKind::Inheritance && edge.sourceId == rectangle->id &&
          edge.targetId == shape->id) {
        found = true;
      }
    }
    ASSERT_TRUE(found);
  });
});

# Prism — Implementation Specification

> This document is an implementation spec for Claude Code. It describes the full Prism codebase:
> what already exists, what needs to be built, and how every piece should behave. Follow it precisely.
> Do not invent architecture — implement what is described here.

---

## Project Summary

Prism is a standalone, headless C++ analysis engine. It accepts a C++ repository as input, traverses
its AST using libclang, and produces a structured `analysis.json` file describing the codebase. It has
no UI, no rendering, and no window management. It runs as a CLI tool.

The output is consumed by a separate tool called Pharos. Prism has no knowledge of Pharos. The boundary
between them is the `analysis.json` file.

---

## Tech Stack

| Component         | Technology                                    |
|-------------------|-----------------------------------------------|
| Language          | C++20                                         |
| Build system      | CMake (minimum 3.20)                          |
| AST parser        | libclang (stable C API, not the C++ API)      |
| JSON serialisation| nlohmann/json (via FetchContent, tag v3.12.0) |
| Testing           | Cimmerian (via FetchContent, GitHub: DeanWilsonDev/Cimmerian) |
| Logging           | Firefly (via FetchContent, GitHub: DeanWilsonDev/firefly)     |
| Platform          | Linux (primary), macOS, Windows (secondary)   |
| Compiler minimum  | GCC 13+, Clang 16+, or MSVC 19.29+ (std::format required)    |

---

## What Already Exists

The following files exist and must be preserved. Read each one before touching anything near it.

### `CMakeLists.txt`
Complete and functional. FetchContent declarations for Firefly, Cimmerian, nlohmann/json are present.
Both `prism` and `prism-tests` executables are declared. `libclang` is linked. Do not restructure this
file — only add to it if new source files are created.

### `include/prism/ast-node.hpp`
Defines `Prism::ASTNode`, `Prism::NodeKind` enum, `kindMap` (string → NodeKind), `nodeKindNames`
(constexpr array for enum-to-string), and `ToStringFromNodeKind()`. This file is the foundation of
Stage 1. Do not move `NodeKind` or `nodeKindNames` out of this file — they are intentionally colocated.

**Known issue:** `ToStringFromNodeKind()` is defined inline in the header without `inline`. This will
cause ODR violations if the header is included in multiple translation units. Fix this by marking it
`inline`.

### `include/prism/json-serialization.hpp`
Contains:
```cpp
#include <nlohmann/json.hpp>
#include "ast-node.hpp"
#include "nlohmann/detail/macro_scope.hpp"

NLOHMANN_JSON_SERIALIZE_ENUM(Prism::NodeKind, Prism::nodeKindNames)
```
This file exists to keep nlohmann out of `ast-node.hpp`. The `NLOHMANN_JSON_SERIALIZE_ENUM` macro
expects a specific format. The `nodeKindNames` array in `ast-node.hpp` uses `std::pair<NodeKind,
std::string_view>` — verify this is compatible with the macro's expected format. If not, provide a
separate mapping array in this file in the format the macro requires, and leave `nodeKindNames` in
`ast-node.hpp` for `ToStringFromNodeKind()` only.

### `include/pch.hpp`
Precompiled header. Includes Firefly log, nlohmann/json, and standard headers. Do not remove includes
from this file.

### `src/main.cpp`
Currently contains exploratory/prototype code: a hardcoded file path, inline `BuildAstNode()`, inline
`Visitor()`, and a `main()` that parses a single hardcoded translation unit. This is proof-of-concept
code. It must be replaced with a proper CLI entry point (see Stage 1 below). The prototype logic is
useful reference — read it before discarding.

### `src/example/test-input.cpp` and `src/example/test-input.hpp`
A small C++ fixture used as parse input during development. Preserve as-is. It is referenced in
`CMakeLists.txt` and used to validate parser output.

### `include/prism/logging/log.hpp` and `include/prism/logging/log.cpp`
Firefly logging wrapper. Functional. Do not modify.

### `test/test-main.cpp`
Contains `#include <cimmerian/test-entry-point.hpp>`. This is the required entry point for Cimmerian.
Do not change it.

### `test/parser.test.cpp`, `test/graph-builder.test.cpp`, `test/exporter.test.cpp`
Currently empty. Implement tests here (see Testing section).

### `.clang-format`, `.clang-tidy`, `.clangd`
Tooling config. Do not modify.

---

## What Needs to Be Built

The following files are either empty stubs or do not yet exist. Implement them in the order listed.

---

## Data Structures

### `include/prism/ast-node.hpp` — already exists, but verify

`ASTNode` has these fields:
```
int id
std::string name
std::string type
NodeKind kind
std::string file
int line
int column
std::string physicalParent
std::string logicalParent
std::string referencedName
```

No changes to the struct itself are required. Fix the `inline` issue on `ToStringFromNodeKind()`.

---

### `include/prism/graph-node.hpp` — implement

`GraphNode` represents a fully constructed node in the dependency graph. It carries all data that will
appear in `analysis.json` for a single node. Fields:

```
std::string id              — unique stable identifier (see Node Identity below)
std::string name            — display name
NodeKind kind               — from ast-node.hpp
std::string physicalParent  — id of parent in the physical tree (Project → Module → File)
std::string logicalParent   — id of parent in the logical tree (Project → Module → Namespace → Class → Function)
std::string file            — filename this node was declared in (empty for project/module nodes)
int line                    — declaration line (0 for project/module/namespace nodes)
int column                  — declaration column (0 for project/module/namespace nodes)
std::string referencedName  — for include/inheritance/composition nodes, the target name
```

Metric fields — all optional (use `std::optional<T>`):
```
std::optional<int> linesOfCode
std::optional<int> cyclomaticComplexity
std::optional<int> methodCount
std::optional<int> publicMethodCount
std::optional<int> inheritanceDepth
std::optional<int> dependencyCount
std::optional<int> dependentCount
std::optional<float> couplingScore
std::optional<bool> circularDependency
std::optional<int> moduleBoundaryViolations
std::optional<float> instability
std::optional<int> includeDepth
std::optional<int> transitiveIncludeCount
std::optional<float> compileImpact
std::optional<float> testCoverage
```

---

### `include/prism/dependency-graph.hpp` — implement

The `DependencyGraph` is the central data model passed between Stages 2, 3, and 4.

`Edge` struct:
```
std::string sourceId
std::string targetId
EdgeKind kind
int weight
```

`EdgeKind` enum:
```
IncludeDependency
Inheritance
SymbolUsage
FunctionCall
Composition
```

Provide `ToStringFromEdgeKind()` following the same `constexpr` array pattern used for `NodeKind`.

`DependencyGraph` struct:
```
std::vector<GraphNode> nodes
std::vector<Edge> edges
```

Provide methods:
- `void AddNode(GraphNode node)`
- `void AddEdge(Edge edge)`
- `GraphNode* FindNodeById(const std::string& id)` — returns nullptr if not found
- `std::vector<Edge> GetOutgoingEdges(const std::string& nodeId) const`
- `std::vector<Edge> GetIncomingEdges(const std::string& nodeId) const`

---

### `include/prism/parser.hpp` and `src/parser.cpp` — implement

`ParseResult` struct:
```
std::vector<ASTNode> nodes
std::vector<std::string> warnings
bool hadErrors
```

`ParserConfig` struct:
```
std::filesystem::path projectRoot
std::filesystem::path compileCommandsPath
bool verbose
```

`Parser` class:
```cpp
class Parser {
public:
  explicit Parser(ParserConfig config);
  ParseResult Parse();
private:
  // internal helpers
};
```

**Implementation requirements:**

`Parse()` must:
1. Load `compile_commands.json` using `clang_CompilationDatabase_fromDirectory`. If loading fails, log
   an error and return an empty result with `hadErrors = true`.
2. Iterate every translation unit in the database using `clang_CompilationDatabase_getAllCompileCommands`.
3. For each translation unit, call `clang_parseTranslationUnit` with
   `CXTranslationUnit_DetailedPreprocessingRecord` (required for include directives).
4. If parsing produces errors (check `clang_getNumDiagnostics` for error-level diagnostics), log a
   warning and continue — do not abort. Add a string to `warnings`.
5. Traverse the AST with `clang_visitChildren` and a visitor function. The visitor must:
   - Skip cursors where `clang_Location_isFromMainFile` returns false.
   - Handle these cursor kinds: `CXCursor_Namespace`, `CXCursor_ClassDecl`, `CXCursor_StructDecl`,
     `CXCursor_FunctionDecl`, `CXCursor_CXXMethod`, `CXCursor_FieldDecl`,
     `CXCursor_InclusionDirective`, `CXCursor_CXXBaseSpecifier`.
   - For each, construct an `ASTNode` and append it to the result list.
   - Return `CXChildVisit_Recurse` for all handled kinds.
6. For `logicalParent`: use `clang_getCursorSemanticParent()` on the cursor. Spell the parent cursor
   with `clang_getCursorSpelling`. If the parent is the translation unit root, `logicalParent` is
   the module name.
7. For `physicalParent`: derive from the file path relative to `projectRoot`. The physical parent of
   any declaration inside a file is that file's node id. Files are children of module nodes. Module
   nodes are derived from folder segments relative to `projectRoot` (see Module Detection below).
8. For `referencedName`: on `CXCursor_InclusionDirective`, use `clang_getIncludedFile` and spell it.
   On `CXCursor_CXXBaseSpecifier`, use `clang_getCursorSpelling`. On `CXCursor_FieldDecl`, use
   `clang_getTypeSpelling(clang_getCursorType(cursor))` to capture the type name (for composition).
9. Assign sequential integer `id` values to each node in extraction order.
10. Dispose of each `CXTranslationUnit` after visiting. Dispose of the index at the end.

**Module detection during parsing:**

Given a file path relative to `projectRoot`, the module hierarchy is the sequence of directory
segments. For a file at `src/physics/collision.cpp` relative to project root, the module chain is
`[src, physics]` and the file is `collision.cpp`. Each directory segment becomes a module node id
of the form `ProjectName::segmentA::segmentB`. The physical parent of the file node is the deepest
module. If the file is at root level, its physical parent is the project node.

---

### `include/prism/graph-builder.hpp` and `src/graph-builder.cpp` — create these files

**Note:** The design document calls this "Stage 2 — Dependency Graph Builder" but the source tree has
`src/dependency-graph.cpp`. Put the graph building logic in a new `GraphBuilder` class in
`src/graph-builder.cpp`. `src/dependency-graph.cpp` should only contain `DependencyGraph` method
implementations.

`GraphBuilder` class:
```cpp
class GraphBuilder {
public:
  explicit GraphBuilder(std::string projectName);
  DependencyGraph Build(const std::vector<ASTNode>& astNodes);
private:
  // internal helpers
};
```

**Implementation requirements:**

`Build()` must:

1. Create a project-level `GraphNode` with `kind = NodeKind::Project`, `id = projectName`,
   `physicalParent = ""`, `logicalParent = ""`.

2. Walk the `ASTNode` list. For each node, derive or look up the module and file `GraphNode` it
   belongs to, creating them on first encounter. Module nodes have `kind = NodeKind::Module`. File
   nodes have `kind = NodeKind::Include` — **no**, file nodes should use a `File` NodeKind.

   **File NodeKind gap:** `NodeKind` in `ast-node.hpp` does not currently have a `File` entry. Add
   `File` to the `NodeKind` enum and to `nodeKindNames`. Do not rename or remove existing values.

3. For each `ASTNode` of kind `Namespace`, `Class`, `Struct`, `Function`, `Field`, `ParentClass`,
   or `Include`, create a corresponding `GraphNode` using the same id scheme (see Node Identity).

4. Set `physicalParent` on each `GraphNode` correctly:
   - Project node: empty string.
   - Module node at top level: project node id.
   - Module node nested: parent module node id.
   - File node: deepest module node id (or project node if file is at root).
   - All other nodes: the file node id they were declared in.

5. Set `logicalParent` on each `GraphNode` correctly:
   - Namespace node: module node id (or project if no module).
   - Class/Struct node: namespace node id, or module node id if in root namespace.
   - Function/Method node: class node id (if a method) or namespace node id (if free function).
   - Field node: class node id it belongs to.

6. Build `Edge` entries:
   - For each `ASTNode` with kind `Include`: add an `Edge` of kind `IncludeDependency` from the
     file node to the included file node. Weight = 1 (accumulate if the same include appears
     multiple times).
   - For each `ASTNode` with kind `ParentClass`: add an `Edge` of kind `Inheritance` from the class
     node to the parent class node. Look up the parent class by `referencedName`.
   - For each `ASTNode` with kind `Field` where `referencedName` refers to a known class: add an
     `Edge` of kind `Composition`.
   - Function call edges (`FunctionCall`, `SymbolUsage`) are deferred — insert as empty stubs for
     now and note them in the implementation with a TODO comment.

7. Deduplicate nodes by id — if two `ASTNode` entries produce the same id, keep one and log a
   warning (see Node Identity for the collision fallback).

---

### Node Identity

Node ids must be stable, unique, and human-readable. Use the fully-qualified path through the
appropriate hierarchy, separated by `::`.

Examples:
- Project: `UmbraEngine`
- Module: `UmbraEngine::Core::Math`
- File: `UmbraEngine::Core::Math::geometry.hpp`
- Namespace: `UmbraEngine::Geometry`
- Class: `UmbraEngine::Geometry::Rectangle`
- Method: `UmbraEngine::Geometry::Rectangle::GetArea`

If two nodes would produce the same id, append `::2`, `::3`, etc. to disambiguate. Log a warning
when this happens.

---

### `include/prism/metrics-engine.hpp` and `src/metrics-engine.cpp` — implement

`MetricsEngine` class:
```cpp
class MetricsEngine {
public:
  void Annotate(DependencyGraph& graph);
private:
  void ComputeStructuralMetrics(DependencyGraph& graph);
  void ComputeCodeMetrics(DependencyGraph& graph);
  void ComputeCppMetrics(DependencyGraph& graph);
};
```

**Implementation requirements:**

`ComputeStructuralMetrics()` must compute for every node in the graph:
- `dependencyCount` = number of outgoing edges from that node.
- `dependentCount` = number of incoming edges to that node.
- `instability` = dependencyCount / (dependencyCount + dependentCount). If both are 0, omit.
- `couplingScore` = dependencyCount / total node count. Normalised 0–1.
- `circularDependency` = whether the node participates in any cycle. Use DFS cycle detection.

`ComputeCodeMetrics()` must compute for every node where the source file is available:
- `linesOfCode`: count lines in the source file for the range spanned by the node. This requires
  storing start/end line on `GraphNode`. Add `int lineEnd` to `GraphNode`.
- `methodCount` and `publicMethodCount`: count child nodes of kind `Function` in the graph.
  Public/private distinction is not captured in the current `ASTNode` — omit `publicMethodCount`
  for now with a TODO.
- `inheritanceDepth`: follow `Inheritance` edges upward and count depth.

`ComputeCppMetrics()` must compute for file nodes:
- `includeDepth`: follow `IncludeDependency` edges recursively and find the maximum depth.
- `transitiveIncludeCount`: count unique reachable file nodes via `IncludeDependency` edges.
- `compileImpact`: count nodes that have a transitive incoming `IncludeDependency` path to this
  file. Expressed as a raw count (not a ratio).

Metrics that cannot be computed must be left as `std::nullopt` — never set to 0 as a sentinel.

---

### `include/prism/exporter.hpp` and `src/exporter.cpp` — implement

`ExporterConfig` struct:
```
std::filesystem::path outputPath
std::string projectName
std::string prismVersion   // e.g. "0.1.0"
```

`Exporter` class:
```cpp
class Exporter {
public:
  explicit Exporter(ExporterConfig config);
  bool Export(const DependencyGraph& graph);
private:
  nlohmann::json SerialiseNode(const GraphNode& node) const;
  nlohmann::json SerialiseEdge(const Edge& edge) const;
};
```

**Implementation requirements:**

`Export()` must produce a JSON file at `outputPath` with this top-level structure:
```json
{
  "metadata": {
    "project": "<projectName>",
    "generated_at": "<ISO 8601 timestamp>",
    "prism_version": "<prismVersion>",
    "file_count": <int>,
    "node_count": <int>,
    "edge_count": <int>
  },
  "nodes": [ ... ],
  "edges": [ ... ]
}
```

`file_count` = count of nodes with kind `File`.
`node_count` = total node count.
`edge_count` = total edge count.

`SerialiseNode()` must output all non-nullopt fields only. Use `std::string_view` from
`ToStringFromNodeKind()` for the `"type"` field. Never output integer enum values.

Node JSON shape:
```json
{
  "id": "...",
  "name": "...",
  "type": "class",
  "physical_parent": "...",
  "logical_parent": "...",
  "file": "...",
  "line": 12,
  "column": 3,
  "lines_of_code": 80,
  "method_count": 5,
  "dependency_count": 3,
  "dependent_count": 2,
  "coupling_score": 0.15,
  "instability": 0.6,
  "circular_dependency": false
}
```

Omit any key where the corresponding `std::optional` is `std::nullopt`.

`SerialiseEdge()` must output:
```json
{
  "source": "...",
  "target": "...",
  "type": "include_dependency",
  "weight": 1
}
```

Use snake_case for edge type strings. Provide `ToStringFromEdgeKind()` following the same pattern as
`ToStringFromNodeKind()`.

The nlohmann dependency must not leak outside `exporter.cpp` and `json-serialization.hpp`. No other
translation unit should `#include <nlohmann/json.hpp>` directly.

Use `std::chrono` and `std::format` to produce the ISO 8601 timestamp for `generated_at`.

Return `false` and log an error if the output file cannot be opened for writing.

---

### `src/main.cpp` — replace with proper CLI entry point

Replace the existing prototype content with a proper `main()` that:

1. Parses CLI arguments. Required flags: `--project <path>`. Optional: `--output <path>` (default
   `./analysis.json`), `--compile-commands <path>` (default `<project>/compile_commands.json`),
   `--verbose`. Use manual `argv` parsing — do not introduce a new dependency for CLI parsing.

2. Validates that `--project` path exists and is a directory. Exits with code 1 and an error message
   if not.

3. Constructs a `ParserConfig` and calls `Parser::Parse()`.

4. Constructs a `GraphBuilder` and calls `Build()` on the parse result.

5. Constructs a `MetricsEngine` and calls `Annotate()` on the graph.

6. Constructs an `Exporter` and calls `Export()` on the graph.

7. Exits with code 0 on success, code 1 on any pipeline failure.

Log progress at each stage if `--verbose` is set.

---

## CMake Updates Required

Add `src/graph-builder.cpp` to the `prism` executable sources in `CMakeLists.txt`. The file does not
exist yet and is not listed. Add it alongside the other `src/` files.

Add `test/metrics-engine.test.cpp` if it is implemented (the file is declared in `CMakeLists.txt`
already but the file does not exist — create it even if empty to avoid a build error).

---

## Testing

Tests use Cimmerian. The test entry point is `test/test-main.cpp` (already correct — do not change).

For each test file, the include is:
```cpp
#include <cimmerian/test.hpp>
```

Test structure:
```cpp
DESCRIBE("ComponentName", {
  IT("does the thing", {
    EXPECT(someValue).ToEqual(expectedValue);
  });
});
```

### `test/parser.test.cpp`

Write tests that:
- Construct a `Parser` pointed at `src/example/` (the test fixture).
- Call `Parse()` and verify the result is non-empty and `hadErrors` is false.
- Verify that a `Class` node named `TestInput` is present.
- Verify that a `Namespace` node named `Tester` is present.
- Verify that a `ParentClass` node referencing `TestInputBase` is present.
- Verify that an `Include` node referencing `test-input.hpp` is present.

### `test/graph-builder.test.cpp`

Write tests that:
- Build a `DependencyGraph` from a small hand-constructed `ASTNode` list (do not call the parser).
- Verify that module nodes are created for each unique directory segment.
- Verify that file nodes are children of the correct module node.
- Verify that class nodes have `physicalParent` pointing to their file node.
- Verify that class nodes have `logicalParent` pointing to their namespace node.

### `test/exporter.test.cpp`

Write tests that:
- Construct a minimal `DependencyGraph` with one project node, one module node, one class node, and
  one edge.
- Call `Export()` with a temp path.
- Read the output file back and verify the JSON structure matches the schema.
- Verify that omitted optional fields are absent from the JSON (not present as null).

---

## Constraints and Rules

- nlohmann/json must not appear in any header other than `json-serialization.hpp`. It must not leak
  into `graph-node.hpp`, `dependency-graph.hpp`, `parser.hpp`, or `exporter.hpp`. It belongs only in
  `exporter.cpp` and `json-serialization.hpp`.
- All enum-to-string conversions use the `constexpr` array + linear scan pattern established by
  `ToStringFromNodeKind()`. Do not use `std::map` or `switch` statements for this purpose.
- All file paths passed between components use `std::filesystem::path`.
- Metric fields use `std::optional<T>`. Never use sentinel values (0, -1, empty string) to represent
  "not computed".
- Parse errors do not abort the pipeline. Log and continue.
- The Parser must dispose of every `CXTranslationUnit` and `CXIndex` it creates.
- Do not modify `src/example/test-input.cpp` or `src/example/test-input.hpp`.
- Do not modify `.clang-format`, `.clang-tidy`, or `.clangd`.
- Format all new code with `clang-format` using the existing `.clang-format` config.

---

## Planned Enhancements (v0.2)

> Status: **implemented.** These were identified after running Prism against a real project
> (`vulkan-3d-orbit-viewer-poc`, ~258 translation units). The v0.1 pipeline ran cleanly and produced
> valid output, but two behaviours made the analysis far less useful on a conventional
> header/source-split C++ project than on the self-contained `test-input.cpp` fixture. Both are now
> implemented; see the **As-built notes** at the end of this section for refinements discovered during
> implementation, and `docs/decisions.md` for outcomes.

### Motivation (observed on vulkan-3d-orbit-viewer-poc)

- **Header-blindness.** The visitor skips every cursor where `clang_Location_isFromMainFile` is false,
  and file nodes are only created for translation units (`.cpp`/`.c`). Because C++ declares its classes
  and APIs in headers, the result was: **0 `File` nodes for any header**, all header-declared classes
  (`Camera`, `Renderer`, `Swapchain`, `Mesh`, …) **missing**, all 72 method definitions **flattened to
  the module** (their `logical_parent` fell back to `src` because the owning class was not a node), and
  the **internal include graph empty** (all 78 `src` include edges pointed at unresolved bare-string
  header names because headers were not nodes).
- **Dependency noise.** The compilation database bundles vendored dependencies (SDL3, glm,
  tinyobjloader). Prism analysed all of them; files outside the project root collapsed to bare
  filenames, producing **421 duplicate-id warnings** and burying the project's ~171 own nodes among
  1835 total.

Both behaviours are technically consistent with the v0.1 spec. v0.2 deliberately changes them.

### Enhancement B — Scope extraction to the project root

**Goal.** By default, analyse only files that live under `--project`. Dependency and system code is
excluded, not flattened.

**New concept — the in-scope predicate.** A file is *in scope* when its real (canonicalised) path is
lexically under the canonicalised `projectRoot`. Implement one helper in `parser.cpp`:

```cpp
// true when `absolutePath` is inside `projectRoot`
bool IsInProjectScope(const std::string& absolutePath, const std::filesystem::path& projectRoot);
```
Use `weakly_canonical` on both sides and test that the relative path does not begin with `..`. This is
the same computation `RelativePath()` already performs; factor the shared logic so both use it.

**Parser changes (`src/parser.cpp`):**

1. **Skip out-of-root translation units.** Before parsing a compile command, resolve its source file to
   an absolute path and skip the whole TU when it is not in scope — unless `--include-external` is set
   (see CLI below). Skipped TUs are not counted as parse warnings.
2. **Replace the main-file filter with the scope filter** in the visitor. The current
   `clang_Location_isFromMainFile(location)` guard becomes `IsInProjectScope(cursorFilePath, root)`.
   This is what makes project **headers** contribute declarations while still excluding `<vector>`,
   SDL, glm, etc. (Enhancement A depends on this.)

**CLI changes (`src/main.cpp`):**

- Add `--include-external` (default off). When set, restore v0.1 behaviour: parse every TU in the
  database and capture declarations from any file (scope filter disabled). Thread a
  `bool includeExternal` through `ParserConfig`.

**Expected result.** On vulkan-3d-orbit-viewer-poc the duplicate-id warnings from dependency files
disappear, and the node set is dominated by the project's own code.

### Enhancement A — Make headers first-class

**Goal.** Capture declarations from project headers, create `File` nodes for headers, connect the
internal include graph (`.cpp → .h`, `.h → .h`), and re-attach methods to their declaring class.

This depends on Enhancement B: once the visitor captures in-scope (not just main-file) cursors, header
declarations flow in automatically. The remaining work is **cross-translation-unit de-duplication** and
**robust parent resolution**, because a header included by *N* translation units will have every one of
its declarations visited *N* times.

**Data-structure changes:**

- Add two fields to `ASTNode` (`include/prism/ast-node.hpp`):
  ```
  std::string usr             // clang_getCursorUSR of this cursor (stable cross-TU identity)
  std::string semanticParentUsr  // USR of the semantic parent (empty at TU root)
  ```
  Adding fields is permitted (v0.1 already added `lineEnd`). Do not remove existing fields.

**Parser changes (`src/parser.cpp`):**

1. Populate `usr` via `clang_getCursorUSR(cursor)` and `semanticParentUsr` from the semantic parent's
   USR. For `ParentClass` (base specifiers) keep using the traversal `parent` cursor for logical
   parenting, but record its USR.
2. **De-duplicate across TUs.** Keep an `unordered_set<std::string>` of USRs already emitted (empty USRs
   — e.g. inclusion directives — are never deduped by USR). Skip a declaration whose USR was already
   captured. This collapses the *N* copies of each header declaration to one.
3. **Include directives.** Capture inclusion directives from in-scope files (not just the main file), so
   header→header includes are recorded. De-duplicate include ASTNodes by
   `(includingFileRelPath, includedFileRelPath)`. Store the included file's **project-relative path**
   (via the existing `RelativePath` helper on `clang_getIncludedFile`), not just its basename, so the
   graph builder can resolve it to a file node unambiguously. Out-of-scope include targets (system /
   dependency headers) keep their bare name and remain external.

**Graph-builder changes (`src/graph-builder.cpp`):**

1. **File nodes for headers.** `ensureFileNode()` already creates a file node for any relative path it
   is given. With header declarations and header→header includes now present, headers naturally become
   `File` nodes. No structural change needed beyond feeding it header paths.
2. **Resolve logical parent by USR, not simple name.** Replace the `logicalNameToId` (simple-name) map
   with a `usrToId` map keyed on `ASTNode.usr`. Resolve a node's `logicalParent` by looking up
   `semanticParentUsr`. Fall back to the current simple-name behaviour only when USRs are empty. This
   fixes `Camera::processEvents` re-attaching to `Camera` instead of the module, and correctly handles
   overloads and same-name-different-namespace collisions.
3. **Resolve include edges by relative path.** Change the include-target lookup from a basename map to a
   `relativePathToFileId` map, matching the included file's project-relative path to the file node.
   Internal includes then produce `.cpp → .h` / `.h → .h` edges; unresolved (external) targets keep the
   bare-string target as today.

**Metrics.** No new metric code required, but note the downstream wins: `includeDepth` /
`transitiveIncludeCount` / `compileImpact` become meaningful once the internal include graph connects,
and `methodCount` / `inheritanceDepth` improve once methods re-attach to their class.

**Node identity note.** Header file node ids follow the existing scheme
(`Project::module::header.hpp`). Because a header's id is derived from its own path (not the including
TU), the same header included by many TUs maps to one stable id.

### Testing additions

- **New fixture:** a header declaring a class plus a `.cpp` defining its methods (do not modify the
  existing `test-input.*`). Add cases verifying:
  - a `Class` node sourced from the **header** is present;
  - a header `File` node exists;
  - a method's `logicalParent` is the class id (not the module id);
  - an internal `IncludeDependency` edge `source.cpp → header.hpp` exists.
- **Scope test:** given a compile command whose source lives outside `projectRoot`, its declarations are
  absent by default and present when `--include-external` is set.
- Keep all existing v0.1 tests green.

### Out of scope for v0.2

- Function-call / symbol-usage edges (still deferred, see below).
- Deduplicating *definitions vs declarations* of the same function across a header/source split beyond
  USR identity (USR de-dup is sufficient; the first-seen wins). See the record-forward-declaration
  refinement below for the one case where this mattered in practice.

### As-built notes (refinements found during implementation)

Three refinements beyond the plan above were needed once the changes were tested against
`vulkan-3d-orbit-viewer-poc`:

1. **Skip record forward declarations.** A `class Foo;` forward declaration and the real definition
   share a USR, so first-seen won the dedup — and a class node could end up pointing at a bodyless
   forward declaration in an unrelated header (observed: `Camera`, forward-declared in `renderer.h`,
   defined in `camera.h`), losing its extent, fields, and methods. Fix: in the parser, skip
   `ClassDecl`/`StructDecl` cursors where `clang_isCursorDefinition` is false, so the definition always
   wins. (Records only — function prototypes are still captured when no in-scope definition exists.)

2. **Scope Include nodes to their including file.** Once includes are captured from headers too, every
   file that `#include`s a common header (`<vector>`, `vulkan/vulkan.h`, …) produced an Include *node*
   whose id was module-scoped by the included name, so they all collided and triggered hundreds of
   disambiguation warnings. Fix: the graph builder sets an Include node's logical parent to its file
   node, so its id is `Project::…::including-file::included-name` — unique per (file, include). Include
   *edges* were already de-duplicated by `(includingFile, includedFile)` and were unaffected.

3. **Vendored dependencies live *inside* the repo — excluded by default.** Project-root scoping
   (Enhancement B) excludes code outside `--project`, but dependencies fetched under `build/_deps/`
   (FetchContent) sit *inside* the repo root and would otherwise remain in scope. Prism therefore also
   skips files whose path passes through a build/dependency directory: the default set is `build`,
   `cmake-build*`, `_deps`, `deps`, `vendor`, `third_party` / `thirdparty` / `third-party`, `external`,
   `extern`, `node_modules`, and `.git` / `.svn` / `.hg`. `--exclude <dir>` extends the set,
   `--no-default-excludes` starts from empty, and `--include-external` bypasses all filtering. With this
   in place, pointing `--project` at the repo root of the test project yields the same clean 360-node
   project-only graph as manually scoping to `<repo>/src` (down from 1835 with deps), with 2 legitimate
   duplicate-id warnings (both real member overloads).

---

## Deferred / Out of Scope for This Implementation

The following are noted in the design document but are explicitly out of scope for this pass:

- Function call edge extraction (`FunctionCall`, `SymbolUsage` edge kinds) — stub these with TODO.
- `publicMethodCount` — requires visibility tracking not yet present in `ASTNode`. Mark TODO.
- Template instantiation extraction — best-effort only, skip if complex.
- Git history integration, incremental analysis, CI exit codes.
- Multi-language support (TypeScript, Python, Rust).
- Coverage data ingestion.

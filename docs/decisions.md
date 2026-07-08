# Prism — Design Decisions

This document records notable decisions made while implementing Prism, and the
places where the implementation deliberately diverges from `prism-spec.md`. Each
entry states the decision, why it was made, and its consequences.

---

## 1. A `prism-core` static library sits between the sources and both executables

**Decision.** The pipeline sources (`parser`, `dependency-graph`,
`graph-builder`, `metrics-engine`, `exporter`, and the logging wrapper) are
compiled once into a static library, `prism-core`. Both `prism` (the CLI) and
`prism-tests` link against it.

**Why.** The spec's tests require constructing real components — e.g.
`parser.test.cpp` builds a `Parser` and calls `Parse()`, `exporter.test.cpp`
calls `Export()`. The original `CMakeLists.txt` linked the test executable
against Cimmerian only, so those symbols (and `libclang`) would never resolve
and the specified tests could not link. Factoring the implementation into a
library is the smallest change that lets the tests exercise the real code.

**Consequence.** `libclang`, `firefly`, and the JSON library are linked
`PUBLIC` on `prism-core`, so both executables inherit them. Deviates from the
spec's "do not restructure `CMakeLists.txt`" guidance, but is required for the
specified tests to build.

---

## 2. `SerialiseNode` / `SerialiseEdge` are file-local, not class members

**Decision.** These helpers live in an anonymous namespace inside
`exporter.cpp` rather than as private members of `Exporter` returning a JSON
value, as the spec's class sketch showed.

**Why.** The spec also states, as a hard constraint, that the JSON library must
not appear in any header except `json-serialization.hpp`. Declaring the helpers
as members returning `Amanuensis::Value` (originally `nlohmann::json`) would pull
the JSON type into `exporter.hpp`. The constraint wins over the illustrative
class shape.

**Consequence.** `exporter.hpp` exposes only `Export()`; all serialisation
detail is private to the translation unit.

---

## 3. Tests use Cimmerian's real API, with shared setup behind cached functions

**Decision.** Tests use `DESCRIBE` / `IT` / `ASSERT_*`, not the
`EXPECT(...).ToEqual(...)` form shown in the spec. Per-suite setup (parsing the
fixture, building a graph, running an export) lives in a namespace-scope
function returning a reference to a `static` local.

**Why.** Cimmerian exposes no `EXPECT().ToEqual()` — the actual assertion
macros are `ASSERT_EQUAL`, `ASSERT_TRUE`, `ASSERT_FALSE`, etc. Additionally,
Cimmerian's `IT(...)` body expands to a **captureless** lambda
(`+[](void*){...}`), so an `IT` block cannot reference locals declared in the
enclosing `DESCRIBE`. Routing shared state through a function with a `static`
local computes it once and makes it reachable from every captureless case.

---

## 4. Base-specifier logical parent comes from the traversal parent

**Decision.** In the parser, a `CXXBaseSpecifier`'s logical parent (the derived
class) is taken from the `parent` cursor that `clang_visitChildren` passes to
the visitor, not from `clang_getCursorSemanticParent`.

**Why.** libclang reports the *translation unit* as a base specifier's semantic
parent. Using it produced inheritance edges rooted at the project/module node
instead of the derived class (caught during end-to-end verification). The
traversal `parent` is the enclosing class, which is what we want.

**Consequence.** Inheritance edges are `derived -> base`, and `inheritanceDepth`
walks correctly.

---

## 5. Project name is derived with `weakly_canonical`

**Decision.** The CLI derives the project name from
`std::filesystem::weakly_canonical(projectRoot).filename()`, with fallbacks to
the parent directory name and finally the literal `"Project"`.

**Why.** `std::filesystem::absolute(".")` yields a path with a trailing
separator, whose `filename()` is empty — which produced node ids like
`::build::...` with an empty project segment. `weakly_canonical` resolves `.`
and trailing separators to the real directory name (e.g. `prism`).

---

## 6. `linesOfCode` is now computed from a captured source extent

**Decision.** `ASTNode` gained an `int lineEnd` field. The parser fills it from
the cursor's source extent (`clang_getRangeEnd(clang_getCursorExtent(...))`),
the graph builder copies it onto `GraphNode.lineEnd`, and the metrics engine
sets `linesOfCode = lineEnd - line + 1` for any node with a real multi-line
span.

**Why.** The original pass left `linesOfCode` as `std::nullopt` because the spec
said `ASTNode` needed no changes and therefore carried no extent end. With that
constraint lifted, adding a single `lineEnd` field is the minimal way to make
the metric real.

**Consequence.** Namespaces, classes, structs, and multi-line functions/methods
now report `lines_of_code`. Nodes without a genuine span — synthesised
project/module/file nodes, and single-line declarations where `lineEnd == line`
— are still omitted rather than reported as a misleading `1`, honouring the
"never use a sentinel for a metric" rule. File-level line counts (whole-file
LOC) remain unimplemented; they would require the metrics engine to open source
files, which it currently has no project-root context to do.

---

## 7. JSON serialisation uses Amanuensis, the first-party library

**Decision.** `nlohmann/json` is removed. Serialisation goes through
`Amanuensis` (`DeanWilsonDev/amanuensis`): `Amanuensis::Value::MakeObject()` /
`MakeArray()` / `Insert` / `PushBack` build the document, and
`Amanuensis::Writer::WriteToFile` writes it. `WriterOptions` defaults (pretty,
two-space indent, trailing newline) reproduce the previous output shape.

**Why.** Amanuensis is the project's own JSON library; standardising on it
removes a third-party dependency.

**Consequences / notes.**

- **CMake target clash.** Firefly vendors Amanuensis via
  `add_subdirectory(external/amanuensis)` and defines the `amanuensis` target
  when Firefly is made available. Declaring a second copy and calling
  `FetchContent_MakeAvailable(amanuensis)` unconditionally would try to define
  the `amanuensis` target twice and fail configuration. The `amanuensis`
  dependency is therefore declared from its canonical URL but its
  `FetchContent_MakeAvailable` is guarded by `if(NOT TARGET amanuensis)`: it
  reuses Firefly's copy when present and fetches from the URL otherwise.
- **`float` construction is explicit.** `Amanuensis::Value` has constructors for
  `bool`, `int`, `long long`, and `double`. A `float` argument is ambiguous
  between them, so optional `float` metrics are cast to `double` before
  construction.
- **`json-serialization.hpp` repurposed.** It is now the single include boundary
  for `<amanuensis.hpp>` and exposes `NodeKindToValue()`. The old
  `NLOHMANN_JSON_SERIALIZE_ENUM` macro (which never compiled against the
  `nodeKindNames` array as written, and was unused because the exporter
  serialises `type` via `ToStringFromNodeKind`) is gone.
- **`pch.hpp`.** The `#include <nlohmann/json.hpp>` line was removed. The JSON
  library is no longer part of the precompiled header, tightening the rule that
  it appears only in `exporter.cpp` and `json-serialization.hpp`.

---

## 8. Building on macOS

The spec targets Linux (its `CMakeLists.txt` defaults `LLVM_DIR` / `Clang_DIR`
to `/usr/lib/cmake/...`). On macOS those defaults are wrong. Configure by
pointing both at a Homebrew LLVM (which ships `libclang`, the `clang-c` headers,
and the CMake package config that Apple's Command Line Tools omit):

```sh
LLVM=$(brew --prefix llvm)
cmake -S . -B build \
  -DLLVM_DIR="$LLVM/lib/cmake/llvm" \
  -DClang_DIR="$LLVM/lib/cmake/clang"
cmake --build build
```

The committed `CMakeLists.txt` keeps the Linux defaults; the macOS paths are
supplied at configure time rather than hard-coded.

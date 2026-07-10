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
sets `linesOfCode = lineEnd - line + 1` for any node with a real span
(`lineEnd >= line`, which always holds — the graph builder falls back to `line`
when the parser has no extent, so the comparison never sees a genuinely-missing
value).

**Why.** The original pass left `linesOfCode` as `std::nullopt` because the spec
said `ASTNode` needed no changes and therefore carried no extent end. With that
constraint lifted, adding a single `lineEnd` field is the minimal way to make
the metric real.

**Consequence.** Namespaces, classes, structs, and functions/methods/fields —
including single-line ones — now report `lines_of_code`. Synthesised
project/module nodes with no line info of their own still get their LOC purely
from aggregation, never a leaf value. File-level line counts (whole-file LOC)
remain unimplemented; they would require the metrics engine to open source
files, which it currently has no project-root context to do.

**Correction (found while investigating a pharos LOC-integrity report).** This
decision originally required `lineEnd > line` (strict), reasoning that a
single-line declaration was "not a genuine span" and should be omitted rather
than "misleadingly" reported as `1`. That reasoning was wrong: a one-line
declaration has a real, correct span of exactly 1 line — it isn't missing data,
it's a valid value that happens to equal 1. The strict comparison silently
dropped `lines_of_code` for every one-line function, method, and field.
Verified against `pharos-proto`: 12 of 96 `Function` nodes (getters, one-line
predicates like `isLeaf()`/`hasChildren()`) and all 218 `Field` nodes — every
member variable in the project, since member declarations are almost always
one line — reported no LOC at all under the old check. Changed to `>=`.

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

---

## 9. v0.2 — header-aware extraction and project-root scoping

Full plan and per-file design in `docs/prism-spec.md` ("Planned Enhancements
(v0.2)"). Motivated by running v0.1 against `vulkan-3d-orbit-viewer-poc`, where
header-declared classes were missing, methods flattened to the module, and the
internal include graph was empty. Key decisions:

- **Project-root scoping (default on).** Only files under `--project` are
  analysed; the visitor's main-file filter became an in-scope-of-project filter,
  and out-of-root translation units are skipped entirely. `--include-external`
  restores whole-database analysis. This is what lets project *headers*
  contribute declarations while excluding system/dependency headers.
- **USR-based identity.** `ASTNode` gained `usr` / `semanticParentUsr`. The
  parser de-duplicates declarations across translation units by USR (a header
  declaration is otherwise visited once per including TU), and the graph builder
  resolves logical parents by the parent's USR (falling back to simple-name).
  This re-attaches out-of-line methods (`Camera::update`) to the class declared
  in the header instead of the module.
- **Include edges resolve by project-relative path**, not basename, so
  `.cpp → .h` and `.h → .h` connect unambiguously; external headers keep a bare
  target string.

Refinements found during implementation (also recorded in the spec's as-built
notes):

- **Skip record forward declarations.** `class Foo;` and the definition share a
  USR; first-seen won, so a class node could point at a bodyless forward
  declaration (observed: `Camera` in `renderer.h`). The parser now skips
  `ClassDecl`/`StructDecl` cursors that are not definitions, so the definition
  wins and carries the real extent, fields, and methods.
- **Include nodes are file-scoped.** Their ids are built from the including file
  node (`…::renderer.h::vector`), not the module, so the same header included by
  many files no longer collides.
- **Vendored deps sit inside the repo — excluded by default.** FetchContent
  places dependencies under `build/_deps/`, which is *inside* the project root,
  so root-scoping alone does not exclude them. Prism therefore also skips files
  whose path passes through a build/dependency directory. The default set is
  `build`, `cmake-build*`, `_deps`, `deps`, `vendor`, `third_party` /
  `thirdparty` / `third-party`, `external`, `extern`, `node_modules`, and
  `.git` / `.svn` / `.hg`. `--exclude <dir>` extends it, `--no-default-excludes`
  starts from empty, and `--include-external` bypasses all filtering. The
  matching is on lowercased path segments (exact, plus a `cmake-build*` prefix).
  With this, pointing `--project` at the repo root gives the same clean
  360-node project-only graph as manually scoping to `<repo>/src` (was 1835 with
  deps), and duplicate-id warnings dropped from 421 to 2 (both genuine
  overloads). Exact-segment matching keeps false positives unlikely (a dir named
  `build_system` is not matched); pass `--no-default-excludes` if a project
  legitimately keeps sources under one of these names.

**Result on `vulkan-3d-orbit-viewer-poc/src`:** 15 classes/structs captured
(were ~1), methods re-attached (e.g. `VulkanContext` = 23), 32 internal include
edges (were 0), and `lines_of_code` populated for every class.

---

## 10. Lines of code: real bodies and container aggregation

Motivated by testing against `snake`, where module nodes reported no LOC,
namespaces showed a single lexical block, and most functions reported nothing —
because a header *declaration* (a one-line signature) won the USR dedup over the
`.cpp` *definition* that has the body.

- **Prefer definitions over declarations.** The parser now keeps one node per
  USR but lets the definition win: when a definition arrives after a declaration
  was captured, it upgrades the stored node's source span (line/lineEnd) and
  file. So a method declared in a header and defined in a `.cpp` reports the
  body's line count and is located at the definition. This generalises — and
  replaces — the earlier "skip record forward declarations" special case.
- **Exact file line counts.** The parser reads each in-scope file once and
  records its physical line count (`ParseResult::fileLineCounts`); the graph
  builder stamps that onto `File` nodes as `linesOfCode`. File LOC matches
  `wc -l` exactly.
- **Container aggregation** (metrics engine, `AggregateLinesOfCode`): modules and
  the project sum their **physical** children (sub-modules + files); namespaces
  sum their **logical** children (nested namespaces + classes/structs/free
  functions). Methods are logical children of their class, not the namespace, so
  they are not double counted. Leaf declarations keep their own source-extent
  LOC. Aggregation is memoised and cycle-guarded, and leaves a container as
  `std::nullopt` when nothing underneath it had a line count (never a `0`
  sentinel).

**Result on `snake`:** file LOC is exact (`entity.cpp` = 99, `size-2d.hpp` = 38),
module `engine` = 2115, namespace `Engine` = 461 (was 30), and the project totals
6313 lines — vs 6626 for a raw `wc -l` of `src`, the difference being header
files that contribute no captured cursors (e.g. pure forward-declaration or
macro-only headers) and so never become file nodes.

**Limitation, since resolved: see §11.** A namespace re-opened across several
files/modules merges to a single node by USR, but that node's `physicalParent`
(and, for top-level namespaces, `logicalParent`) was pinned to wherever the
parser happened to visit it *first* — order depending on `compile_commands.json`
iteration, not the namespace's actual footprint. Its aggregate LOC was always
correct (it does correctly sum every member across every file that reopens it),
but the node's place in the tree was arbitrary, so a namespace could appear
nested under a directory that physically holds only a fraction of its content.
Physical module/project aggregation was unaffected — see §11 for the fix.

---

## 11. Constructors, destructors, and conversion operators were invisible to the graph

**Decision.** `MapCursorKind` (parser) now also maps `CXCursor_Constructor`,
`CXCursor_Destructor`, and `CXCursor_ConversionFunction` to `NodeKind::Function`.

**Why.** Found while investigating a pharos LOC-integrity report. libclang gives
constructors, destructors, and conversion operators their own cursor kinds,
distinct from `CXCursor_CXXMethod`. The switch in `MapCursorKind` (and the
matching cursor-kind list in `prism-spec.md`) only ever covered
`CXCursor_FunctionDecl` and `CXCursor_CXXMethod`, so every constructor,
destructor, and conversion operator in any analysed project silently vanished —
not reported with 0 LOC, simply never turned into an `ASTNode` at all. Verified
with a standalone fixture (`Widget()` / `~Widget()` / `operator int()`): all
three were absent from `analysis.json` before the fix and present with correct
`lines_of_code` after.

**Consequence.** Classes whose only substantial logic lives in a constructor or
destructor now contribute that code to method counts and LOC totals. This was a
gap since the original v0.2 spec (the cursor-kind list it specifies was already
incomplete), not a regression.

---

## 12. Namespaces are re-anchored to where their content actually lives

**Decision.** After the graph is fully built, a new pass (`ReanchorNamespaces` in
`graph-builder.cpp`) walks every `Namespace` node and recomputes its
`physicalParent` (and `logicalParent`, when it currently points at a
module/project rather than an enclosing namespace) as the closest common
`::`-ancestor of every file its logical children — recursively through nested
namespaces — actually live in.

**Why.** Found via the same pharos LOC-integrity report: the physical tree and
the logical tree appeared to disagree. Root cause was the limitation described
in §10 — `namespace pharos`, reopened across `ui/`, `data/`, `layout/`,
`icons/`, and `metrics/`, was anchored under the `icons` module (369 physical
lines) simply because that was the first file the parser visited, while its
logical LOC (2271, gathered correctly from every file) made it look wildly
inconsistent with its physical home. Re-anchoring it to the common ancestor of
its actual content places it at `pharos-proto::src` — the true home — while a
namespace whose content genuinely lives in one file (`pharos::icons`) still
collapses precisely to that file, not a directory.

**Consequence.** A namespace's tree position no longer depends on translation
unit visitation order, and is consistent whichever tree (physical or logical)
it's viewed from. Physical file/module/project LOC totals are unaffected: this
pass only changes where a namespace node is nested, not the summation math
(namespaces were never part of the physical aggregation walk, only files and
modules are).

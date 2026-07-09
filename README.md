# Prism

A standalone, headless C++ analysis engine. Prism accepts a C++ repository as
input, analyses its structure with libclang, and produces a structured
`analysis.json` file describing the codebase — its modules, files, namespaces,
classes, functions, and the dependency edges between them, each annotated with
metrics.

Prism is a pure data pipeline — no UI, no rendering, no window management. It
runs from a terminal, a CI job, or a build script. The primary consumer of its
output is [Pharos](../pharos), the architecture visualisation UI, but the output
schema is tool-agnostic.

______________________________________________________________________

## How to use

### 1. Prerequisites

- A C++20 toolchain (GCC 13+, Clang 16+, or MSVC 19.29+ — `std::format` is required).
- CMake 3.20+.
- **libclang** and its development headers / CMake package config. Prism parses
  code through libclang, so this must be present at build time (see
  [Building](#building)).

### 2. Generate a compilation database for the project you want to analyse

Prism needs a `compile_commands.json` describing how each translation unit is
compiled. Most build systems can emit one:

```bash
# CMake projects
cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
# → build/compile_commands.json

# Make / other build systems
#   use Bear:  bear -- make
```

### 3. Run Prism

```bash
prism --project /path/to/repo \
      --compile-commands /path/to/repo/build/compile_commands.json \
      --output analysis.json \
      --verbose
```

If `--compile-commands` is omitted, Prism looks for
`<project>/compile_commands.json`. If `--output` is omitted, it writes
`./analysis.json`.

### Options

| Flag | Required | Default | Description |
| --- | --- | --- | --- |
| `--project <path>` | yes | — | Path to the root of the repository to analyse. Node ids are named after this directory. |
| `--output <path>` | no | `./analysis.json` | Where to write the analysis. |
| `--compile-commands <path>` | no | `<project>/compile_commands.json` | Path to the compilation database (file or its directory). |
| `--verbose` | no | off | Print per-stage progress and diagnostics. |
| `--include-external` | no | off | Analyse every translation unit in the database, including files outside `--project` (dependencies, system headers). Overrides all scoping and exclusions. |
| `--exclude <dir>` | no | — | Skip any file whose path passes through a directory with this name. Repeatable; added on top of the built-in defaults. |
| `--no-default-excludes` | no | off | Do not apply the built-in excluded-directory list (only your `--exclude` entries, if any). |
| `--help`, `-h` | no | — | Print usage and exit. |

> **Scoping and dependency exclusion.** By default Prism analyses only files
> under `--project`, and additionally skips files inside common build/dependency
> directories even when they live in-tree. This means you can point `--project`
> at the repository root and still get a clean, project-only graph — vendored
> dependencies (e.g. CMake FetchContent under `build/_deps/`) are excluded
> automatically. The built-in skip list covers `build`, `cmake-build*`, `_deps`,
> `deps`, `vendor`, `third_party` / `thirdparty` / `third-party`, `external`,
> `extern`, `node_modules`, and `.git` / `.svn` / `.hg`. Use `--exclude <dir>`
> to add more, `--no-default-excludes` to start from an empty list, or
> `--include-external` to analyse everything.

Prism exits `0` on success and `1` on a pipeline failure (bad arguments, missing
project directory, unreadable compilation database, or an export failure).
Individual translation units that fail to parse are logged as warnings and
**skipped** — they do not abort the run.

### Worked example

Analysing this repository's own example fixture:

```bash
# after building (see below)
./build/prism --project src/example \
              --compile-commands /path/to/compile_commands.json \
              --output analysis.json
```

produces:

```json
{
  "metadata": {
    "project": "example",
    "generated_at": "2026-07-08T06:26:26Z",
    "prism_version": "0.1.0",
    "file_count": 1,
    "node_count": 11,
    "edge_count": 3
  },
  "nodes": [
    {
      "id": "example::Tester::TestInput",
      "name": "TestInput",
      "type": "Class",
      "physical_parent": "example::test-input.cpp",
      "logical_parent": "example::Tester",
      "file": "test-input.cpp",
      "line": 12,
      "column": 7,
      "lines_of_code": 5,
      "method_count": 1,
      "inheritance_depth": 1,
      "dependency_count": 1,
      "dependent_count": 0,
      "coupling_score": 0.0909090936,
      "circular_dependency": false,
      "instability": 1.0
    }
  ],
  "edges": [
    { "source": "example::Tester::TestInput", "target": "example::Tester::TestInputBase", "type": "inheritance", "weight": 1 }
  ]
}
```

______________________________________________________________________

## Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The committed `CMakeLists.txt` defaults `LLVM_DIR` / `Clang_DIR` to the usual
Linux locations (`/usr/lib/cmake/...`). On other platforms, point them at your
LLVM install at configure time.

**macOS** (Apple's Command Line Tools ship `libclang.dylib` but omit the headers
and CMake config, so install LLVM via Homebrew):

```bash
brew install llvm
LLVM=$(brew --prefix llvm)
cmake -B build \
  -DLLVM_DIR="$LLVM/lib/cmake/llvm" \
  -DClang_DIR="$LLVM/lib/cmake/clang"
cmake --build build
```

Dependencies (Firefly, Cimmerian, Amanuensis) are fetched automatically via
CMake `FetchContent`.

### Running the tests

```bash
cmake --build build --target prism-tests
./build/prism-tests
```

______________________________________________________________________

## Pipeline

Prism processes a repository in four sequential stages, each with a single
responsibility, communicating with the next via a well-defined data structure:

```
Repository (source files + compile_commands.json)
        │
        ▼
┌─────────────────────┐
│  Stage 1 — Parser   │  AST traversal via libclang
└─────────────────────┘
        │  ASTNode collection
        ▼
┌──────────────────────────────┐
│  Stage 2 — Graph Builder     │  Nodes, edges, weights, hierarchy
└──────────────────────────────┘
        │  DependencyGraph
        ▼
┌──────────────────────────────┐
│  Stage 3 — Metrics Engine    │  Complexity, coupling, compile impact
└──────────────────────────────┘
        │  Annotated DependencyGraph
        ▼
┌─────────────────────────────┐
│  Stage 4 — Exporter         │  Serialises to analysis.json
└─────────────────────────────┘
        │
        ▼
  analysis.json
```

### Stage 1 — Parser

Uses `libclang` to traverse the AST of every translation unit in the
compilation database and extract raw structural facts from main-file cursors:
namespaces, class/struct declarations, functions and methods, fields, include
directives, and inheritance (base) specifiers. Outputs a flat `ASTNode`
collection — no graph structure yet.

### Stage 2 — Graph Builder

Constructs the graph data model from the raw `ASTNode` collection. It builds the
**physical hierarchy** (project → module → file, derived from directory layout)
and the **logical hierarchy** (project → namespace → class → function/field,
from semantic nesting), assigns each node a stable `::`-separated id, and emits
typed, weighted **edges** (`include_dependency`, `inheritance`, `composition`).

### Stage 3 — Metrics Engine

Walks the constructed graph and annotates every node with computed metrics, with
full access to graph topology:

- **Structural** — dependency / dependent counts, instability, coupling score,
  and circular-dependency participation (via Tarjan SCC).
- **Code** — lines of code (from the declaration's source extent), method count,
  and inheritance depth.
- **C++-specific** — include depth, transitive include count, and compile impact
  for file nodes.

Metrics that cannot be derived are omitted rather than reported as a sentinel.

### Stage 4 — Exporter

Serialises the fully annotated graph to `analysis.json` using
[Amanuensis](https://github.com/DeanWilsonDev/amanuensis), the project's
first-party JSON library. Owns the output schema; omits any optional field that
was not computed.

______________________________________________________________________

## Output schema

```json
{
  "metadata": {
    "project": "example",
    "generated_at": "2026-07-08T06:26:26Z",
    "prism_version": "0.1.0",
    "file_count": 1,
    "node_count": 11,
    "edge_count": 3
  },
  "nodes": [ ... ],
  "edges": [ ... ]
}
```

**Node types:** `Project` `Module` `File` `Namespace` `Class` `Struct`
`ParentClass` `Include` `Function` `Field`

**Edge types:** `include_dependency` `inheritance` `composition`
(`symbol_usage` and `function_call` are reserved but not yet emitted).

Every node carries `id`, `name`, `type`, `physical_parent`, `logical_parent`,
`file`, `line`, and `column`. Metric fields (`lines_of_code`, `method_count`,
`dependency_count`, `coupling_score`, `instability`, `circular_dependency`,
`include_depth`, …) appear only when they were computed for that node.

______________________________________________________________________

## Tech stack

| Component | Technology |
| --- | --- |
| Language | C++20 |
| Build system | CMake (3.20+) |
| C++ parser | libclang (stable C API) |
| Compilation database | compile_commands.json |
| JSON serialisation | [Amanuensis](https://github.com/DeanWilsonDev/amanuensis) |
| Testing | [Cimmerian](https://github.com/DeanWilsonDev/cimmerian) |
| Logging | [Firefly](https://github.com/DeanWilsonDev/firefly) |

**Target platforms:** Linux (primary), macOS, Windows (secondary).

See [`docs/decisions.md`](docs/decisions.md) for design decisions and notes, and
[`docs/prism-spec.md`](docs/prism-spec.md) for the full implementation spec.

______________________________________________________________________

## Relationship to Pharos

Prism is a dependency of Pharos, not a component of it. The boundary is
`analysis.json`.

```
prism  ──produces──▶  analysis.json  ──consumed by──▶  pharos
```

Pharos does not call into Prism at runtime — it reads the output file. Prism can
be run separately and its output cached, and Pharos can be developed against a
static `analysis.json` without running the analysis pipeline.

______________________________________________________________________

## Non-goals

- Does not render anything
- Does not manage a UI or window
- Does not perform refactoring or modify source files
- Does not support languages other than C++

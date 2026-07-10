# `analysis.json` changes — metric correctness pass

Everything here is a change to what Prism writes into `analysis.json`. Consumers
(primarily Pharos) need to react to the items marked **breaking**; the rest are
values getting more accurate, not the schema moving.

The before/after figures come from running Prism over its own source tree.

______________________________________________________________________

## 1. Structural metrics are now omitted on most node kinds — **breaking**

`dependency_count`, `dependent_count`, `coupling_score` and `circular_dependency`
used to be written on **every** node. They are now written only on `File`,
`Class` and `Struct`.

They disappear entirely from: `Project`, `Module`, `Namespace`, `Function`,
`Field`, `Include`, `ParentClass`.

**Why.** Prism does not yet extract `function_call` or `symbol_usage` edges, so a
function's `dependency_count: 0` was never a measurement — it was an artefact of
what Prism does not look at. Pharos would render every function as
zero-coupling and non-circular, which reads as a finding. The spec already says
metrics that cannot be computed must be `null`/absent rather than `0`, and the
engine was already applying that rule to `instability` (which drops out when
`dependency_count + dependent_count == 0`) while emitting three zeros beside it.

**Pharos action.** Treat these four fields as absent-by-default and only render
coupling/cycle affordances for file, class and struct nodes. Do not fall back to
`0`/`false` — absent means "not measured", and a file with genuinely no includes
still reports `dependency_count: 0`, which *is* a measurement.

When `function_call`/`symbol_usage` edge extraction lands, these fields will
reappear on `Function` nodes. The gate is `ParticipatesInEdgeModel()` in
`src/metrics-engine.cpp`.

## 2. `dependency_count` no longer counts system headers — **breaking-ish**

Only edges whose **source and target are both nodes in the graph** now count. An
`#include <vector>` produces an edge whose target is the bare string `"vector"`,
which is not a node.

`dependent_count`, `include_depth`, `transitive_include_count` and
`compile_impact` already ignored those edges, so `instability` was a ratio of two
different populations. File-level numbers move accordingly:

| file | `dependency_count` | `instability` |
| --- | --- | --- |
| `ast-node.hpp` | 3 → 0 | 0.43 → 0.00 |
| `dependency-graph.hpp` | 5 → 1 | 0.63 → 0.20 |
| `exporter.hpp` | 3 → 1 | 0.75 → 0.25 |
| `graph-builder.hpp` | 5 → 2 | 0.71 → 0.33 |

The `edges` array still contains the external include edges — they are only
excluded from metric arithmetic. If Pharos draws them, nothing changes there.

`ast-node.hpp` going to `instability: 0.0` is the intended reading: it is a leaf
header that four things depend on and that depends on nothing in the project.

## 3. `cyclomatic_complexity` is now populated — **additive**

New on `Function` nodes, and only on nodes whose **definition** was seen (a bare
declaration in a header has no body to measure). Absent otherwise.

McCabe: `1 + decision points`, where a decision point is `if`, `for`, range-`for`,
`while`, `do`, each `case` label, `catch`, `?:`, and each `&&` / `||`. `switch`
itself is not one, nor is `default`, nor `else`.

Verified against a fixture: a plain function is `1`, a three-`case` switch is `4`,
`if (a > 0 && b > 0 || a == b)` is `4`, a ternary is `2`, two `catch` blocks are
`3`.

## 4. Anonymous namespace nodes — **breaking**

Clang gives every top-level anonymous namespace the same USR (`c:@aN`), so the
cross-translation-unit dedup folded the anonymous namespaces of *every* `.cpp`
into a single node. Its `file` named one arbitrary source and its
`lines_of_code` summed helpers from all of them. On Prism's own tree that was one
node holding 296 lines drawn from six unrelated test files.

There is now one node per file.

| | before | after |
| --- | --- | --- |
| node count | 1 | 10 |
| `id` | `prism::test::anonymous` | `prism::src::parser.cpp::anonymous` |
| `name` | `""` (empty) | `"anonymous"` |

**Pharos action.** The `id` is now scoped to the **file** node, not the module
node — a child of `<module>::<file>.cpp`, not of `<module>`. Any code assuming a
namespace id is `<parent-id>::<name>` needs to accept the file-scoped form. The
`name` field is no longer empty, so a display fallback for `""` can go.

## 5. Composition and inheritance edges are resolved by type identity — **fixes**

Field types were matched by searching the type spelling for any known class name
as a substring. Consequences, all now fixed:

- `struct Edge { EdgeKind kind; }` produced a composition edge **`Edge → Edge`**,
  because `"EdgeKind"` contains `"Edge"`. That self-loop then made
  `circular_dependency: true` on `Edge` — a cycle that does not exist.
- `std::vector<GraphNode>` matched nothing useful, so `DependencyGraph` had **no**
  composition edges at all despite being built from `GraphNode` and `Edge`.
- When several class names matched (`Exporter` and `ExporterConfig` both occur in
  `"Prism::ExporterConfig"`), the winner came from `unordered_map` iteration
  order.

Edges are now resolved through clang's type declarations, peeling pointers,
references, arrays and cv-qualifiers, and walking template arguments — so
`std::vector<GraphNode>` composes `GraphNode`, `Point corners[4]` composes
`Point`, and `Box<Point>` composes `Point`. A class is never composed with
itself. Composition edge `weight` counts how many of a class's fields reach the
target type.

Effect on Prism's own graph: `composition` edges 3 → 8 (the three before included
the bogus `Edge → Edge`), and `Edge.circular_dependency` is now `false`.

## 6. `module_boundary_violations`, `test_coverage`, `public_method_count`

Still always absent. They were declared in the schema and serialised, but nothing
ever computed them and only `public_method_count` carried a TODO. They now carry
explicit TODOs in `src/metrics-engine.cpp`.

`module_boundary_violations` has **no definition anywhere in the spec** — the
field is named but what counts as a violation is never stated. It stays absent
rather than guessing a rule that Pharos would then present as fact. Worth
deciding on before it is implemented.

## 7. Node and edge counts jump — not a schema change

`node_count` 196 → 347, `edge_count` 62 → 131, `file_count` 14 → 24 on Prism's own
tree, because the parser now actually parses. Previously every `src/*.cpp`
translation unit failed outright and the rest error-recovered, so unresolved
types degraded to `int` — `Edge::sourceId`, a `std::string`, was reported with
`referenced_name: "int"`. Two causes, both fixed in `src/parser.cpp`:

- `-include-pch` and friends dropped the *next* token, but CMake emits PCH flags
  as `-Xclang -include-pch -Xclang <path>`, so the token eaten was the second
  `-Xclang`, leaving dangling `-Xclang` flags with no operand.
- libclang skips the clang driver, so it never discovers the compiler's own
  resource directory (`stdarg.h`) or the macOS SDK. Prism now passes
  `-resource-dir` (baked in at build time from the libclang it linked) and, on
  macOS, `-isysroot` from `SDKROOT` or `xcrun`. Both defer to values already
  present in the compile command.

Expect *any* project analysed before this change to have produced degraded
output. Re-run Prism rather than trusting a cached `analysis.json`.

______________________________________________________________________

## Unchanged

`lines_of_code`, `method_count`, `inheritance_depth`, `include_depth`,
`transitive_include_count` and `compile_impact` keep their meaning and their
values — they were verified correct against a hand-computed fixture before and
after. `compile_impact` remains a raw count typed as a float.

`coupling_score` keeps the spec's formula, `dependency_count / total node count`.
Note the denominator counts *every* node, including `Field` and `Include` nodes a
file could never depend on (121 of 196 nodes on Prism's own tree), so the value
is not comparable across projects. It is now emitted on fewer nodes but is
otherwise the same number. **This one is a spec question, not a bug** — see
`docs/prism-spec.md`.

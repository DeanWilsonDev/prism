#include "prism/parser.hpp"

#include <clang-c/CXCompilationDatabase.h>
#include <clang-c/CXString.h>
#include <clang-c/Index.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "firefly/log.hpp"

namespace Prism {

namespace {

std::string ToString(CXString value)
{
  const char* cstr = clang_getCString(value);
  std::string result = (cstr != nullptr) ? cstr : "";
  clang_disposeString(value);
  return result;
}

std::optional<NodeKind> MapCursorKind(CXCursorKind kind)
{
  switch (kind) {
    case CXCursor_Namespace:
      return NodeKind::Namespace;
    case CXCursor_ClassDecl:
      return NodeKind::Class;
    case CXCursor_StructDecl:
      return NodeKind::Struct;
    case CXCursor_FunctionDecl:
    case CXCursor_CXXMethod:
    case CXCursor_Constructor:
    case CXCursor_Destructor:
    case CXCursor_ConversionFunction:
      return NodeKind::Function;
    case CXCursor_FieldDecl:
      return NodeKind::Field;
    case CXCursor_InclusionDirective:
      return NodeKind::Include;
    case CXCursor_CXXBaseSpecifier:
      return NodeKind::ParentClass;
    default:
      return std::nullopt;
  }
}

/// Path of `target` relative to `root`, POSIX separators. Falls back to the
/// bare filename when the target lives outside the project root.
std::filesystem::path Canonicalise(const std::filesystem::path& path)
{
  std::error_code ec;
  std::filesystem::path result = std::filesystem::weakly_canonical(path, ec);
  if (ec) {
    result = std::filesystem::absolute(path, ec);
  }
  return result;
}

std::string RelativePath(const std::string& target, const std::filesystem::path& root)
{
  if (target.empty()) {
    return "";
  }
  std::filesystem::path relative = Canonicalise(target).lexically_relative(Canonicalise(root));
  if (relative.empty() || *relative.begin() == "..") {
    return std::filesystem::path(target).filename().generic_string();
  }
  return relative.generic_string();
}

std::string ToLower(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return value;
}

/// Directory names skipped by default: common build outputs and vendored /
/// fetched dependency locations that live inside a project tree.
const std::unordered_set<std::string>& DefaultExcludedDirectories()
{
  static const std::unordered_set<std::string> defaults = {
      "build",
      "_deps",
      "deps",
      "vendor",
      "third_party",
      "thirdparty",
      "third-party",
      "external",
      "extern",
      "node_modules",
      ".git",
      ".svn",
      ".hg",
  };
  return defaults;
}

/// True when the project-relative path passes through an excluded directory
/// segment (exact match, case-insensitive) or a `cmake-build*` directory.
bool IsExcludedRelative(
    const std::filesystem::path& relative, const std::unordered_set<std::string>& excluded
)
{
  for (const std::filesystem::path& segment : relative) {
    const std::string name = ToLower(segment.string());
    if (excluded.count(name) != 0 || name.starts_with("cmake-build")) {
      return true;
    }
  }
  return false;
}

/// Whether a file should contribute to the analysis: inside the project root and
/// not within an excluded (build / dependency) directory.
bool ShouldAnalyse(
    const std::string& absolutePath, const std::filesystem::path& projectRoot,
    const std::unordered_set<std::string>& excluded
)
{
  if (absolutePath.empty()) {
    return false;
  }
  std::filesystem::path relative =
      Canonicalise(absolutePath).lexically_relative(Canonicalise(projectRoot));
  if (relative.empty() || *relative.begin() == "..") {
    return false;
  }
  return !IsExcludedRelative(relative, excluded);
}

#if defined(__APPLE__)
/// The clang *driver* discovers the macOS SDK (via SDKROOT or `xcrun`) and
/// passes -isysroot down to the frontend; libclang skips the driver, so a
/// compile command produced by Apple clang — which never needs an explicit
/// -isysroot — leaves libclang unable to find the C++ standard library. Clang
/// then error-recovers rather than bailing out, and every unresolved type
/// silently degrades to `int`, so a std::string field looks like an int and the
/// metrics computed over it are quietly wrong. Recover the SDK path once.
const std::string& MacOsSdkPath()
{
  static const std::string path = []() -> std::string {
    if (const char* fromEnvironment = std::getenv("SDKROOT");
        fromEnvironment != nullptr && *fromEnvironment != '\0') {
      return fromEnvironment;
    }
    std::string discovered;
    FILE* pipe = popen("xcrun --show-sdk-path 2>/dev/null", "r");
    if (pipe == nullptr) {
      return discovered;
    }
    std::array<char, 512> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
      discovered += buffer.data();
    }
    pclose(pipe);
    while (!discovered.empty() && (discovered.back() == '\n' || discovered.back() == '\r')) {
      discovered.pop_back();
    }
    return discovered;
  }();
  return path;
}
#endif

/// USRs of every declaration reachable from a type, once pointers, references,
/// arrays and cv-qualifiers are peeled away, plus the same walk over each
/// template argument. `std::vector<GraphNode>` therefore yields the USRs of both
/// std::vector and GraphNode, and the graph builder keeps whichever of them is a
/// node in the project. This replaces matching class names against type
/// spellings, which reported `EdgeKind` as a reference to `Edge`.
void CollectTypeUsrs(CXType type, std::vector<std::string>& usrs, int depth = 0)
{
  constexpr int maxDepth = 8;
  if (depth > maxDepth || type.kind == CXType_Invalid) {
    return;
  }

  while (true) {
    const CXType pointee = clang_getPointeeType(type);
    if (pointee.kind != CXType_Invalid) {
      type = pointee;
      continue;
    }
    const CXType element = clang_getArrayElementType(type);
    if (element.kind != CXType_Invalid) {
      type = element;
      continue;
    }
    break;
  }
  type = clang_getCanonicalType(clang_getUnqualifiedType(type));

  const CXCursor declaration = clang_getTypeDeclaration(type);
  if (!clang_Cursor_isNull(declaration)) {
    std::string usr = ToString(clang_getCursorUSR(declaration));
    if (!usr.empty() && std::find(usrs.begin(), usrs.end(), usr) == usrs.end()) {
      usrs.push_back(std::move(usr));
    }
  }

  const int templateArguments = clang_Type_getNumTemplateArguments(type);
  for (int index = 0; index < templateArguments; ++index) {
    CollectTypeUsrs(clang_Type_getTemplateArgumentAsType(type, index), usrs, depth + 1);
  }
}

/// Clang gives every top-level anonymous namespace the same USR (`c:@aN`), so
/// the cross-translation-unit dedup below would fold the unrelated anonymous
/// namespaces of every .cpp into one node whose lines of code span files it has
/// nothing to do with. An anonymous namespace is file-local by definition, so
/// qualify its USR with the file to restore one node per file.
std::string CursorUsr(CXCursor cursor, const std::string& file)
{
  std::string usr = ToString(clang_getCursorUSR(cursor));
  if (usr.empty() || clang_getCursorKind(cursor) != CXCursor_Namespace) {
    return usr;
  }
  if (ToString(clang_getCursorSpelling(cursor)).empty()) {
    usr += "@anonymous@" + file;
  }
  return usr;
}

/// McCabe cyclomatic complexity: one, plus one for every point the control flow
/// can branch. `switch` itself is not a branch — each of its `case` labels is —
/// and `else` is not a branch, it is the fall-through of its `if`.
int CyclomaticComplexity(CXCursor function)
{
  int decisionPoints = 0;
  clang_visitChildren(
      function,
      [](CXCursor cursor, CXCursor, CXClientData data) {
        auto* count = static_cast<int*>(data);
        switch (clang_getCursorKind(cursor)) {
          case CXCursor_IfStmt:
          case CXCursor_ForStmt:
          case CXCursor_CXXForRangeStmt:
          case CXCursor_WhileStmt:
          case CXCursor_DoStmt:
          case CXCursor_CaseStmt:
          case CXCursor_CXXCatchStmt:
          case CXCursor_ConditionalOperator:
            *count += 1;
            break;
          case CXCursor_BinaryOperator:
          case CXCursor_CompoundAssignOperator: {
            const CX_BinaryOperatorKind opcode = clang_Cursor_getBinaryOpcode(cursor);
            if (opcode == CX_BO_LAnd || opcode == CX_BO_LOr) {
              *count += 1;
            }
            break;
          }
          default:
            break;
        }
        return CXChildVisit_Recurse;
      },
      &decisionPoints
  );
  return 1 + decisionPoints;
}

struct VisitContext {
  ParseResult* result;
  std::filesystem::path projectRoot;
  int* idCounter;
  bool includeExternal;
  bool verbose;
  std::unordered_set<std::string>* seen;                     // dedup keys for USR-less nodes
  const std::unordered_set<std::string>* excluded;           // excluded directory names
  std::unordered_map<std::string, std::size_t>* usrToIndex;  // USR -> index in result->nodes
  std::unordered_set<std::string>* definedUsrs;              // USRs captured from a definition
  // USRs of out-of-scope namespace/class/struct declarations captured purely to
  // complete an in-scope member's logical parent chain -- see EnsureContainerNode.
  std::unordered_set<std::string>* externalContainers;
};

/// Physical line count of a text file (number of lines, counting a final line
/// without a trailing newline). 0 when the file cannot be read.
int CountFileLines(const std::string& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return 0;
  }
  int lines = 0;
  bool sawContentOnLine = false;
  char character = 0;
  while (input.get(character)) {
    if (character == '\n') {
      ++lines;
      sawContentOnLine = false;
    }
    else {
      sawContentOnLine = true;
    }
  }
  if (sawContentOnLine) {
    ++lines;  // trailing line with no newline
  }
  return lines;
}

/// Ensures `containerCursor` (a Namespace/Class/Struct) has a corresponding
/// ASTNode, synthesizing one -- and, recursively, its own out-of-scope
/// ancestors -- when it doesn't. Called only with a specific in-scope
/// declaration's *actual* semantic-parent chain (never a lexical walk), so it
/// stays bounded to that lineage: resolving Logger::LogImpl's parent touches
/// Logger and Firefly, never wanders into an unrelated header's namespace
/// tree the way relaxing the scope filter itself would (that was tried and
/// pulled in libc++'s entire internal namespace tree -- see git history).
/// A no-op once the container is already present, whether captured normally
/// (in scope) or synthesized here for a different member earlier.
void EnsureContainerNode(CXCursor containerCursor, VisitContext* context)
{
  if (clang_Cursor_isNull(containerCursor)) {
    return;
  }
  const CXCursorKind kind = clang_getCursorKind(containerCursor);
  if (kind != CXCursor_Namespace && kind != CXCursor_ClassDecl && kind != CXCursor_StructDecl) {
    return;
  }

  CXSourceLocation location = clang_getCursorLocation(containerCursor);
  CXFile file;
  unsigned line = 0;
  unsigned column = 0;
  unsigned offset = 0;
  clang_getExpansionLocation(location, &file, &line, &column, &offset);
  const std::string cursorFile = ToString(clang_getFileName(file));
  if (cursorFile.empty()) {
    return;
  }

  const std::string usr = CursorUsr(containerCursor, cursorFile);
  if (usr.empty() || context->usrToIndex->count(usr) != 0) {
    return;  // already captured (in scope or synthesized for an earlier member), or unnameable
  }

  // Ancestors must precede descendants in result->nodes: GraphBuilder resolves
  // logicalParent in a single forward pass over the flat node list.
  CXCursor parentCursor = clang_getCursorSemanticParent(containerCursor);
  const bool hasParent =
      !clang_Cursor_isNull(parentCursor) && clang_getCursorKind(parentCursor) != CXCursor_TranslationUnit;
  if (hasParent) {
    EnsureContainerNode(parentCursor, context);
  }

  ASTNode node;
  node.id = (*context->idCounter)++;
  node.kind = (kind == CXCursor_Namespace) ? NodeKind::Namespace
              : (kind == CXCursor_ClassDecl) ? NodeKind::Class
                                              : NodeKind::Struct;
  node.name = ToString(clang_getCursorSpelling(containerCursor));
  node.usr = usr;
  // No project-relative path: this declaration isn't actually under --project,
  // so it attaches directly to the project root physically rather than having
  // GraphBuilder fabricate a fake file/module chain for it.
  node.file = "";
  node.line = static_cast<int>(line);
  node.column = static_cast<int>(column);
  node.lineEnd = node.line;
  if (hasParent) {
    node.logicalParent = ToString(clang_getCursorSpelling(parentCursor));
    node.semanticParentUsr = CursorUsr(parentCursor, cursorFile);
  }

  if (context->externalContainers->insert(usr).second && context->verbose) {
    LOG_INFO(
        "Captured [{}] from outside --project ({}) to resolve an in-scope member's logical "
        "parent",
        node.name, cursorFile
    );
  }

  const std::size_t index = context->result->nodes.size();
  context->result->nodes.push_back(std::move(node));
  context->usrToIndex->emplace(usr, index);
}

CXChildVisitResult Visitor(CXCursor cursor, CXCursor parent, CXClientData data)
{
  auto* context = static_cast<VisitContext*>(data);

  CXSourceLocation location = clang_getCursorLocation(cursor);
  CXFile file;
  unsigned line = 0;
  unsigned column = 0;
  unsigned offset = 0;
  clang_getExpansionLocation(location, &file, &line, &column, &offset);
  const std::string cursorFile = ToString(clang_getFileName(file));

  // Scope filter: skip cursors (and their subtrees) that live outside the
  // project root or inside an excluded build/dependency directory, unless
  // external analysis was explicitly requested. This is what lets project
  // *headers* contribute declarations while excluding system and dependency
  // headers. Cursors with no file (compiler builtins) are always skipped.
  // (A cursor's own semantic parent living out of scope -- e.g. a class
  // declared in a header outside a --project scoped at a "src/" subtree --
  // is handled separately below, by EnsureContainerNode; it deliberately
  // does *not* widen this filter, or every out-of-scope #include, right down
  // to libc++'s own internals, would get walked and captured too.)
  if (cursorFile.empty()) {
    return CXChildVisit_Continue;
  }
  if (!context->includeExternal &&
      !ShouldAnalyse(cursorFile, context->projectRoot, *context->excluded)) {
    return CXChildVisit_Continue;
  }

  const std::optional<NodeKind> mappedKind = MapCursorKind(clang_getCursorKind(cursor));
  if (!mappedKind.has_value()) {
    return CXChildVisit_Recurse;
  }

  ASTNode node;
  node.id = (*context->idCounter)++;
  node.kind = mappedKind.value();
  node.name = ToString(clang_getCursorSpelling(cursor));
  node.type = ToString(clang_getTypeSpelling(clang_getCursorType(cursor)));
  node.file = RelativePath(cursorFile, context->projectRoot);
  node.usr = CursorUsr(cursor, node.file);

  node.line = static_cast<int>(line);
  node.column = static_cast<int>(column);

  // End line of the cursor's source extent, so the metrics engine can derive
  // lines of code. Falls back to the start line when unavailable.
  CXSourceLocation endLocation = clang_getRangeEnd(clang_getCursorExtent(cursor));
  unsigned endLine = 0;
  unsigned endColumn = 0;
  unsigned endOffset = 0;
  clang_getExpansionLocation(endLocation, nullptr, &endLine, &endColumn, &endOffset);
  node.lineEnd = (endLine >= line) ? static_cast<int>(endLine) : static_cast<int>(line);

  node.physicalParent = std::filesystem::path(node.file).parent_path().generic_string();

  const bool isDefinition = clang_isCursorDefinition(cursor) != 0;
  if (node.kind == NodeKind::Function && isDefinition) {
    node.cyclomaticComplexity = CyclomaticComplexity(cursor);
  }

  // Record the file's physical line count once, so file/module LOC can be
  // aggregated later. Keyed by the project-relative path used for file nodes.
  if (!node.file.empty() &&
      context->result->fileLineCounts.find(node.file) == context->result->fileLineCounts.end()) {
    context->result->fileLineCounts.emplace(node.file, CountFileLines(cursorFile));
  }

  // Logical parent: the semantic parent, recorded by both simple name (fallback)
  // and USR (primary, robust across translation units). Base specifiers report
  // the translation unit as their semantic parent, so use the traversal parent
  // (the derived class) passed by clang_visitChildren instead.
  CXCursor logicalParentCursor =
      (node.kind == NodeKind::ParentClass) ? parent : clang_getCursorSemanticParent(cursor);
  if (!clang_Cursor_isNull(logicalParentCursor) &&
      clang_getCursorKind(logicalParentCursor) != CXCursor_TranslationUnit) {
    node.logicalParent = ToString(clang_getCursorSpelling(logicalParentCursor));
    // Same file as this cursor: an anonymous namespace cannot span files, so the
    // salt CursorUsr() applies matches the one its members compute for it.
    node.semanticParentUsr = CursorUsr(logicalParentCursor, node.file);
    // A no-op when the parent is already captured (the common case); otherwise
    // it lies outside --project (e.g. this is an out-of-line definition of a
    // class declared in a header --project doesn't cover) and needs synthesizing
    // so GraphBuilder resolves the real parent instead of falling back to the
    // project root.
    EnsureContainerNode(logicalParentCursor, context);
  }

  // Referenced name for relationship-bearing kinds.
  switch (node.kind) {
    case NodeKind::Include: {
      CXFile included = clang_getIncludedFile(cursor);
      const std::string includedPath = ToString(clang_getFileName(included));
      // Project-relative path for in-scope headers (so the graph builder can
      // resolve the include to a file node), bare filename for external ones.
      node.referencedName = RelativePath(includedPath, context->projectRoot);
      if (node.referencedName.empty()) {
        node.referencedName = node.name;
      }
      break;
    }
    case NodeKind::ParentClass: {
      CXCursor referenced = clang_getCursorReferenced(cursor);
      std::string baseName = ToString(clang_getCursorSpelling(referenced));
      if (baseName.empty()) {
        baseName = node.name;
      }
      node.referencedName = baseName;
      if (node.name.empty()) {
        node.name = baseName;
      }
      CollectTypeUsrs(clang_getCursorType(cursor), node.referencedUsrs);
      break;
    }
    case NodeKind::Field:
      node.referencedName = node.type;
      CollectTypeUsrs(clang_getCursorType(cursor), node.referencedUsrs);
      break;
    default:
      break;
  }

  // Cross-translation-unit de-duplication. A header declaration is visited once
  // per TU that includes it, and a function/record is often declared in a header
  // and defined in a .cpp — both share a USR. Keep one node per USR, but let the
  // *definition* win: when the definition arrives after a declaration was
  // captured, upgrade the stored node's source span (so its lines of code
  // reflect the body, not the signature) and its file.
  if (!node.usr.empty()) {
    auto existing = context->usrToIndex->find(node.usr);
    if (existing != context->usrToIndex->end()) {
      if (isDefinition && context->definedUsrs->insert(node.usr).second) {
        ASTNode& stored = context->result->nodes[existing->second];
        stored.line = node.line;
        stored.column = node.column;
        stored.lineEnd = node.lineEnd;
        stored.file = node.file;
        stored.physicalParent = node.physicalParent;
        stored.type = node.type;
        // Only the definition has a body to measure.
        stored.cyclomaticComplexity = node.cyclomaticComplexity;
      }
      return CXChildVisit_Recurse;  // already captured in another TU
    }
    const std::string usr = node.usr;
    const std::size_t index = context->result->nodes.size();
    if (isDefinition) {
      context->definedUsrs->insert(usr);
    }
    context->result->nodes.push_back(std::move(node));
    context->usrToIndex->emplace(usr, index);
    return CXChildVisit_Recurse;
  }

  // USR-less nodes (includes, base specifiers): key on their file / relationship.
  std::string dedupKey;
  if (node.kind == NodeKind::Include) {
    dedupKey = "inc|" + node.file + "|" + node.referencedName;
  }
  else if (node.kind == NodeKind::ParentClass) {
    dedupKey = "base|" + node.semanticParentUsr + "|" + node.referencedName;
  }
  else {
    dedupKey = "loc|" + node.file + "|" + std::to_string(node.line) + "|" + node.name;
  }
  if (!context->seen->insert(dedupKey).second) {
    return CXChildVisit_Recurse;  // already captured in another TU
  }

  context->result->nodes.push_back(std::move(node));
  return CXChildVisit_Recurse;
}

/// Build the libclang argument list for a compile command, dropping flags that
/// break out-of-tree reparsing (output selection, precompiled headers) and the
/// source file itself (passed separately to clang_parseTranslationUnit).
///
/// `-Xclang` forwards the *following* token to cc1, so flags and their values
/// can each arrive wrapped in their own `-Xclang`. CMake emits precompiled
/// headers as `-Xclang -include-pch -Xclang <path>`; dropping `-include-pch`
/// plus the one token after it would eat the second `-Xclang` and leave the
/// path behind, so the wrapper has to be unwrapped before a flag is recognised
/// and re-applied to whatever is kept.
std::vector<std::string> BuildArguments(CXCompileCommand command, const std::string& sourceFile)
{
  const std::string sourceName = std::filesystem::path(sourceFile).filename().string();
  const unsigned count = clang_CompileCommand_getNumArgs(command);

  std::vector<std::string> raw;
  raw.reserve(count);
  for (unsigned index = 1; index < count; ++index) {  // skip argv[0], the compiler
    raw.push_back(ToString(clang_CompileCommand_getArg(command, index)));
  }

  // Flags whose value is a separate token. That value may itself be wrapped.
  auto takesValue = [](const std::string& value) {
    return value == "-o" || value == "-include" || value == "-include-pch" || value == "-imacros";
  };
  // A precompiled header artefact. Matched on extension only: a substring test
  // for "pch" would also swallow any -I path that happens to contain it.
  auto looksLikePch = [](const std::string& value) {
    return value.ends_with(".gch") || value.ends_with(".pch") || value.ends_with(".hxx");
  };

  std::vector<std::string> arguments;
  std::size_t index = 0;
  while (index < raw.size()) {
    const bool wrapped = raw[index] == "-Xclang" && index + 1 < raw.size();
    const std::string& flag = wrapped ? raw[index + 1] : raw[index];
    index += wrapped ? 2 : 1;

    if (flag == "-c" || flag == "-Winvalid-pch") {
      continue;
    }
    if (takesValue(flag)) {
      if (index < raw.size() && raw[index] == "-Xclang") {
        ++index;  // the value carries its own -Xclang wrapper
      }
      if (index < raw.size()) {
        ++index;  // the value itself
      }
      continue;
    }
    if (flag.ends_with(".o") || looksLikePch(flag)) {
      continue;
    }
    if (flag == sourceFile || flag == sourceName ||
        std::filesystem::path(flag).filename().string() == sourceName) {
      continue;
    }

    if (wrapped) {
      arguments.emplace_back("-Xclang");
    }
    arguments.push_back(flag);
  }

  auto hasFlag = [&arguments](std::string_view prefix) {
    return std::any_of(arguments.begin(), arguments.end(), [prefix](const std::string& argument) {
      return argument.starts_with(prefix);
    });
  };

#if defined(PRISM_CLANG_RESOURCE_DIR)
  if (!hasFlag("-resource-dir")) {
    arguments.emplace_back("-resource-dir");
    arguments.emplace_back(PRISM_CLANG_RESOURCE_DIR);
  }
#endif

#if defined(__APPLE__)
  if (!hasFlag("-isysroot") && !hasFlag("--sysroot") && !MacOsSdkPath().empty()) {
    arguments.emplace_back("-isysroot");
    arguments.push_back(MacOsSdkPath());
  }
#endif

  return arguments;
}

}  // namespace

Parser::Parser(ParserConfig config) : config_(std::move(config)) {}

ParseResult Parser::Parse()
{
  ParseResult result;

  std::filesystem::path databaseDir = config_.compileCommandsPath;
  if (databaseDir.empty()) {
    databaseDir = config_.projectRoot / "compile_commands.json";
  }
  if (databaseDir.has_filename() && databaseDir.filename() == "compile_commands.json") {
    databaseDir = databaseDir.parent_path();
  }
  if (databaseDir.empty()) {
    databaseDir = config_.projectRoot;
  }

  const std::filesystem::path databaseFile = databaseDir / "compile_commands.json";
  std::error_code existsError;
  if (!std::filesystem::exists(databaseFile, existsError)) {
    LOG_ERROR(
        "No compile_commands.json found at [{}]. Generate one with "
        "`cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON` (or `bear -- <build command>` for "
        "non-CMake builds), or pass --compile-commands to point at an existing one.",
        databaseFile.string()
    );
    result.hadErrors = true;
    return result;
  }

  CXCompilationDatabase_Error databaseError = CXCompilationDatabase_NoError;
  CXCompilationDatabase database =
      clang_CompilationDatabase_fromDirectory(databaseDir.string().c_str(), &databaseError);
  if (databaseError != CXCompilationDatabase_NoError) {
    LOG_ERROR(
        "Found [{}] but failed to parse it — check that it is valid JSON.", databaseFile.string()
    );
    result.hadErrors = true;
    return result;
  }

  // Effective excluded-directory set: built-in defaults (unless disabled) plus
  // any names the caller added.
  std::unordered_set<std::string> excluded;
  if (config_.useDefaultExcludes) {
    excluded = DefaultExcludedDirectories();
  }
  for (const std::string& name : config_.excludedDirectories) {
    excluded.insert(ToLower(name));
  }

  CXIndex index = clang_createIndex(0, 0);
  CXCompileCommands commands = clang_CompilationDatabase_getAllCompileCommands(database);
  const unsigned commandCount = clang_CompileCommands_getSize(commands);
  int idCounter = 0;
  std::unordered_set<std::string> seen;  // cross-TU dedup keys for USR-less nodes
  std::unordered_map<std::string, std::size_t> usrToIndex;
  std::unordered_set<std::string> definedUsrs;
  std::unordered_set<std::string> externalContainers;  // see VisitContext::externalContainers

  // Diagnostic counters: distinguish "nothing in scope" from "parsed but every
  // TU failed" from "parsed fine but nothing was captured", so a bad --project,
  // a stale compile-commands database, or an over-broad --exclude each surface
  // their own actionable message instead of a silent empty graph.
  unsigned inScopeCount = 0;
  unsigned parsedOkCount = 0;

  for (unsigned commandIndex = 0; commandIndex < commandCount; ++commandIndex) {
    CXCompileCommand command = clang_CompileCommands_getCommand(commands, commandIndex);
    const std::string filename = ToString(clang_CompileCommand_getFilename(command));
    const std::string directory = ToString(clang_CompileCommand_getDirectory(command));

    std::filesystem::path sourcePath(filename);
    if (sourcePath.is_relative() && !directory.empty()) {
      sourcePath = std::filesystem::path(directory) / sourcePath;
    }
    const std::string sourceFile = sourcePath.lexically_normal().string();

    // Skip translation units whose source lives outside the project root or in
    // an excluded build/dependency directory. Project headers are still reached
    // through the in-root TUs that include them.
    if (!config_.includeExternal && !ShouldAnalyse(sourceFile, config_.projectRoot, excluded)) {
      continue;
    }
    ++inScopeCount;

    if (config_.verbose) {
      LOG_INFO("Parsing translation unit [{}]", sourceFile);
    }

    const std::vector<std::string> argumentStrings = BuildArguments(command, sourceFile);
    std::vector<const char*> argumentPointers;
    argumentPointers.reserve(argumentStrings.size());
    for (const std::string& argument : argumentStrings) {
      argumentPointers.push_back(argument.c_str());
    }

    CXTranslationUnit unit = clang_parseTranslationUnit(
        index,
        sourceFile.c_str(),
        argumentPointers.data(),
        static_cast<int>(argumentPointers.size()),
        nullptr,
        0,
        CXTranslationUnit_DetailedPreprocessingRecord
    );

    if (unit == nullptr) {
      const std::string warning = "Failed to parse translation unit: " + sourceFile;
      LOG_WARNING("{}", warning);
      result.warnings.push_back(warning);
      continue;
    }

    // An error diagnostic means clang recovered rather than gave up: the cursor
    // tree still walks, but unresolved types silently degrade (a std::string
    // field reports as `int`), which quietly corrupts every downstream metric.
    // Surface the first error so a bad argument list is diagnosable.
    const unsigned diagnosticCount = clang_getNumDiagnostics(unit);
    std::string firstError;
    for (unsigned diagnosticIndex = 0; diagnosticIndex < diagnosticCount; ++diagnosticIndex) {
      CXDiagnostic diagnostic = clang_getDiagnostic(unit, diagnosticIndex);
      if (firstError.empty() && clang_getDiagnosticSeverity(diagnostic) >= CXDiagnostic_Error) {
        firstError = ToString(clang_getDiagnosticSpelling(diagnostic));
      }
      clang_disposeDiagnostic(diagnostic);
    }
    if (!firstError.empty()) {
      const std::string warning =
          "Parse diagnostics reported errors in: " + sourceFile + " (first: " + firstError + ")";
      LOG_WARNING("{}", warning);
      result.warnings.push_back(warning);
    }
    ++parsedOkCount;

    CXCursor rootCursor = clang_getTranslationUnitCursor(unit);
    VisitContext context{
        &result,
        config_.projectRoot,
        &idCounter,
        config_.includeExternal,
        config_.verbose,
        &seen,
        &excluded,
        &usrToIndex,
        &definedUsrs,
        &externalContainers
    };
    clang_visitChildren(rootCursor, Visitor, &context);

    clang_disposeTranslationUnit(unit);
  }

  clang_CompileCommands_dispose(commands);
  clang_CompilationDatabase_dispose(database);
  clang_disposeIndex(index);

  if (commandCount == 0) {
    LOG_ERROR(
        "Compile commands database at [{}] contains no translation units; nothing to analyse.",
        databaseFile.string()
    );
    result.hadErrors = true;
  }
  else if (inScopeCount == 0) {
    LOG_ERROR(
        "None of the {} translation unit(s) in [{}] fall under --project [{}] (after "
        "exclusions). This usually means the database was generated on a different machine or "
        "in a container, so its paths don't match this checkout. Regenerate it locally (e.g. "
        "`cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`), point --compile-commands at a "
        "fresh one, or pass --include-external to analyse it regardless of path.",
        commandCount, databaseFile.string(), config_.projectRoot.string()
    );
    result.hadErrors = true;
  }
  else if (parsedOkCount == 0) {
    LOG_ERROR(
        "All {} in-scope translation unit(s) failed to parse; see the warning(s) above for the "
        "specific failures.",
        inScopeCount
    );
    result.hadErrors = true;
  }
  else if (result.nodes.empty()) {
    LOG_WARNING(
        "{} translation unit(s) parsed successfully but no declarations were captured within "
        "scope; the resulting graph will be empty.",
        parsedOkCount
    );
  }

  if (!externalContainers.empty()) {
    LOG_WARNING(
        "{} declaration(s) outside --project were captured to complete the logical structure of "
        "in-scope code (e.g. a class declared in a header outside --project, defined by an "
        "in-scope .cpp) -- their own members are not analysed. Widen --project or pass "
        "--include-external to include them fully; re-run with --verbose to see which ones.",
        externalContainers.size()
    );
  }

  if (config_.verbose) {
    LOG_INFO(
        "Compile database: {} total, {} in scope, {} parsed OK.", commandCount, inScopeCount,
        parsedOkCount
    );
  }

  return result;
}

}  // namespace Prism

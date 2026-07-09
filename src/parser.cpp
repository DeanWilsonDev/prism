#include "prism/parser.hpp"

#include <clang-c/CXCompilationDatabase.h>
#include <clang-c/CXString.h>
#include <clang-c/Index.h>
#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
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

/// True when `absolutePath` resolves to a location inside `projectRoot`.
bool IsInProjectScope(const std::string& absolutePath, const std::filesystem::path& projectRoot)
{
  if (absolutePath.empty()) {
    return false;
  }
  std::filesystem::path relative =
      Canonicalise(absolutePath).lexically_relative(Canonicalise(projectRoot));
  return !relative.empty() && *relative.begin() != "..";
}

struct VisitContext {
  ParseResult* result;
  std::filesystem::path projectRoot;
  int* idCounter;
  bool includeExternal;
  std::unordered_set<std::string>* seen;  // dedup keys across translation units
};

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
  // project root, unless external analysis was explicitly requested. This is
  // what lets project *headers* contribute declarations while excluding system
  // and dependency headers. Cursors with no file (compiler builtins) are always
  // skipped.
  if (cursorFile.empty()) {
    return CXChildVisit_Continue;
  }
  if (!context->includeExternal && !IsInProjectScope(cursorFile, context->projectRoot)) {
    return CXChildVisit_Continue;
  }

  const std::optional<NodeKind> mappedKind = MapCursorKind(clang_getCursorKind(cursor));
  if (!mappedKind.has_value()) {
    return CXChildVisit_Recurse;
  }

  // Skip forward declarations of records (`class Foo;`). A forward declaration
  // and the real definition share a USR, and the first seen wins the dedup — so
  // without this a class node could point at a bodyless declaration in an
  // unrelated header, losing its extent, fields, and methods.
  if ((mappedKind.value() == NodeKind::Class || mappedKind.value() == NodeKind::Struct) &&
      clang_isCursorDefinition(cursor) == 0) {
    return CXChildVisit_Recurse;
  }

  ASTNode node;
  node.id = (*context->idCounter)++;
  node.kind = mappedKind.value();
  node.name = ToString(clang_getCursorSpelling(cursor));
  node.type = ToString(clang_getTypeSpelling(clang_getCursorType(cursor)));
  node.usr = ToString(clang_getCursorUSR(cursor));

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

  node.file = RelativePath(cursorFile, context->projectRoot);
  node.physicalParent = std::filesystem::path(node.file).parent_path().generic_string();

  // Logical parent: the semantic parent, recorded by both simple name (fallback)
  // and USR (primary, robust across translation units). Base specifiers report
  // the translation unit as their semantic parent, so use the traversal parent
  // (the derived class) passed by clang_visitChildren instead.
  CXCursor logicalParentCursor =
      (node.kind == NodeKind::ParentClass) ? parent : clang_getCursorSemanticParent(cursor);
  if (!clang_Cursor_isNull(logicalParentCursor) &&
      clang_getCursorKind(logicalParentCursor) != CXCursor_TranslationUnit) {
    node.logicalParent = ToString(clang_getCursorSpelling(logicalParentCursor));
    node.semanticParentUsr = ToString(clang_getCursorUSR(logicalParentCursor));
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
      break;
    }
    case NodeKind::Field:
      node.referencedName = node.type;
      break;
    default:
      break;
  }

  // Cross-translation-unit de-duplication. A header declaration is visited once
  // per TU that includes it; keep only the first occurrence. USR is the stable
  // identity where clang provides one; includes and base specifiers have none,
  // so key those on their file/relationship instead.
  std::string dedupKey;
  if (!node.usr.empty()) {
    dedupKey = "usr|" + node.usr;
  }
  else if (node.kind == NodeKind::Include) {
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
std::vector<std::string> BuildArguments(CXCompileCommand command, const std::string& sourceFile)
{
  const std::string sourceName = std::filesystem::path(sourceFile).filename().string();
  std::vector<std::string> arguments;
  const unsigned count = clang_CompileCommand_getNumArgs(command);

  auto looksLikePch = [](const std::string& value) {
    return value.find("pch") != std::string::npos || value.ends_with(".gch") ||
           value.ends_with(".pch") || value.ends_with(".hxx");
  };

  for (unsigned index = 1; index < count; ++index) {  // skip argv[0], the compiler
    std::string argument = ToString(clang_CompileCommand_getArg(command, index));
    if (argument == "-c" || argument == "-Winvalid-pch") {
      continue;
    }
    if (argument == "-o" || argument == "-include" || argument == "-include-pch") {
      if (index + 1 < count) {
        ++index;  // drop the paired value as well
      }
      continue;
    }
    if (argument.ends_with(".o")) {
      continue;
    }
    if (argument == sourceFile || argument == sourceName ||
        std::filesystem::path(argument).filename().string() == sourceName) {
      continue;
    }
    if (looksLikePch(argument)) {
      continue;
    }
    arguments.push_back(std::move(argument));
  }
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

  CXCompilationDatabase_Error databaseError = CXCompilationDatabase_NoError;
  CXCompilationDatabase database =
      clang_CompilationDatabase_fromDirectory(databaseDir.string().c_str(), &databaseError);
  if (databaseError != CXCompilationDatabase_NoError) {
    LOG_ERROR("Failed to load compile_commands.json from [{}]", databaseDir.string());
    result.hadErrors = true;
    return result;
  }

  CXIndex index = clang_createIndex(0, 0);
  CXCompileCommands commands = clang_CompilationDatabase_getAllCompileCommands(database);
  const unsigned commandCount = clang_CompileCommands_getSize(commands);
  int idCounter = 0;
  std::unordered_set<std::string> seen;  // cross-TU de-duplication keys

  for (unsigned commandIndex = 0; commandIndex < commandCount; ++commandIndex) {
    CXCompileCommand command = clang_CompileCommands_getCommand(commands, commandIndex);
    const std::string filename = ToString(clang_CompileCommand_getFilename(command));
    const std::string directory = ToString(clang_CompileCommand_getDirectory(command));

    std::filesystem::path sourcePath(filename);
    if (sourcePath.is_relative() && !directory.empty()) {
      sourcePath = std::filesystem::path(directory) / sourcePath;
    }
    const std::string sourceFile = sourcePath.lexically_normal().string();

    // Skip translation units whose source lives outside the project root. Their
    // in-scope project headers are still reached through the TUs that live
    // inside the root and include them.
    if (!config_.includeExternal && !IsInProjectScope(sourceFile, config_.projectRoot)) {
      continue;
    }

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

    const unsigned diagnosticCount = clang_getNumDiagnostics(unit);
    bool hasErrorDiagnostic = false;
    for (unsigned diagnosticIndex = 0; diagnosticIndex < diagnosticCount; ++diagnosticIndex) {
      CXDiagnostic diagnostic = clang_getDiagnostic(unit, diagnosticIndex);
      if (clang_getDiagnosticSeverity(diagnostic) >= CXDiagnostic_Error) {
        hasErrorDiagnostic = true;
      }
      clang_disposeDiagnostic(diagnostic);
    }
    if (hasErrorDiagnostic) {
      const std::string warning = "Parse diagnostics reported errors in: " + sourceFile;
      LOG_WARNING("{}", warning);
      result.warnings.push_back(warning);
    }

    CXCursor rootCursor = clang_getTranslationUnitCursor(unit);
    VisitContext context{&result, config_.projectRoot, &idCounter, config_.includeExternal, &seen};
    clang_visitChildren(rootCursor, Visitor, &context);

    clang_disposeTranslationUnit(unit);
  }

  clang_CompileCommands_dispose(commands);
  clang_CompilationDatabase_dispose(database);
  clang_disposeIndex(index);

  return result;
}

}  // namespace Prism

#include "prism/parser.hpp"

#include <clang-c/CXCompilationDatabase.h>
#include <clang-c/CXString.h>
#include <clang-c/Index.h>
#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
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
std::string RelativePath(const std::string& target, const std::filesystem::path& root)
{
  if (target.empty()) {
    return "";
  }
  std::error_code ec;
  std::filesystem::path absTarget = std::filesystem::weakly_canonical(target, ec);
  if (ec) {
    absTarget = std::filesystem::absolute(target, ec);
  }
  std::filesystem::path absRoot = std::filesystem::weakly_canonical(root, ec);
  if (ec) {
    absRoot = std::filesystem::absolute(root, ec);
  }
  std::filesystem::path relative = absTarget.lexically_relative(absRoot);
  if (relative.empty() || *relative.begin() == "..") {
    return std::filesystem::path(target).filename().generic_string();
  }
  return relative.generic_string();
}

struct VisitContext {
  ParseResult* result;
  std::filesystem::path projectRoot;
  int* idCounter;
};

CXChildVisitResult Visitor(CXCursor cursor, CXCursor parent, CXClientData data)
{
  auto* context = static_cast<VisitContext*>(data);

  CXSourceLocation location = clang_getCursorLocation(cursor);
  if (!clang_Location_isFromMainFile(location)) {
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

  CXFile file;
  unsigned line = 0;
  unsigned column = 0;
  unsigned offset = 0;
  clang_getExpansionLocation(location, &file, &line, &column, &offset);
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

  node.file = RelativePath(ToString(clang_getFileName(file)), context->projectRoot);
  node.physicalParent = std::filesystem::path(node.file).parent_path().generic_string();

  // Logical parent: the semantic parent's simple name, or empty at TU root.
  // Base specifiers report the translation unit as their semantic parent, so
  // use the traversal parent (the derived class) passed by clang_visitChildren.
  CXCursor logicalParentCursor =
      (node.kind == NodeKind::ParentClass) ? parent : clang_getCursorSemanticParent(cursor);
  if (!clang_Cursor_isNull(logicalParentCursor) &&
      clang_getCursorKind(logicalParentCursor) != CXCursor_TranslationUnit) {
    node.logicalParent = ToString(clang_getCursorSpelling(logicalParentCursor));
  }

  // Referenced name for relationship-bearing kinds.
  switch (node.kind) {
    case NodeKind::Include: {
      CXFile included = clang_getIncludedFile(cursor);
      const std::string includedPath = ToString(clang_getFileName(included));
      node.referencedName = std::filesystem::path(includedPath).filename().generic_string();
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

  for (unsigned commandIndex = 0; commandIndex < commandCount; ++commandIndex) {
    CXCompileCommand command = clang_CompileCommands_getCommand(commands, commandIndex);
    const std::string filename = ToString(clang_CompileCommand_getFilename(command));
    const std::string directory = ToString(clang_CompileCommand_getDirectory(command));

    std::filesystem::path sourcePath(filename);
    if (sourcePath.is_relative() && !directory.empty()) {
      sourcePath = std::filesystem::path(directory) / sourcePath;
    }
    const std::string sourceFile = sourcePath.lexically_normal().string();

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
    VisitContext context{&result, config_.projectRoot, &idCounter};
    clang_visitChildren(rootCursor, Visitor, &context);

    clang_disposeTranslationUnit(unit);
  }

  clang_CompileCommands_dispose(commands);
  clang_CompilationDatabase_dispose(database);
  clang_disposeIndex(index);

  return result;
}

}  // namespace Prism

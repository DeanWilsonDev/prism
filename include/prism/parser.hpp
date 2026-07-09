#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include "ast-node.hpp"

namespace Prism {

struct ParseResult {
  std::vector<ASTNode> nodes;
  std::vector<std::string> warnings;
  bool hadErrors = false;
};

struct ParserConfig {
  std::filesystem::path projectRoot;
  std::filesystem::path compileCommandsPath;
  bool verbose = false;
  // When false (default), only files under projectRoot are analysed. When true,
  // every translation unit in the database is parsed and declarations from any
  // file (including dependencies and system headers) are captured.
  bool includeExternal = false;
};

/// Stage 1. Loads a compilation database, parses every translation unit with
/// libclang, and flattens the interesting cursors into a list of ASTNodes.
class Parser {
 public:
  explicit Parser(ParserConfig config);
  ParseResult Parse();

 private:
  ParserConfig config_;
};

}  // namespace Prism

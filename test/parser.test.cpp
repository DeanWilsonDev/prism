#include <cimmerian/test.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include "prism/logging/log.hpp"
#include "prism/parser.hpp"

#ifndef PRISM_TEST_EXAMPLE_DIR
#define PRISM_TEST_EXAMPLE_DIR "src/example"
#endif

namespace {

// Write a throwaway compile_commands.json describing the example fixture and
// return the directory it lives in (what the parser loads).
std::filesystem::path WriteExampleDatabase()
{
  const std::filesystem::path exampleDir = PRISM_TEST_EXAMPLE_DIR;
  const std::filesystem::path source = exampleDir / "test-input.cpp";
  const std::filesystem::path databaseDir =
      std::filesystem::temp_directory_path() / "prism-parser-test";
  std::filesystem::create_directories(databaseDir);

  std::ofstream out(databaseDir / "compile_commands.json");
  out << "[\n"
      << "  {\n"
      << "    \"directory\": \"" << exampleDir.generic_string() << "\",\n"
      << "    \"file\": \"" << source.generic_string() << "\",\n"
      << "    \"command\": \"c++ -std=c++20 -c " << source.generic_string() << "\"\n"
      << "  }\n"
      << "]\n";
  return databaseDir;
}

bool HasNode(const Prism::ParseResult& result, Prism::NodeKind kind, const std::string& name)
{
  for (const Prism::ASTNode& node : result.nodes) {
    if (node.kind == kind && node.name == name) {
      return true;
    }
  }
  return false;
}

bool HasReferenced(const Prism::ParseResult& result, Prism::NodeKind kind, const std::string& ref)
{
  for (const Prism::ASTNode& node : result.nodes) {
    if (node.kind == kind && node.referencedName == ref) {
      return true;
    }
  }
  return false;
}

// Cimmerian's IT() body is a captureless lambda, so shared setup lives behind a
// function with a static local (parsed once, reused across cases).
const Prism::ParseResult& SharedResult()
{
  static const Prism::ParseResult result = []() {
    Prism::Logging::Log::Init();
    const std::filesystem::path databaseDir = WriteExampleDatabase();
    Prism::ParserConfig config;
    config.projectRoot = PRISM_TEST_EXAMPLE_DIR;
    config.compileCommandsPath = databaseDir / "compile_commands.json";
    return Prism::Parser(config).Parse();
  }();
  return result;
}

}  // namespace

DESCRIBE("Parser", {
  IT("parses the fixture without database errors", {
    ASSERT_FALSE(SharedResult().hadErrors);
    ASSERT_TRUE(!SharedResult().nodes.empty());
  });

  IT("extracts the TestInput class",
     { ASSERT_TRUE(HasNode(SharedResult(), Prism::NodeKind::Class, "TestInput")); });

  IT("extracts the Tester namespace",
     { ASSERT_TRUE(HasNode(SharedResult(), Prism::NodeKind::Namespace, "Tester")); });

  IT("extracts the TestInputBase parent class", {
    ASSERT_TRUE(HasReferenced(SharedResult(), Prism::NodeKind::ParentClass, "TestInputBase"));
  });

  IT("extracts the test-input.hpp include",
     { ASSERT_TRUE(HasReferenced(SharedResult(), Prism::NodeKind::Include, "test-input.hpp")); });
});

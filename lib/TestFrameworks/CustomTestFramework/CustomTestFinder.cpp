#include "TestFrameworks/CustomTestFramework/CustomTestFinder.h"

#include "Config/Configuration.h"
#include "Filter.h"
#include "Logger.h"
#include "Program/Program.h"
#include "TestFrameworks/CustomTestFramework/CustomTest_Test.h"

#include <llvm/IR/Module.h>

#include <map>
#include <string>
#include <vector>

using namespace mull;
using namespace llvm;

CustomTestFinder::CustomTestFinder(
    const std::vector<CustomTestDefinition> &definitions)
    : testDefinitions(definitions) {}

std::vector<std::unique_ptr<Test>> CustomTestFinder::findTests(Program &program,
                                                               Filter &filter) {
  std::map<std::string, size_t> testMapping;
  for (size_t i = 0; i < testDefinitions.size(); i++) {
    const CustomTestDefinition &definition = testDefinitions[i];
    testMapping[definition.methodName] = i;
  }

  std::vector<std::unique_ptr<Test>> tests;

  for (auto &currentModule : program.modules()) {
    for (auto &function : currentModule->getModule()->getFunctionList()) {
      if (function.isDeclaration()) {
        continue;
      }

      const std::string &functionName = function.getName().str();
      if (testMapping.count(functionName) == 0) {
        continue;
      }

      const CustomTestDefinition &definition =
          testDefinitions[testMapping.at(functionName)];
      if (filter.shouldSkipTest(definition.testName)) {
        continue;
      }

      std::string programName = definition.programName;
      if (programName.empty()) {
        programName = "mull";
      }

      tests.emplace_back(make_unique<CustomTest_Test>(
          definition.testName, programName, definition.callArguments, &function,
          program.getStaticConstructors()));
    }
  }

  return tests;
}

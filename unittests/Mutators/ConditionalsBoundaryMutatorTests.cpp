#include "Config/Configuration.h"
#include "Context.h"
#include "Mutators/ConditionalsBoundaryMutator.h"
#include "MutationPoint.h"
#include "Toolchain/Compiler.h"
#include "Toolchain/Toolchain.h"
#include "Filter.h"
#include "Testee.h"
#include "MutationsFinder.h"
#include "FixturePaths.h"

#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include "gtest/gtest.h"

using namespace mull;
using namespace llvm;

TEST(ConditionalsBoundaryMutator, findMutations) {
  LLVMContext llvmContext;
  ModuleLoader loader;
  auto mullModule = loader.loadModuleAtPath(fixtures::mutators_boundary_module_bc_path(), llvmContext);
  auto module = mullModule->getModule();

  Context mullContext;
  mullContext.addModule(std::move(mullModule));
  Configuration configuration;

  std::vector<std::unique_ptr<Mutator>> mutators;
  mutators.emplace_back(make_unique<ConditionalsBoundaryMutator>());
  MutationsFinder finder(std::move(mutators), configuration);
  Filter filter;

  std::vector<std::unique_ptr<Testee>> testees;
  for (auto &function : *module) {
    testees.emplace_back(make_unique<Testee>(&function, nullptr, 1));
  }
  auto mergedTestees = mergeTestees(testees);
  std::vector<MutationPoint *> points = finder.getMutationPoints(mullContext, mergedTestees, filter);

  ASSERT_EQ(points.size(), 7U);
}

TEST(ConditionalsBoundaryMutator, applyMutations) {
  LLVMContext llvmContext;
  ModuleLoader loader;
  auto mullModule = loader.loadModuleAtPath(fixtures::mutators_boundary_module_bc_path(), llvmContext);
  auto borrowedModule = mullModule.get();
  auto module = borrowedModule->getModule();

  Context mullContext;
  mullContext.addModule(std::move(mullModule));
  Configuration configuration;

  std::vector<std::unique_ptr<Mutator>> mutators;
  mutators.emplace_back(make_unique<ConditionalsBoundaryMutator>());
  MutationsFinder finder(std::move(mutators), configuration);
  Filter filter;

  std::vector<std::unique_ptr<Testee>> testees;
  for (auto &function : *module) {
    testees.emplace_back(make_unique<Testee>(&function, nullptr, 1));
  }
  auto mergedTestees = mergeTestees(testees);

  std::vector<MutationPoint *> points = finder.getMutationPoints(mullContext, mergedTestees, filter);

  for (auto point: points) {
    ValueToValueMapTy map;
    auto mutatedFunction = CloneFunction(point->getOriginalFunction(), map);

    Instruction *originalInstruction = &point->getAddress().findInstruction(point->getOriginalFunction());
    point->setMutatedFunction(mutatedFunction);
    point->applyMutation();
    Instruction *mutatedInstruction = &point->getAddress().findInstruction(mutatedFunction);

    if (ConditionalsBoundaryMutator::isGT(originalInstruction)) {
      ASSERT_TRUE(ConditionalsBoundaryMutator::isGTE(mutatedInstruction));
    }
    else if (ConditionalsBoundaryMutator::isGTE(originalInstruction)) {
      ASSERT_TRUE(ConditionalsBoundaryMutator::isGT(mutatedInstruction));
    }
    else if (ConditionalsBoundaryMutator::isLTE(originalInstruction)) {
      ASSERT_TRUE(ConditionalsBoundaryMutator::isLT(mutatedInstruction));
    }
    else if (ConditionalsBoundaryMutator::isLT(originalInstruction)) {
      ASSERT_TRUE(ConditionalsBoundaryMutator::isLTE(mutatedInstruction));
    }
  }

  ASSERT_EQ(points.size(), 7U);
}

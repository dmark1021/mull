#include <ebc/BitcodeArchive.h>
#include <ebc/BitcodeContainer.h>
#include <ebc/BitcodeMetadata.h>
#include <ebc/BitcodeRetriever.h>
#include <ebc/EmbeddedBitcode.h>
#include <ebc/EmbeddedFile.h>

#include <llvm/Support/MemoryBuffer.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/TargetSelect.h>

#include <iostream>

#include "Config/Configuration.h"
#include "Driver.h"
#include "JunkDetection/JunkDetector.h"
#include "Metrics/Metrics.h"
#include "ModuleLoader.h"
#include "MutationsFinder.h"
#include "Mutators/MutatorsFactory.h"
#include "Parallelization/Parallelization.h"
#include "Program/Program.h"
#include "Result.h"
#include "TestFrameworks/TestFrameworkFactory.h"

/// Temp includes to make it running

#include "Reporters/SQLiteReporter.h"
#include "Config/RawConfig.h"
#include "Metrics/Metrics.h"

class LoadModuleFromBitcodeTask {
public:
  using In = std::vector<std::unique_ptr<ebc::EmbeddedFile>>;
  using Out = std::vector<std::unique_ptr<mull::MullModule>>;
  using iterator = In::iterator;

  explicit LoadModuleFromBitcodeTask(llvm::LLVMContext &context)
      : context(context) {}

  void operator()(iterator begin, iterator end, Out &storage,
                  mull::progress_counter &counter) {
    for (auto it = begin; it != end; it++, counter.increment()) {
      auto pair = (*it)->GetRawBuffer();
      const auto data = pair.first;
      const auto size = pair.second;
      assert(data);
      assert(size);

      llvm::StringRef bufferView(data, size);
      auto ownedBuffer = llvm::MemoryBuffer::getMemBufferCopy(bufferView);
      auto buffer = ownedBuffer.get();

      auto modulePair = mull::loadModuleFromBuffer(context, *buffer);
      auto hash = modulePair.first;
      auto module = std::move(modulePair.second);
      assert(module && "Could not load module");
      module->setModuleIdentifier(hash);

      auto mullModule = llvm::make_unique<mull::MullModule>(
          std::move(module), std::move(ownedBuffer), hash);
      storage.push_back(std::move(mullModule));
    }
  }

private:
  llvm::LLVMContext &context;
};

llvm::cl::OptionCategory MullCXXCategory("mull-cxx");

llvm::cl::opt<std::string> InputFile(llvm::cl::Positional,
                                     llvm::cl::desc("<input files>"),
                                     llvm::cl::Required);

llvm::cl::opt<unsigned> Workers("workers", llvm::cl::Optional,
                                llvm::cl::desc("How many threads to use"),
                                llvm::cl::cat(MullCXXCategory));

enum MutatorsOptionIndex : int {
  _mutatorsOptionIndex_unused
};
llvm::cl::list<MutatorsOptionIndex> Mutators("mutators",
                                             llvm::cl::ZeroOrMore,
                                             llvm::cl::desc("Choose mutators:"),
                                             llvm::cl::cat(MullCXXCategory));

class MutatorsCLIOptions {
public:
  explicit MutatorsCLIOptions(llvm::cl::list<MutatorsOptionIndex> &parameter)
  : options(factory.commandLineOptions()), parameter(parameter) {
    int index = 0;
    for (auto &option : options) {
      parameter.getParser().addLiteralOption(option.first.c_str(),
                                             index++,
                                             option.second.c_str());
    }
  }

  std::vector<std::unique_ptr<mull::Mutator>> mutators() {
    std::vector<std::string> selectedGroups;
    for (int i = 0; i < parameter.size(); i++) {
      auto &name = parameter[i];
      selectedGroups.push_back(options[name].first);
    }

    return factory.mutators(selectedGroups);
  }
private:
  mull::MutatorsFactory factory;
  std::vector<std::pair<std::string, std::string>> options;
  llvm::cl::list<MutatorsOptionIndex> &parameter;
};

int main(int argc, char **argv) {
  MutatorsCLIOptions mutatorsOptions(Mutators);

  llvm::cl::HideUnrelatedOptions(MullCXXCategory);
  llvm::cl::ParseCommandLineOptions(argc, argv);

  mull::MetricsMeasure totalExecutionTime;
  totalExecutionTime.start();

  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();

  mull::Configuration configuration;
  configuration.failFastEnabled = true;

  if (Workers) {
    mull::ParallelizationConfig parallelizationConfig;
    parallelizationConfig.workers = Workers;
    parallelizationConfig.normalize();
    configuration.parallelization = parallelizationConfig;
  } else {
    configuration.parallelization = mull::ParallelizationConfig::defaultConfig();
  }

  std::vector<std::unique_ptr<ebc::EmbeddedFile>> embeddedFiles;
  mull::SingleTaskExecutor extractBitcodeBuffers(
      "Extracting bitcode from executable", [&] {
        ebc::BitcodeRetriever bitcodeRetriever(InputFile.getValue());
        for (auto &bitcodeInfo : bitcodeRetriever.GetBitcodeInfo()) {
          auto &container = bitcodeInfo.bitcodeContainer;
          if (container) {
            for (auto &file : container->GetRawEmbeddedFiles()) {
              embeddedFiles.push_back(std::move(file));
            }
          } else {
            mull::Logger::warn() << "No bitcode: " << bitcodeInfo.arch << "\n";
          }
        }
      });
  extractBitcodeBuffers.execute();

  std::vector<std::unique_ptr<llvm::LLVMContext>> contexts;
  std::vector<LoadModuleFromBitcodeTask> tasks;
  for (int i = 0; i < configuration.parallelization.workers; i++) {
    auto context = llvm::make_unique<llvm::LLVMContext>();
    tasks.emplace_back(LoadModuleFromBitcodeTask(*context));
    contexts.push_back(std::move(context));
  }
  std::vector<std::unique_ptr<mull::MullModule>> modules;
  mull::TaskExecutor<LoadModuleFromBitcodeTask> executor(
      "Loading bitcode files", embeddedFiles, modules, std::move(tasks));
  executor.execute();

  mull::Program program({}, {}, std::move(modules));

  mull::Toolchain toolchain(configuration);

  mull::TestFrameworkFactory testFrameworkFactory(configuration, toolchain);
  mull::TestFramework testFramework(testFrameworkFactory.googleTestFramework());

  mull::Filter filter;
  mull::NullJunkDetector junkDetector;
  mull::MutatorsFactory factory;
  mull::MutationsFinder mutationsFinder(mutatorsOptions.mutators(),
                                        configuration);

  mull::Metrics metrics;

  mull::Driver driver(configuration, program, testFramework, toolchain, filter,
                      mutationsFinder, metrics, junkDetector);
  metrics.beginRun();
  auto result = driver.Run();
  metrics.endRun();

  mull::RawConfig rawConfig;
  mull::SQLiteReporter reporter;
  reporter.reportResults(*result, rawConfig, metrics);

  llvm::llvm_shutdown();

  totalExecutionTime.finish();
  mull::Logger::info() << "\nTotal execution time: "
                       << totalExecutionTime.duration()
                       << mull::MetricsMeasure::precision() << "\n";

  return 0;
}

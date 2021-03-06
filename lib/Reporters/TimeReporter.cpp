#include "Reporters/TimeReporter.h"
#include "Metrics/Metrics.h"

using namespace mull;

void TimeReporter::reportResults(const Result &result,
                                 const RawConfig &config,
                                 const Metrics &metrics) {
  metrics.dump();
}

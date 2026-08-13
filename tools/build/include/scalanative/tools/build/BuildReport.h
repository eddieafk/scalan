#pragma once

#include "scalanative/tools/build/BuildDriver.h"

#include <string>

namespace scalanative::tools::build {

[[nodiscard]] std::string buildReportJson(const BuildResult& result,
                                          const BuildOptions& options);

} // namespace scalanative::tools::build

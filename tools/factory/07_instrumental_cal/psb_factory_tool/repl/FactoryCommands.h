#pragma once

#include "FactorySession.h"
#include <cli/cli.h>
#include <memory>

namespace psb::factory {

std::unique_ptr<cli::Menu> buildRootMenu(FactorySession& session);

} // namespace psb::factory

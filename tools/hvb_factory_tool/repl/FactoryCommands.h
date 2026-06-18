#pragma once

#include "FactorySession.h"
#include <cli/cli.h>
#include <memory>

namespace hvb::factory {

std::unique_ptr<cli::Menu> buildRootMenu(FactorySession& session);

} // namespace hvb::factory

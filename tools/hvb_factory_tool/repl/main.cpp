#include "FactorySession.h"
#include "FactoryCommands.h"
#include <cli/cli.h>
#include <cli/loopscheduler.h>
#include <cli/clilocalsession.h>
#include <iostream>

int main() {
    hvb::factory::FactorySession session;
    auto rootMenu = hvb::factory::buildRootMenu(session);

    cli::Cli app(std::move(rootMenu));
    cli::LoopScheduler sched;
    cli::CliLocalTerminalSession localSession(app, sched, std::cout);
    localSession.ExitAction([&sched, &session](auto& out) {
        session.stopWatch();
        session.disconnect();
        out << "Goodbye.\n";
        sched.Stop();
    });
    sched.Run();
    return 0;
}

#include <cli/cli.h>
#include <cli/loopscheduler.h>
#include <cli/clilocalsession.h>
#include <iostream>

int main() {
    auto rootMenu = std::make_unique<cli::Menu>("factory");
    rootMenu->Insert("info", [](std::ostream& out) {
        out << "HVB Factory Calibration Tool v0.1\n";
    }, "Show tool version");

    cli::Cli app(std::move(rootMenu));
    cli::LoopScheduler sched;
    cli::CliLocalTerminalSession session(app, sched, std::cout);
    session.ExitAction([&sched](auto& out) {
        out << "Goodbye.\n";
        sched.Stop();
    });
    sched.Run();
    return 0;
}

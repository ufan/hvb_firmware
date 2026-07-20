#include "FactorySession.h"
#include "FactoryCommands.h"
#include <cli/cli.h>
#include <cli/loopscheduler.h>
#include <cli/clilocalsession.h>
#include <iostream>
#include <string>

namespace {

void printUsage(const char* prog) {
    std::cerr << "Usage: " << prog << " -p <port> [-b <baud>] [-i <slaveId>]\n"
              << "  -p, --port   Serial port to connect to (required, e.g. /dev/ttyUSB0)\n"
              << "  -b, --baud   Baud rate (default 115200)\n"
              << "  -i, --id     Modbus slave id (default 1)\n"
              << "\n"
              << "The connection is established once at startup; there is no\n"
              << "'connect' command inside the REPL.\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string port;
    int baud = 115200;
    int slaveId = 1;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        try {
            if ((a == "-p" || a == "--port") && i + 1 < argc) port = argv[++i];
            else if ((a == "-b" || a == "--baud") && i + 1 < argc) baud = std::stoi(argv[++i]);
            else if ((a == "-i" || a == "--id") && i + 1 < argc) slaveId = std::stoi(argv[++i]);
            else if (a == "-h" || a == "--help") { printUsage(argv[0]); return 0; }
            else { std::cerr << "Unknown or incomplete argument: " << a << "\n"; printUsage(argv[0]); return 1; }
        } catch (const std::exception&) {
            std::cerr << "Error: invalid value for " << a << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    if (port.empty()) {
        std::cerr << "Error: -p/--port is required\n";
        printUsage(argv[0]);
        return 1;
    }

    psb::factory::FactorySession session;
    if (!session.connect(port, baud, slaveId)) {
        std::cerr << "Error: failed to connect to " << port << ": " << session.lastError() << "\n";
        return 1;
    }
    std::cout << "Connected to " << port << " @ " << baud << " id=" << slaveId << "\n";

    auto rootMenu = psb::factory::buildRootMenu(session);

    cli::Cli app(std::move(rootMenu));
    cli::LoopScheduler sched;
    cli::CliLocalTerminalSession localSession(app, sched, std::cout);
    localSession.ExitAction([&sched, &session](auto& out) {
        session.disconnect();
        out << "Goodbye.\n";
        sched.Stop();
    });
    sched.Run();
    return 0;
}

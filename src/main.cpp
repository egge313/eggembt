// eggembt: model-based testing with Petri-net specifications.
#include "Version.h"  // DeepSeek V4.1 Flash: the version lives in one place
#include "cli/Commands.h"

#include <exception>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        cli::printUsage(std::cerr);
        return cli::kExitUsage;
    }
    const std::string command = args.front();
    args.erase(args.begin());

    try {
        if (command == "help" || command == "--help" || command == "-h") {
            if (!args.empty() && args[0] != "help") {
                args.push_back("--help");
                const std::string target = args[0];
                args.clear();
                args.push_back("--help");
                if (target == "check") return cli::cmdCheck(args, std::cout, std::cerr);
                if (target == "explore") return cli::cmdExplore(args, std::cout, std::cerr);
                if (target == "run") return cli::cmdRun(args, std::cout, std::cerr);
                if (target == "report") return cli::cmdReport(args, std::cout, std::cerr);
                if (target == "compare") return cli::cmdCompare(args, std::cout, std::cerr);
                if (target == "agent-sim") return cli::cmdAgentSim(args, std::cout, std::cerr);
            }
            cli::printUsage(std::cout);
            return cli::kExitOk;
        }
        if (command == "version" || command == "--version") {
            std::cout << "eggembt " << EGGEMBT_VERSION << "\n";
            return cli::kExitOk;
        }
        // GLM-5.3-Flash: the tool's credits, exactly as Egge asked for them
        // (the indented lines start with four spaces, byte for byte, and each
        // artificial author is credited on a line of its own).
        if (command == "credits" || command == "--credits") {
            std::cout << "Egge's Model-Based Tester\n"
                         "    Designer: Esa Kettunen\n"
                         "    Copyright (c) Esa Kettunen 2026\n"
                         "    Software written by artificial intelligence: Claude Sonnet 5.5 Medium\n"
                         "    Software written by artificial intelligence: DeepSeek V4.1 Flash\n"
                         // DeepSeek V4.1 Flash: Egge's batch-20 credit line.
                         "    Software written by artificial intelligence: DeepSeek V4.1 Flash\n";
            return cli::kExitOk;
        }
        if (command == "check") return cli::cmdCheck(args, std::cout, std::cerr);
        if (command == "explore") return cli::cmdExplore(args, std::cout, std::cerr);
        if (command == "run") return cli::cmdRun(args, std::cout, std::cerr);
        if (command == "report") return cli::cmdReport(args, std::cout, std::cerr);
        if (command == "compare") return cli::cmdCompare(args, std::cout, std::cerr);
        if (command == "agent-sim") return cli::cmdAgentSim(args, std::cout, std::cerr);
    } catch (const std::exception &e) {
        std::cerr << "eggembt: internal error: " << e.what() << "\n";
        return cli::kExitInfra;
    }
    std::cerr << "eggembt: unknown command \"" << command << "\"\n\n";
    cli::printUsage(std::cerr);
    return cli::kExitUsage;
}

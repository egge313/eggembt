// The commands of the `eggembt` tool. Each takes its arguments (without the command
// name) and the streams to write to, and returns the process exit code.
#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace cli {

constexpr int kExitOk = 0;
constexpr int kExitFailures = 1;    // the SUT misbehaved: deviation, deadlock or livelock found
constexpr int kExitUsage = 2;       // bad command line, bad model, bad log
constexpr int kExitInfra = 3;       // the agent or the connection did not work
constexpr int kExitIncomplete = 4;  // --require-complete and coverage is not complete

void printUsage(std::ostream &out);

int cmdCheck(const std::vector<std::string> &args, std::ostream &out, std::ostream &err);
int cmdExplore(const std::vector<std::string> &args, std::ostream &out, std::ostream &err);
int cmdRun(const std::vector<std::string> &args, std::ostream &out, std::ostream &err);
int cmdReport(const std::vector<std::string> &args, std::ostream &out, std::ostream &err);
int cmdAgentSim(const std::vector<std::string> &args, std::ostream &out, std::ostream &err);

// GLM-5.3-Flash: mutual overlap of the coverage of two run logs (campaigns),
// e.g. recorded with the same seed and with a different one.
int cmdCompare(const std::vector<std::string> &args, std::ostream &out, std::ostream &err);

}  // namespace cli

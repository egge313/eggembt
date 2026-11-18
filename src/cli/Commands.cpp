#include "cli/Commands.h"

#include "cli/Args.h"
#include "cli/Report.h"
#include "engine/AgentServer.h"
#include "engine/OverlapAnalysis.h"
#include "engine/Session.h"
#include "engine/SimulatedSut.h"
#include "io/JsonNetLoader.h"
#include "io/PnmlLoader.h"  // GLM-5.3-Flash
#include "io/RunLogIO.h"
#include "io/Transports.h"
#include "model/Fingerprint.h"

#include <atomic>
#include <chrono>
#include <cctype>  // GLM-5.3-Flash: PNML sniffing
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <thread>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace cli {

using namespace pn;

namespace {

volatile std::sig_atomic_t g_interrupted = 0;
void onSignal(int) { g_interrupted = 1; }

bool colorWanted(const Args &args)
{
    if (args.has("no-color") || std::getenv("NO_COLOR"))
        return false;
#if defined(__unix__) || defined(__APPLE__)
    return ::isatty(STDOUT_FILENO) != 0;
#else
    return false;
#endif
}

Console makeConsole(const Args &args, std::ostream &out, std::ostream &err)
{
    Console con{out, err};
    con.color = colorWanted(args);
    con.quiet = args.has("quiet");
    return con;
}

int usageError(const Console &con, const std::string &message, const char *command)
{
    con.err << "mbt " << command << ": " << message << "\n"
            << "Try: mbt " << command << " --help\n";
    return kExitUsage;
}

// Loads and validates a model; prints the findings. Null if it cannot be used.
// GLM-5.3-Flash: PNML import (doku section 12, item 6). The format is
// recognised by the first non-space byte: '<' means XML, i.e. PNML; anything
// else (and an unreadable file) goes to the JSON loader, whose error
// reporting stays the fallback.
std::optional<Net> loadModel(const std::string &path, const Console &con, bool strict)
{
    char first = 0;
    {
        std::ifstream in(path);
        if (in) {
            while (in.get(first) && std::isspace(static_cast<unsigned char>(first))) {
            }
        }
    }
    const LoadResult r = first == '<' ? PnmlLoader::loadFile(path) : JsonNetLoader::loadFile(path);
    for (const LoadIssue &i : r.issues) {
        const bool isError = i.severity == LoadIssue::Severity::Error;
        const std::string where = i.path == path ? path : path + ": " + i.path;
        con.err << (isError ? con.red("error") : con.yellow("warning")) << " " << where << ": " << i.message << "\n";
    }
    if (!r.ok()) {
        con.err << "The model has errors.\n";
        return std::nullopt;
    }
    if (strict && r.hasWarnings()) {
        con.err << "The model has warnings and --strict was given.\n";
        return std::nullopt;
    }
    return *r.net;
}

bool parsePolicy(const std::string &s, TargetPolicy &out)
{
    if (s == "until_covered") { out = TargetPolicy::UntilCovered; return true; }
    if (s == "once")          { out = TargetPolicy::Once;         return true; }
    if (s == "avoid")         { out = TargetPolicy::Avoid;        return true; }
    return false;
}

bool parseDump(const std::string &s, DumpMode &out)
{
    return parseDumpMode(s, out);
}

std::string kindCounts(const Net &net)
{
    int inputs = 0, outputs = 0, traced = 0, silent = 0;
    for (const Transition &t : net.transitions) {
        switch (t.kind) {
        case TransitionKind::Controllable: ++inputs; break;
        case TransitionKind::Observable:   ++outputs; break;
        case TransitionKind::Internal:     (t.traced ? traced : silent)++; break;  // GLM-5.3-Flash
        }
    }
    return std::to_string(inputs) + " inputs, " + std::to_string(outputs) + " outputs, " + std::to_string(traced) +
           " traced and " + std::to_string(silent) + " silent internal";
}

std::string policyName(TargetPolicy p)
{
    return p == TargetPolicy::Once ? "once" : p == TargetPolicy::Avoid ? "avoid" : "until_covered";
}

// GLM-5.3-Flash: mean over all cells of a (non-square) overlap matrix.
double meanOfMatrix(const std::vector<double> &m)
{
    if (m.empty())
        return 0.0;
    double sum = 0.0;
    for (double v : m)
        sum += v;
    return sum / static_cast<double>(m.size());
}

// --- options shared by several commands -----------------------------------------------------

void addCommonSpec(ArgSpec &spec)
{
    spec.flags.insert({"help", "no-color", "quiet"});
}

void addSimSpec(ArgSpec &spec)
{
    spec.valued.insert({"sim-seed", "sim-weight", "sim-ack-delay", "sim-step-delay", "sim-deviate-after",
                        "sim-deviation-label", "sim-crash-after"});  // GLM-5.3-Flash
    spec.flags.insert({"sim-no-snapshot", "sim-no-crash"});
}

bool buildSimOptions(const Args &args, SimulatedSutOptions &o, const Net &net, std::string *error)
{
    long long v = 0;
    if (args.given("sim-seed")) {
        v = 1;
        if (!args.getInt("sim-seed", v, 0, 1LL << 62, error))
            return false;
        o.seed = static_cast<std::uint64_t>(v);
    }
    v = o.ackDelayMs;
    if (!args.getInt("sim-ack-delay", v, 0, 3600000, error))
        return false;
    o.ackDelayMs = static_cast<int>(v);
    v = o.stepDelayMs;
    if (!args.getInt("sim-step-delay", v, 0, 3600000, error))
        return false;
    o.stepDelayMs = static_cast<int>(v);
    v = o.deviateAfterEvents;
    if (!args.getInt("sim-deviate-after", v, 0, 1000000000, error))
        return false;
    o.deviateAfterEvents = static_cast<int>(v);
    o.deviationLabel = args.get("sim-deviation-label", "mbt.simulated.deviation");
    o.snapshot = !args.has("sim-no-snapshot");
    o.crash = !args.has("sim-no-crash");
    for (const std::string &w : args.all("sim-weight")) {
        const std::size_t eq = w.find('=');
        char *end = nullptr;
        const double value = eq == std::string::npos ? 0 : std::strtod(w.c_str() + eq + 1, &end);
        if (eq == std::string::npos || eq == 0 || end == w.c_str() + eq + 1 || *end != '\0' || value < 0) {
            *error = "--sim-weight needs TRANSITION=NUMBER, not \"" + w + "\"";
            return false;
        }
        const std::string id = w.substr(0, eq);
        if (!net.findTransition(id)) {
            *error = "--sim-weight: the model has no transition \"" + id + "\"";
            return false;
        }
        o.weight[id] = value;
    }
    // GLM-5.3-Flash: --sim-crash-after N ends the simulated SUT once N events
    // were pushed, wherever it is (0 = never): a death nobody predicted, the
    // seed of an implicit crash entry.
    v = 0;
    if (!args.getInt("sim-crash-after", v, 0, 1000000000, error))
        return false;
    o.crashAfterEvents = v > 0 ? static_cast<int>(v) : -1;
    // GLM-5.3-Flash: seed the predicted crashes the model declares on
    // CONTROLLABLE transitions: handling that stimulus kills the SUT with the
    // prediction's prior mean (the class word's, or the explicit "p"). The
    // credibility experiment then has something to observe, seeded
    // reproducibly by --sim-seed. Marking predictions and the SUT's own moves
    // cannot be seeded this way: the simulator only dies at a stimulus it
    // handles.
    for (const Transition &t : net.transitions) {
        if (t.kind != TransitionKind::Controllable || !t.crashPrediction || t.label.empty())
            continue;
        CrashCalibration cal = classCalibration(t.crashPrediction->crashClass);
        if (t.crashPrediction->p)
            cal.priorP = *t.crashPrediction->p;
        o.crashAt[t.label] = cal.priorP;
    }
    return true;
}

bool splitHostPort(const std::string &text, std::string &host, int &port, bool hostOptional)
{
    const std::size_t colon = text.rfind(':');
    const std::string portText = colon == std::string::npos ? text : text.substr(colon + 1);
    host = colon == std::string::npos ? (hostOptional ? "127.0.0.1" : "") : text.substr(0, colon);
    char *end = nullptr;
    const long p = std::strtol(portText.c_str(), &end, 10);
    if (portText.empty() || *end != '\0' || p < 0 || p > 65535 || (host.empty() && !hostOptional))
        return false;
    port = static_cast<int>(p);
    return true;
}

const char *kSimHelp =
    "  Simulated SUT options:\n"
    "    --sim-seed N              seed of the SUT's random choices (default 1)\n"
    "    --sim-weight ID=W         weight of the SUT's own transition ID (0 = never); repeatable\n"
    "    --sim-ack-delay MS        inputs take effect this long after they were sent\n"
    "    --sim-step-delay MS       pause between the SUT's own steps (default 1)\n"
    "    --sim-deviate-after N     after N events emit an event the model does not know\n"
    "    --sim-deviation-label L   label of that event\n"
    "    --sim-crash-after N       the simulated SUT dies after N events, wherever it is\n"
    "                              (0 = never); predicted stimuli crash it as the model says\n"
    "    --sim-no-snapshot         the simulated agent cannot take snapshots\n"
    "    --sim-no-crash            ... nor crash dumps\n";

}  // namespace

// ===========================================================================
// help
// ===========================================================================

void printUsage(std::ostream &out)
{
    out << "mbt " << MBT_VERSION << " - model-based testing with Petri-net specifications\n\n"
        << "Usage: mbt <command> [options]\n\n"
        << "Commands:\n"
        << "  check MODEL      validate a model and show what it describes\n"
        << "  explore MODEL    explore the reachability graph: deadlocks, livelocks, Graphviz output\n"
        << "  run MODEL        test a system (an agent, or the built-in simulator) against the model\n"
        << "  report MODEL     replay a run log and show the coverage reached so far\n"
        << "  compare MODEL    mutual overlap of two run logs' coverage (Jaccard, CSV for plots)\n"
        << "  agent-sim MODEL  serve a simulated SUT over standard input/output (an agent for 'run')\n"
        << "  version          print the version\n"
        << "  credits          print the credits\n"  // GLM-5.3-Flash
        << "  help [COMMAND]   this text, or help for one command\n\n"
        << "Exit codes: 0 ok, 1 faults found (deviation, deadlock, livelock), 2 usage or model error,\n"
        << "            3 infrastructure problems (agent, connection), 4 coverage incomplete (--require-complete).\n";
}

static void printCommandHelp(std::ostream &out, const std::string &command)
{
    if (command == "check") {
        out << "mbt check MODEL [--strict] [--max-states N] [--no-explore] [--no-color]\n\n"
            << "Validates the model file (all problems are reported at once, with JSON paths), summarises\n"
            // GLM-5.3-Flash: PNML files are accepted, too (doku section 12, item 6).
            << "it, and explores the reachability graph (up to --max-states, default 100000) to report\n"
            << "deadlocks and livelocks of the specification. The model is JSON, or PNML when the file\n"
            << "starts with '<'.\n\n"
            << "  --strict       treat warnings as errors (exit 2)\n"
            << "  --no-explore   only validate\n";
    } else if (command == "explore") {
        out << "mbt explore MODEL [--max-states N] [--list N] [--internal-cycles] [--dot FILE] [--no-color]\n\n"
            << "Explores the reachability graph and reports its size, deadlocks, terminal cycles (livelocks)\n"
            << "and error markings.\n\n"
            << "  --max-states N      stop creating states beyond N (default 1000000)\n"
            << "  --list N            show at most N states per kind of finding (default 10)\n"
            << "  --internal-cycles   also report cycles of internal transitions only (divergence)\n"
            << "  --dot FILE          write the graph for Graphviz (at most 5000 states)\n";
    } else if (command == "run") {
        out << "mbt run MODEL (--simulate | --agent \"CMD ARGS\" | --connect HOST:PORT | --listen [ADDR:]PORT | --stdio) [options]\n\n"
            << "Runs test runs against a system until everything explored is covered or a limit is hit.\n\n"
            << "  Where the SUT is:\n"
            << "    --simulate            the built-in simulated SUT (conforms to the model; try it first)\n"
            << "    --agent \"CMD ARGS\"    start an agent program; it speaks the protocol on stdin/stdout\n"
            << "    --connect HOST:PORT   connect to an agent that listens\n"
            << "    --listen [ADDR:]PORT  wait for an agent to connect (--accept-timeout SEC, default 60)\n"
            << "    --stdio               speak the protocol on this tool's stdin/stdout (messages go to stderr)\n\n"
            << "  Campaign:\n"
            << "    --max-runs N          default 20          --max-events N        per run, default 10000\n"
            << "    --quiescence MS       silence before the agent reports it (default 500)\n"
            << "    --livelock-timeout MS run-time livelock detection, 0 = off (default 0)\n"
            << "    --livelock-min-events N  default 3\n"
            << "    --idle-timeout MS     abort a run if the SUT is silent while it owes output (default 10000)\n"
            << "    --ack-timeout MS      abort if an input is never reported back (default 5000)\n"
            << "    --poll MS             how long to wait for events per loop (default 20)\n"
            << "    --threshold N         visits after which a state/edge counts as covered (default 1)\n"
            << "    --initial NAME        start only from this initial marking; repeatable\n"
            // GLM-5.3-Flash: seed for the suggester's tie-breaks.
            << "    --seed N              seed for random tie-breaks in the input choice; 0 keeps the\n"
            << "                          deterministic order, the same seed reproduces a campaign\n"
            << "    --no-ants             no ant-colony guidance          --ant-seed N   seed of the ants\n"
            // GLM-5.3-Flash: switch the time-priced catastrophe pheromones off.
            << "    --flat-catastrophe    catastrophe pheromones stay flat instead of growing with the\n"
            << "                          way into the error state (the wall-clock pricing)\n"
            // GLM-5.3-Flash: the T-list layer (doku section 16).
            << "    --tlist N             make the (marking, T-list) pairs a finer pheromone basis\n"
            << "                          beside the markings: pheromones also remember the recent\n"
            << "                          trace history they were learned under; N is the list\n"
            << "                          length, 0..8 (default 0 = off)\n"
            // GLM-5.3-Flash: the carry-over factor of the inherited pheromones.
            << "    --restored-pheromones N\n"
            << "                          when continuing from a run log, start with N percent of\n"
            << "                          the earlier rounds' pheromone strength (default 50;\n"
            << "                          100 = full strength, 0 = ignore their teaching)\n"
            << "    --max-states N        limit of the reachability graph (default 1000000)\n"
            << "    --no-analyze          skip the static analysis of the model\n"
            << "    --deadlock-policy P   until_covered | once | avoid (default once)   --livelock-policy P\n"
            << "    --dump-on-deviation M   none | snapshot | crash (default none)  (also --dump-on-deadlock, --dump-on-livelock)\n"
            // GLM-5.3-Flash: the crash-prediction experiment's knobs (doku section 17).
            << "    --crash-level L       credibility level for \"certain deterministic\" crash\n"
            << "                          predictions, 0.5..1 (default 0.95; 1 = never certain)\n"
            << "    --crash-attempts N    give-up budget for crashes nobody predicted (default 6);\n"
            << "                          predictions bring their class's own budget unless they say \"attempts\"\n"
            // GLM-5.3-Flash: the command hooks' bound (doku section 18).
            << "    --hook-timeout S      bound for a synchronous command hook in seconds (default 60);\n"
            << "                          a child still running past it is killed and counts as failed\n\n"
            << "  Output:\n"
            << "    --log FILE            append every run to FILE; if it exists, continue from it\n"
            << "    --json FILE           write a machine-readable report\n"
            << "    --require-complete    exit 4 if coverage is not complete\n"
            << "    --quiet               only the final report      --no-color\n\n"
            << kSimHelp;
    } else if (command == "report") {
        out << "mbt report MODEL --log FILE [--json FILE] [--no-color]\n\n"
            << "Replays the run log into the model and shows the coverage reached, what is still uncovered,\n"
            << "the error markings with their budgets, and the findings of the recorded runs.\n";
    } else if (command == "compare") {
        // GLM-5.3-Flash: the overlap of two campaigns' coverage.
        out << "mbt compare MODEL LOG_A LOG_B [--csv FILE] [--json FILE] [--no-color]\n\n"
            << "Replays both run logs into the model and measures how much their coverage overlaps,\n"
            << "per element kind as the Jaccard index (1 = the same coverage, 0 = nothing in common):\n"
            << "the campaigns as a whole, the runs within each campaign, and every run of A against\n"
            << "every run of B. Use it to see whether e.g. different --seed values really test\n"
            << "differently, and how much.\n\n"
            << "  --csv FILE   every pairwise index as CSV rows (group,i,j,transitions,states,edges)\n"
            << "               for plots; the matrices in full are also in --json FILE\n";
    } else if (command == "agent-sim") {
        out << "mbt agent-sim MODEL [--connect HOST:PORT | --listen [ADDR:]PORT] [options]\n\n"
            << "A simulated SUT that speaks the plug-in protocol, so that\n"
            << "  mbt run MODEL --agent \"mbt agent-sim MODEL\"\n"
            << "tests it over the real protocol. It also serves as a reference for agent authors.\n"
            << "By default the protocol runs on standard input and output; with --connect the agent dials a\n"
            << "waiting `mbt run --listen`, with --listen it waits for `mbt run --connect` (--accept-timeout SEC,\n"
            << "default 60). Logs go to standard error.\n\n"
            << kSimHelp;
    } else {
        printUsage(out);
    }
}

// ===========================================================================
// check
// ===========================================================================

int cmdCheck(const std::vector<std::string> &argv, std::ostream &out, std::ostream &err)
{
    ArgSpec spec;
    addCommonSpec(spec);
    spec.flags.insert({"strict", "no-explore"});
    spec.valued.insert("max-states");
    Args args;
    std::string error;
    Console plain{out, err};
    if (!parseArgs(argv, spec, args, &error))
        return usageError(plain, error, "check");
    const Console con = makeConsole(args, out, err);
    if (args.has("help")) {
        printCommandHelp(out, "check");
        return kExitOk;
    }
    if (args.positional.size() != 1)
        return usageError(con, "expected exactly one model file", "check");
    long long maxStates = 100000;
    if (!args.getInt("max-states", maxStates, 1, 100000000, &error))
        return usageError(con, error, "check");

    const std::optional<Net> net = loadModel(args.positional[0], con, args.has("strict"));
    if (!net)
        return kExitUsage;

    out << con.bold("Model") << "  " << (net->name.empty() ? args.positional[0] : net->name) << "\n";
    out << "  " << net->places.size() << " places, " << net->transitions.size() << " transitions (" << kindCounts(*net)
        << ")\n";
    const auto names = [](const std::vector<NamedMarking> &v) {
        std::string s;
        for (const NamedMarking &m : v)
            s += (s.empty() ? "" : ", ") + m.name;
        return s.empty() ? std::string("none") : s;
    };
    out << "  initial markings: " << names(net->initialMarkings) << "\n";
    out << "  target markings:  " << names(net->targetMarkings) << "\n";
    out << "  error markings:   " << names(net->errorMarkings) << "\n";
    for (const NamedMarking &m : net->errorMarkings) {
        out << "    " << m.name << ": " << policyName(m.policy);
        if (m.policy == TargetPolicy::UntilCovered && m.targetHits)
            out << ", " << *m.targetHits << " hits";
        if (m.crashDump != DumpMode::None)
            out << ", post-mortem " << toString(m.crashDump) << " (at most " << m.maxDumps << ")";
        out << "\n";
    }

    if (!args.has("no-explore")) {
        SessionConfig cfg;
        cfg.graph.maxStates = static_cast<std::size_t>(maxStates);
        Session session(*net, cfg);
        const LivelockReport &report = session.analyzeModel();
        const ReachabilityGraph &g = session.graph();
        out << "\n" << con.bold("Reachability graph") << "\n";
        out << "  " << g.numStates() << " states, " << g.numEdges() << " edges";
        out << (report.graphComplete ? " (explored completely)" : con.yellow(" (NOT explored completely: limit or bounds)"))
            << "\n";
        if (!g.boundViolations().empty())
            out << "  " << con.yellow(std::to_string(g.boundViolations().size()) + " firing(s) would exceed a place bound and were not followed") << "\n";
        const std::size_t deadlocks = g.deadlocks().size();
        out << "  deadlocks: " << (deadlocks ? con.red(std::to_string(deadlocks)) : std::string("0")) << "\n";
        std::size_t cycles = 0;
        for (const LivelockFinding &f : report.findings)
            if (f.kind == LivelockKind::TerminalCycle)
                ++cycles;
        out << "  terminal cycles without a target marking (livelocks): " << (cycles ? con.red(std::to_string(cycles)) : std::string("0")) << "\n";
        for (const std::string &w : session.warnings())
            out << con.yellow("warning") << " " << w << "\n";
    }
    return kExitOk;
}

// ===========================================================================
// explore
// ===========================================================================

static std::string dotEscape(const std::string &s)
{
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out;
}

static bool writeDot(const std::string &path, const Net &net, const ReachabilityGraph &g, std::string *error)
{
    if (g.numStates() > 5000) {
        *error = "the graph has " + std::to_string(g.numStates()) + " states; --dot writes at most 5000 (lower --max-states)";
        return false;
    }
    std::ofstream f(path);
    if (!f) {
        *error = "cannot write " + path;
        return false;
    }
    f << "digraph \"" << dotEscape(net.name.empty() ? "model" : net.name) << "\" {\n"
      << "  rankdir=LR;\n  node [shape=box, fontname=monospace, fontsize=10, style=filled, fillcolor=white];\n"
      << "  edge [fontname=monospace, fontsize=9];\n";
    for (StateId s = 0; s < g.numStates(); ++s) {
        std::string attrs;
        if (g.kind(s) == StateKind::Target)
            attrs = ", fillcolor=\"#d5e8d4\"";
        else if (g.kind(s) == StateKind::Error)
            attrs = ", fillcolor=\"#f8cecc\"";
        else if (g.isDeadlock(s))
            attrs = ", fillcolor=\"#ffe6cc\"";
        else if (!g.isExpanded(s))
            attrs = ", style=\"filled,dashed\"";
        for (StateId i : g.initialStates())
            if (i == s)
                attrs += ", peripheries=2";
        f << "  s" << s << " [label=\"" << dotEscape(formatMarking(net, g.marking(s))) << "\"" << attrs << "];\n";
    }
    for (StateId s = 0; s < g.numStates(); ++s) {
        if (!g.isExpanded(s))
            continue;
        for (const Edge &e : g.successors(s)) {
            const Transition &t = net.transitions[e.transition];
            const char *style = t.kind == TransitionKind::Controllable ? "solid" : t.kind == TransitionKind::Observable ? "dashed" : "dotted";
            f << "  s" << s << " -> s" << e.target << " [label=\"" << dotEscape(t.id) << "\", style=" << style << "];\n";
        }
    }
    f << "}\n";
    return static_cast<bool>(f);
}

int cmdExplore(const std::vector<std::string> &argv, std::ostream &out, std::ostream &err)
{
    ArgSpec spec;
    addCommonSpec(spec);
    spec.flags.insert("internal-cycles");
    spec.valued.insert({"max-states", "list", "dot"});
    Args args;
    std::string error;
    Console plain{out, err};
    if (!parseArgs(argv, spec, args, &error))
        return usageError(plain, error, "explore");
    const Console con = makeConsole(args, out, err);
    if (args.has("help")) {
        printCommandHelp(out, "explore");
        return kExitOk;
    }
    if (args.positional.size() != 1)
        return usageError(con, "expected exactly one model file", "explore");
    long long maxStates = 1000000, list = 10;
    if (!args.getInt("max-states", maxStates, 1, 100000000, &error) || !args.getInt("list", list, 0, 1000000, &error))
        return usageError(con, error, "explore");

    const std::optional<Net> net = loadModel(args.positional[0], con, false);
    if (!net)
        return kExitUsage;

    SessionConfig cfg;
    cfg.graph.maxStates = static_cast<std::size_t>(maxStates);
    cfg.livelock.internalCycles = args.has("internal-cycles");
    Session session(*net, cfg);
    const LivelockReport &report = session.analyzeModel();
    const ReachabilityGraph &g = session.graph();

    out << con.bold("Reachability graph") << "\n";
    out << "  " << g.numStates() << " states, " << g.numEdges() << " edges, " << g.numExpanded() << " expanded";
    out << (report.graphComplete ? "" : con.yellow("  (incomplete: state limit or place bounds)")) << "\n";
    out << "  " << g.statesOfKind(StateKind::Target).size() << " target, " << g.statesOfKind(StateKind::Error).size()
        << " error states; " << report.components << " strongly connected components, " << report.cyclicComponents
        << " with a cycle\n";

    const auto showStates = [&](const std::vector<StateId> &states) {
        std::size_t shown = 0;
        for (StateId s : states) {
            if (shown++ >= static_cast<std::size_t>(list)) {
                out << "    ... and " << (states.size() - shown + 1) << " more\n";
                break;
            }
            out << "    " << formatMarking(*net, g.marking(s)) << "\n";
        }
    };

    const std::vector<StateId> dead = g.deadlocks();
    out << "\n" << con.bold("Deadlocks") << "  " << (dead.empty() ? "none" : con.red(std::to_string(dead.size()))) << "\n";
    showStates(dead);

    out << "\n" << con.bold("Livelocks") << "  ";
    if (report.findings.empty()) {
        out << "none\n";
    } else {
        out << con.red(std::to_string(report.findings.size())) << "\n";
        for (const LivelockFinding &f : report.findings) {
            out << "  " << (f.kind == LivelockKind::TerminalCycle ? "terminal cycle" : "internal cycle") << ", "
                << f.states.size() << " state(s)" << (f.certain ? "" : ", not certain: exploration was incomplete") << "\n";
            showStates(f.states);
        }
    }
    for (const std::string &w : session.warnings())
        out << "\n" << con.yellow("warning") << " " << w << "\n";

    if (args.given("dot")) {
        if (!writeDot(args.get("dot"), *net, g, &error)) {
            err << "mbt explore: " << error << "\n";
            return kExitUsage;
        }
        out << "\nGraph written to " << args.get("dot") << "\n";
    }
    return kExitOk;
}

// ===========================================================================
// report
// ===========================================================================

int cmdReport(const std::vector<std::string> &argv, std::ostream &out, std::ostream &err)
{
    ArgSpec spec;
    addCommonSpec(spec);
    spec.valued.insert({"log", "json"});
    Args args;
    std::string error;
    Console plain{out, err};
    if (!parseArgs(argv, spec, args, &error))
        return usageError(plain, error, "report");
    const Console con = makeConsole(args, out, err);
    if (args.has("help")) {
        printCommandHelp(out, "report");
        return kExitOk;
    }
    if (args.positional.size() != 1 || !args.given("log"))
        return usageError(con, "expected a model file and --log FILE", "report");

    const std::optional<Net> net = loadModel(args.positional[0], con, false);
    if (!net)
        return kExitUsage;

    const RunLogReadResult log = readRunLog(args.get("log"));
    if (!log.ok) {
        err << "mbt report: " << log.error << "\n";
        return kExitUsage;
    }
    if (log.contents.fingerprint != graphFingerprint(*net)) {
        err << "mbt report: " << args.get("log") << " was recorded for a different model (the graph changed)\n";
        return kExitUsage;
    }
    for (const std::string &w : log.contents.warnings)
        err << con.yellow("warning") << " " << args.get("log") << ": " << w << "\n";

    SessionConfig cfg;
    Session session(*net, cfg);
    std::size_t replayed = 0;
    for (std::size_t i = 0; i < log.contents.runs.size(); ++i) {
        const ReplayResult r = session.restore(log.contents.runs[i]);
        if (r.ok)
            ++replayed;
        else
            err << con.yellow("warning") << " run " << (i + 1) << " of the log no longer fits the model and was skipped: "
                << r.error << "\n";
    }
    out << con.bold("Run log") << "  " << args.get("log") << ": " << replayed << " of " << log.contents.runs.size()
        << " runs replayed\n\n";
    printCoverage(con, *net, session.coverage());
    printBacktraces(con, *net, session.graph(), session.coverage());  // GLM-5.3-Flash
    printRunsAndFindings(con, session.coverage());
    printCrashPredictions(con, session.coverage());  // GLM-5.3-Flash
    printHookRuns(con, session.coverage());  // GLM-5.3-Flash

    if (args.given("json")) {
        JsonValue j = JsonValue::object();
        j.set("model", JsonValue::string(net->name));
        j.set("runs_replayed", JsonValue::number(static_cast<double>(replayed)));
        j.set("coverage", coverageJson(*net, session.coverage()));
        std::ofstream f(args.get("json"));
        f << j.dump() << "\n";
        if (!f) {
            err << "mbt report: cannot write " << args.get("json") << "\n";
            return kExitUsage;
        }
    }
    return kExitOk;
}

// ===========================================================================
// GLM-5.3-Flash: compare - the mutual overlap of two campaigns' coverage
// ===========================================================================

int cmdCompare(const std::vector<std::string> &argv, std::ostream &out, std::ostream &err)
{
    ArgSpec spec;
    addCommonSpec(spec);
    spec.valued.insert({"csv", "json"});
    Args args;
    std::string error;
    Console plain{out, err};
    if (!parseArgs(argv, spec, args, &error))
        return usageError(plain, error, "compare");
    const Console con = makeConsole(args, out, err);
    if (args.has("help")) {
        printCommandHelp(out, "compare");
        return kExitOk;
    }
    if (args.positional.size() != 3)
        return usageError(con, "expected a model file and two run logs", "compare");

    const std::optional<Net> net = loadModel(args.positional[0], con, false);
    if (!net)
        return kExitUsage;
    const std::string fingerprint = graphFingerprint(*net);

    // Replays one log into its own session; the per-run coverage attribution
    // of a replayed run is exactly the one the live run recorded.
    const auto replayLog = [&](const std::string &path, Session &session) -> bool {
        const RunLogReadResult log = readRunLog(path);
        if (!log.ok) {
            err << "mbt compare: " << log.error << "\n";
            return false;
        }
        if (log.contents.fingerprint != fingerprint) {
            err << "mbt compare: " << path << " was recorded for a different model (the graph changed)\n";
            return false;
        }
        for (const std::string &w : log.contents.warnings)
            err << con.yellow("warning") << " " << path << ": " << w << "\n";
        std::size_t replayed = 0;
        for (std::size_t i = 0; i < log.contents.runs.size(); ++i) {
            const ReplayResult r = session.restore(log.contents.runs[i]);
            if (r.ok)
                ++replayed;
            else
                err << con.yellow("warning") << " run " << (i + 1) << " of " << path
                    << " no longer fits the model and was skipped: " << r.error << "\n";
        }
        out << con.bold("Run log") << "  " << path << ": " << replayed << " of " << log.contents.runs.size()
            << " runs replayed\n";
        return true;
    };

    SessionConfig cfg;
    Session a(*net, cfg);
    Session b(*net, cfg);
    if (!replayLog(args.positional[1], a) || !replayLog(args.positional[2], b))
        return kExitUsage;

    const CampaignCoverage ca = campaignCoverage(a.coverage());
    const CampaignCoverage cb = campaignCoverage(b.coverage());
    const double campaignTr = jaccardIndex(ca.transitions, cb.transitions);
    const double campaignSt = jaccardIndex(ca.states, cb.states);
    const double campaignEd = jaccardIndex(ca.edges, cb.edges);

    const OverlapMatrix ma = runOverlapMatrix(a.coverage());
    const OverlapMatrix mb = runOverlapMatrix(b.coverage());

    const std::size_t na = a.coverage().numRuns();
    const std::size_t nb = b.coverage().numRuns();
    std::vector<RunCoverage> ra, rb;
    for (RunId i = 0; i < na; ++i)
        ra.push_back(runCoverage(a.coverage(), i));
    for (RunId i = 0; i < nb; ++i)
        rb.push_back(runCoverage(b.coverage(), i));
    OverlapMatrix cross;
    cross.n = na;
    cross.transitions.assign(na * nb, 0.0);
    cross.states.assign(na * nb, 0.0);
    cross.edges.assign(na * nb, 0.0);
    for (std::size_t i = 0; i < na; ++i)
        for (std::size_t j = 0; j < nb; ++j) {
            cross.transitions[i * nb + j] = jaccardIndex(ra[i].transitions, rb[j].transitions);
            cross.states[i * nb + j] = jaccardIndex(ra[i].states, rb[j].states);
            cross.edges[i * nb + j] = jaccardIndex(ra[i].edges, rb[j].edges);
        }

    const auto pct = [](double v) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%.0f%%", v * 100.0);
        return std::string(buf);
    };

    out << "\n" << con.bold("Overlap") << "  (Jaccard index, 100% = the same coverage)\n";
    out << "  campaigns as a whole: transitions " << pct(campaignTr) << ", states " << pct(campaignSt) << ", edges "
        << pct(campaignEd) << "\n";
    if (ma.n >= 2)
        out << "  within " << args.positional[1] << ": transitions " << pct(ma.meanPairwise(ma.transitions))
            << ", states " << pct(ma.meanPairwise(ma.states)) << ", edges " << pct(ma.meanPairwise(ma.edges)) << "\n";
    if (mb.n >= 2)
        out << "  within " << args.positional[2] << ": transitions " << pct(mb.meanPairwise(mb.transitions))
            << ", states " << pct(mb.meanPairwise(mb.states)) << ", edges " << pct(mb.meanPairwise(mb.edges)) << "\n";
    if (na >= 1 && nb >= 1)
        out << "  run against run (" << na << " x " << nb << " pairs), cross means: transitions "
            << pct(meanOfMatrix(cross.transitions)) << ", states " << pct(meanOfMatrix(cross.states))
            << ", edges " << pct(meanOfMatrix(cross.edges)) << "\n";

    // The cross matrix as a small graphic, while it fits on a terminal.
    if (na > 0 && nb > 0 && na * nb <= 64) {
        out << "\n  transition overlap, " << args.positional[1] << " runs (rows) against " << args.positional[2]
            << " runs (columns), percent:\n\n";
        out << "        ";
        for (std::size_t j = 0; j < nb; ++j)
            out << padRight("r" + std::to_string(j), 5);
        out << "\n";
        for (std::size_t i = 0; i < na; ++i) {
            out << "  " << padRight("r" + std::to_string(i), 7);
            for (std::size_t j = 0; j < nb; ++j)
                out << padRight(pct(cross.transitions[i * nb + j]), 5);
            out << "\n";
        }
    }

    if (args.given("csv")) {
        std::ofstream f(args.get("csv"));
        f << "group,i,j,transitions,states,edges\n";
        const auto row = [&](const char *group, std::size_t i, std::size_t j, double tr, double st, double ed) {
            char trs[32], sts[32], eds[32];
            std::snprintf(trs, sizeof trs, "%.6g", tr);
            std::snprintf(sts, sizeof sts, "%.6g", st);
            std::snprintf(eds, sizeof eds, "%.6g", ed);
            f << group << ',' << i << ',' << j << ',' << trs << ',' << sts << ',' << eds << "\n";
        };
        for (std::size_t i = 0; i < ma.n; ++i)
            for (std::size_t j = i + 1; j < ma.n; ++j)
                row("within-a", i, j, ma.at(ma.transitions, i, j), ma.at(ma.states, i, j), ma.at(ma.edges, i, j));
        for (std::size_t i = 0; i < mb.n; ++i)
            for (std::size_t j = i + 1; j < mb.n; ++j)
                row("within-b", i, j, mb.at(mb.transitions, i, j), mb.at(mb.states, i, j), mb.at(mb.edges, i, j));
        for (std::size_t i = 0; i < na; ++i)
            for (std::size_t j = 0; j < nb; ++j)
                row("cross", i, j, cross.transitions[i * nb + j], cross.states[i * nb + j],
                    cross.edges[i * nb + j]);
        if (!f) {
            err << "mbt compare: cannot write " << args.get("csv") << "\n";
            return kExitUsage;
        }
        out << "  pairwise indices written to " << args.get("csv") << "\n";
    }

    if (args.given("json")) {
        JsonValue j = JsonValue::object();
        j.set("model", JsonValue::string(net->name));
        const auto side = [&](const Session &s, const CampaignCoverage &c, const OverlapMatrix &m) {
            JsonValue v = JsonValue::object();
            v.set("runs", JsonValue::number(static_cast<double>(s.coverage().numRuns())));
            JsonValue cov = JsonValue::object();
            cov.set("transitions", JsonValue::number(static_cast<double>(c.transitions.size())));
            cov.set("states", JsonValue::number(static_cast<double>(c.states.size())));
            cov.set("edges", JsonValue::number(static_cast<double>(c.edges.size())));
            v.set("covered", cov);
            v.set("overlap", overlapJson(m));
            return v;
        };
        j.set("a", side(a, ca, ma));
        j.set("b", side(b, cb, mb));
        JsonValue campaign = JsonValue::object();
        campaign.set("transitions", JsonValue::number(campaignTr));
        campaign.set("states", JsonValue::number(campaignSt));
        campaign.set("edges", JsonValue::number(campaignEd));
        j.set("campaign_overlap", campaign);
        JsonValue c = JsonValue::object();
        c.set("a_runs", JsonValue::number(static_cast<double>(na)));
        c.set("b_runs", JsonValue::number(static_cast<double>(nb)));
        const auto matrix = [&](const std::vector<double> &v) {
            JsonValue rows = JsonValue::array();
            for (std::size_t i = 0; i < na; ++i) {
                JsonValue r = JsonValue::array();
                for (std::size_t j2 = 0; j2 < nb; ++j2)
                    r.push(JsonValue::number(v[i * nb + j2]));
                rows.push(r);
            }
            return rows;
        };
        c.set("transitions", matrix(cross.transitions));
        c.set("states", matrix(cross.states));
        c.set("edges", matrix(cross.edges));
        j.set("cross", c);
        std::ofstream f(args.get("json"));
        f << j.dump() << "\n";
        if (!f) {
            err << "mbt compare: cannot write " << args.get("json") << "\n";
            return kExitUsage;
        }
    }
    return kExitOk;
}

// ===========================================================================
// agent-sim
// ===========================================================================

int cmdAgentSim(const std::vector<std::string> &argv, std::ostream &out, std::ostream &err)
{
    ArgSpec spec;
    addCommonSpec(spec);
    addSimSpec(spec);
    spec.valued.insert({"connect", "listen", "accept-timeout"});
    Args args;
    std::string error;
    Console con{out, err};
    if (!parseArgs(argv, spec, args, &error))
        return usageError(con, error, "agent-sim");
    if (args.has("help")) {
        printCommandHelp(out, "agent-sim");
        return kExitOk;
    }
    if (args.positional.size() != 1)
        return usageError(con, "expected exactly one model file", "agent-sim");

    Console quietCon{err, err};
    const std::optional<Net> net = loadModel(args.positional[0], quietCon, false);
    if (!net)
        return kExitUsage;
    SimulatedSutOptions options;
    if (!buildSimOptions(args, options, *net, &error))
        return usageError(quietCon, error, "agent-sim");

    long long acceptTimeout = 60;
    if (!args.getInt("accept-timeout", acceptTimeout, 1, 86400, &error))
        return usageError(quietCon, error, "agent-sim");

    SimulatedSut sut(*net, options);
    std::unique_ptr<ILineTransport> link;
#ifdef MBT_POSIX_IO
    if (args.given("connect") && args.given("listen"))
        return usageError(quietCon, "choose --connect or --listen, not both", "agent-sim");
    if (args.given("connect")) {
        std::string host;
        int port = 0;
        if (!splitHostPort(args.get("connect"), host, port, false))
            return usageError(quietCon, "--connect needs HOST:PORT", "agent-sim");
        link = tcpConnect(host, port, 10000, &error);
        if (!link) {
            err << "mbt agent-sim: " << error << "\n";
            return kExitInfra;
        }
    } else if (args.given("listen")) {
        std::string host;
        int port = 0;
        if (!splitHostPort(args.get("listen"), host, port, true))
            return usageError(quietCon, "--listen needs [ADDR:]PORT", "agent-sim");
        TcpListener listener;
        if (!listener.open(host, port, &error)) {
            err << "mbt agent-sim: " << error << "\n";
            return kExitInfra;
        }
        err << "mbt agent-sim: listening on " << host << ":" << listener.port() << "\n";
        link = listener.accept(static_cast<int>(acceptTimeout) * 1000, &error);
        if (!link) {
            err << "mbt agent-sim: " << error << "\n";
            return kExitInfra;
        }
    }
#else
    if (args.given("connect") || args.given("listen"))
        return usageError(quietCon, "--connect and --listen need POSIX sockets; this build only has standard input/output", "agent-sim");
#endif
    if (!link) {
#ifdef MBT_POSIX_IO
        // FdTransport, not StreamTransport: its reader polls and can be stopped
        // cleanly, so this process can still exit while the tracker keeps the
        // pipe open. A getline blocked in std::cin cannot be woken, and it holds
        // the stdio lock that exit() needs to flush all streams.
        link = std::make_unique<FdTransport>(0, 1, false);
#else
        link = std::make_unique<StreamTransport>(std::cin, out);  // the protocol owns standard output here
#endif
    }
    AgentServer server(sut, *link, "mbt-agent-sim");
    server.start();
    err << "mbt agent-sim: serving \"" << (net->name.empty() ? args.positional[0] : net->name) << "\"\n";
    while (!server.waitUntilFinished(std::chrono::milliseconds(200))) {
    }
    return kExitOk;
}

// ===========================================================================
// run
// ===========================================================================

namespace {

struct Endpoint {
    std::unique_ptr<ILineTransport> transport;
    std::unique_ptr<RemotePlugin> remote;
    std::unique_ptr<SimulatedSut> sim;
    ISutPlugin *plugin = nullptr;
    std::string description;
};

std::string outcomeWord(const Console &con, RunOutcome o)
{
    const std::string word = toString(o);
    if (isFailure(o) || o == RunOutcome::Aborted)
        return con.red(word);
    if (o == RunOutcome::ErrorReached || o == RunOutcome::SutCrashed)
        return con.yellow(word);
    return con.green(word);
}

}  // namespace

int cmdRun(const std::vector<std::string> &argv, std::ostream &stdoutStream, std::ostream &stderrStream)
{
    ArgSpec spec;
    addCommonSpec(spec);
    addSimSpec(spec);
    spec.flags.insert({"simulate", "stdio", "no-ants", "no-analyze", "require-complete", "strict",
                       "flat-catastrophe"});  // GLM-5.3-Flash
    spec.valued.insert({"agent", "connect", "listen", "accept-timeout", "hello-timeout", "max-runs", "max-events",
                        "quiescence", "livelock-timeout", "livelock-min-events", "idle-timeout", "ack-timeout", "poll",
                        "threshold", "initial", "seed", "ant-seed", "max-states", "deadlock-policy", "livelock-policy",
                        "dump-on-deviation", "dump-on-deadlock", "dump-on-livelock", "log", "json",
                        "tlist", "restored-pheromones",
                        "crash-level", "crash-attempts", "hook-timeout"});  // GLM-5.3-Flash
    Args args;
    std::string error;
    Console con = makeConsole(args, stdoutStream, stderrStream);
    if (!parseArgs(argv, spec, args, &error))
        return usageError(con, error, "run");
    if (args.has("help")) {
        printCommandHelp(stdoutStream, "run");
        return kExitOk;
    }
    if (args.positional.size() != 1)
        return usageError(con, "expected exactly one model file", "run");

    const int modes = int(args.has("simulate")) + int(args.given("agent")) + int(args.given("connect")) +
                      int(args.given("listen")) + int(args.has("stdio"));
    if (modes != 1)
        return usageError(con, "choose exactly one of --simulate, --agent, --connect, --listen, --stdio", "run");

    // With --stdio the protocol owns standard output: everything for humans goes to standard error.
    Console log = makeConsole(args, args.has("stdio") ? stderrStream : stdoutStream, stderrStream);
    if (args.has("stdio"))
        log.color = false;

    const std::optional<Net> netOpt = loadModel(args.positional[0], log, args.has("strict"));
    if (!netOpt)
        return kExitUsage;
    const Net &net = *netOpt;

    // --- configuration ---------------------------------------------------------------------
    SessionConfig cfg;
    long long maxRuns = 20, maxEvents = 10000, quiescence = 500, livelock = 0, minEvents = 3, idle = 10000,
              ack = 5000, poll = 20, threshold = 1, antSeed = 1, maxStates = 1000000, helloTimeout = 10000,
              acceptTimeout = 60, seed = 0, tlist = 0, restoredPheromones = 50, crashAttempts = 6,
              hookTimeout = 60;  // GLM-5.3-Flash: synchronous command hooks
    double crashLevel = 0.95;  // GLM-5.3-Flash
    bool ok = args.getInt("max-runs", maxRuns, 1, 100000000, &error) &&
              args.getInt("max-events", maxEvents, 1, 1000000000, &error) &&
              args.getInt("quiescence", quiescence, 1, 3600000, &error) &&
              args.getInt("livelock-timeout", livelock, 0, 86400000, &error) &&
              args.getInt("livelock-min-events", minEvents, 1, 1000000, &error) &&
              args.getInt("idle-timeout", idle, 0, 86400000, &error) &&
              args.getInt("ack-timeout", ack, 0, 86400000, &error) &&
              args.getInt("poll", poll, 1, 10000, &error) &&
              args.getInt("threshold", threshold, 1, 1000000, &error) &&
              args.getInt("ant-seed", antSeed, 0, 1LL << 62, &error) &&
              args.getInt("seed", seed, 0, 1LL << 62, &error) &&
              args.getInt("max-states", maxStates, 1, 100000000, &error) &&
              args.getInt("hello-timeout", helloTimeout, 1, 3600000, &error) &&
              args.getInt("accept-timeout", acceptTimeout, 1, 86400, &error) &&
              args.getInt("tlist", tlist, 0, 8, &error) &&  // GLM-5.3-Flash
              args.getInt("restored-pheromones", restoredPheromones, 0, 100, &error) &&  // GLM-5.3-Flash
              args.getDouble("crash-level", crashLevel, 0.5, 1.0, &error) &&  // GLM-5.3-Flash
              args.getInt("crash-attempts", crashAttempts, 1, 1000000, &error) &&  // GLM-5.3-Flash
              args.getInt("hook-timeout", hookTimeout, 1, 3600, &error);  // GLM-5.3-Flash
    if (!ok)
        return usageError(log, error, "run");
    cfg.maxRuns = static_cast<std::size_t>(maxRuns);
    cfg.maxEventsPerRun = static_cast<std::size_t>(maxEvents);
    cfg.plugin.quiescenceTimeoutMs = quiescence;
    cfg.tracker.livelockTimeoutMs = livelock;
    cfg.tracker.livelockMinEvents = static_cast<std::size_t>(minEvents);
    cfg.idleTimeout = std::chrono::milliseconds(idle);
    cfg.stimulusAckTimeout = std::chrono::milliseconds(ack);
    cfg.poll = std::chrono::milliseconds(poll);
    cfg.coverage.threshold = static_cast<std::uint32_t>(threshold);
    cfg.ants.seed = static_cast<std::uint64_t>(antSeed);
    cfg.selectionSeed = static_cast<std::uint64_t>(seed);  // GLM-5.3-Flash
    // GLM-5.3-Flash: the T-list layer (doku section 16). 0 (the default)
    // switches it off; N > 0 makes the (marking, T-list) pairs a finer
    // pheromone basis beside the edge channels.
    cfg.ants.tlistLength = static_cast<std::size_t>(tlist);
    // GLM-5.3-Flash: how much of the inherited pheromone teaching a continued
    // campaign starts with (doku section 16). 100 keeps the old full
    // strength, 0 ignores it; the merge of the earlier runs is unchanged.
    cfg.restoredPheromonePercent = static_cast<unsigned>(restoredPheromones);
    // GLM-5.3-Flash: the crash-prediction experiment's knobs (doku section 17).
    cfg.coverage.crashLevel = crashLevel;
    cfg.coverage.crashAttempts = static_cast<std::uint32_t>(crashAttempts);
    // GLM-5.3-Flash: the command hooks (doku section 18). The open run log's
    // path travels to every hook as MBT_LOG.
    cfg.hookTimeoutSeconds = static_cast<std::uint32_t>(hookTimeout);
    cfg.logPath = args.get("log");
    cfg.graph.maxStates = static_cast<std::size_t>(maxStates);
    cfg.useAnts = !args.has("no-ants");
    cfg.analyzeModel = !args.has("no-analyze");
    // GLM-5.3-Flash: --flat-catastrophe switches the time pricing of the
    // catastrophe pheromones off (deposits stay at the base level).
    cfg.catastropheTimePricing = !args.has("flat-catastrophe");

    if (args.given("deadlock-policy") && !parsePolicy(args.get("deadlock-policy"), cfg.targets.deadlock))
        return usageError(log, "--deadlock-policy must be until_covered, once or avoid", "run");
    if (args.given("livelock-policy") && !parsePolicy(args.get("livelock-policy"), cfg.targets.livelock))
        return usageError(log, "--livelock-policy must be until_covered, once or avoid", "run");
    const std::pair<const char *, DumpMode *> dumpOptions[] = {{"dump-on-deviation", &cfg.dumps.onDeviation},
                                                                {"dump-on-deadlock", &cfg.dumps.onDeadlock},
                                                                {"dump-on-livelock", &cfg.dumps.onLivelock}};
    for (const auto &d : dumpOptions)
        if (args.given(d.first) && !parseDump(args.get(d.first), *d.second))
            return usageError(log, std::string("--") + d.first + " must be none, snapshot or crash", "run");
    for (const std::string &name : args.all("initial")) {
        std::size_t index = net.initialMarkings.size();
        for (std::size_t i = 0; i < net.initialMarkings.size(); ++i)
            if (net.initialMarkings[i].name == name)
                index = i;
        if (index == net.initialMarkings.size())
            return usageError(log, "the model has no initial marking \"" + name + "\"", "run");
        cfg.initialMarkings.push_back(index);
    }

    // --- run log ------------------------------------------------------------------------------
    RunLogWriter writer;
    std::vector<PersistedRun> restored;
    const std::string logPath = args.get("log");
    if (!logPath.empty()) {
        const std::string fingerprint = graphFingerprint(net);
        std::error_code ec;
        if (std::filesystem::exists(logPath, ec) && std::filesystem::file_size(logPath, ec) > 0) {
            const RunLogReadResult r = readRunLog(logPath);
            if (!r.ok) {
                log.err << "mbt run: " << r.error << "\n";
                return kExitUsage;
            }
            if (r.contents.fingerprint != fingerprint) {
                log.err << "mbt run: " << logPath << " belongs to a different model; use another file\n";
                return kExitUsage;
            }
            for (const std::string &w : r.contents.warnings)
                log.err << log.yellow("warning") << " " << logPath << ": " << w << "\n";
            restored = r.contents.runs;
        }
        if (!writer.open(logPath, fingerprint, &error)) {
            log.err << "mbt run: " << error << "\n";
            return kExitUsage;
        }
        cfg.runSink = [&writer, &log](const PersistedRun &run) {
            std::string e;
            if (!writer.append(run, &e))
                log.err << log.yellow("warning") << " could not append to the run log: " << e << "\n";
        };
    }

    // --- progress -------------------------------------------------------------------------------
    cfg.progress = [&log](const RunReport &r) {
        if (log.quiet)
            return;
        std::string line = "  run " + padRight(std::to_string(r.id + 1), 4) + " " + padRight("", 0);
        log.out << line << outcomeWord(log, r.outcome) << std::string(r.outcome == RunOutcome::Completed ? 5 : 0, ' ');
        log.out << "  " << r.events << " events, " << r.stimuli << " inputs";
        if (r.newStates || r.newEdges)
            log.out << ", +" << r.newStates << " states +" << r.newEdges << " edges";
        if (r.dumps)
            log.out << ", " << r.dumps << " post-mortem(s)";
        if (!r.detail.empty())
            log.out << "  " << log.dim(truncate(r.detail, 90));
        log.out << "\n";
        for (const std::string &n : r.notes)
            log.out << "         " << log.yellow("note") << " " << n << "\n";
    };

    Session session(net, cfg);
    for (std::size_t i = 0; i < restored.size(); ++i) {
        const ReplayResult r = session.restore(restored[i]);
        if (!r.ok)
            log.err << log.yellow("warning") << " run " << (i + 1) << " of the log no longer fits the model and was skipped: "
                    << r.error << "\n";
    }
    if (!restored.empty() && !log.quiet)
        log.out << "Continuing from " << logPath << ": " << session.coverage().numRuns() << " earlier run(s) restored.\n";

    // --- the SUT ----------------------------------------------------------------------------------
    Endpoint ep;
    if (args.has("simulate")) {
        SimulatedSutOptions so;
        if (!buildSimOptions(args, so, net, &error))
            return usageError(log, error, "run");
        ep.sim = std::make_unique<SimulatedSut>(net, so);
        ep.plugin = ep.sim.get();
        ep.description = "the built-in simulated SUT";
    } else {
#ifdef MBT_POSIX_IO
        if (args.given("agent")) {
            ep.transport = ProcessTransport::spawn(splitCommandLine(args.get("agent")), &error);
            ep.description = "agent \"" + args.get("agent") + "\"";
        } else if (args.given("connect")) {
            std::string host;
            int port = 0;
            if (!splitHostPort(args.get("connect"), host, port, false))
                return usageError(log, "--connect needs HOST:PORT", "run");
            ep.transport = tcpConnect(host, port, 10000, &error);
            ep.description = "agent at " + args.get("connect");
        } else if (args.given("listen")) {
            std::string host;
            int port = 0;
            if (!splitHostPort(args.get("listen"), host, port, true))
                return usageError(log, "--listen needs [ADDR:]PORT", "run");
            TcpListener listener;
            if (!listener.open(host, port, &error)) {
                log.err << "mbt run: " << error << "\n";
                return kExitInfra;
            }
            log.out << "Waiting for an agent on " << host << ":" << listener.port() << " ...\n" << std::flush;
            ep.transport = listener.accept(static_cast<int>(acceptTimeout) * 1000, &error);
            ep.description = "agent connected on port " + std::to_string(listener.port());
        } else
#endif
        if (args.has("stdio")) {
#ifdef MBT_POSIX_IO
            // FdTransport, not StreamTransport, so the process can exit while
            // the agent keeps the pipe open; see cmdAgentSim.
            ep.transport = std::make_unique<FdTransport>(0, 1, false);
#else
            ep.transport = std::make_unique<StreamTransport>(std::cin, std::cout);
#endif
            ep.description = "agent on standard input/output";
        } else {
            return usageError(log, "this option needs POSIX sockets and processes, which this build does not have; use --stdio or --simulate", "run");
        }
        if (!ep.transport) {
            log.err << "mbt run: " << error << "\n";
            return kExitInfra;
        }
        ep.remote = std::make_unique<RemotePlugin>(*ep.transport);
        if (!ep.remote->waitForHello(std::chrono::milliseconds(helloTimeout))) {
            log.err << "mbt run: the agent did not say hello within " << helloTimeout << " ms\n";
            return kExitInfra;
        }
        const PluginCapabilities caps = ep.remote->capabilities();
        ep.description += " (" + ep.remote->agentName() + "; snapshot " + (caps.snapshot ? "yes" : "no") + ", crash " +
                          (caps.crash ? "yes" : "no") + ")";
        ep.plugin = ep.remote.get();
    }

    if (!log.quiet)
        log.out << log.bold("Testing ") << (net.name.empty() ? args.positional[0] : net.name) << " against " << ep.description << "\n";

    // --- Ctrl-C ends the campaign cleanly ------------------------------------------------------
    g_interrupted = 0;
    const auto oldHandler = std::signal(SIGINT, onSignal);
    std::atomic<bool> finished{false};
    std::thread watcher([&] {
        while (!finished) {
            if (g_interrupted) {
                session.requestStop();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });

    const CampaignReport report = session.runCampaign(*ep.plugin);

    finished = true;
    watcher.join();
    std::signal(SIGINT, oldHandler);
    if (ep.remote)
        ep.remote->shutdown();

    // --- the report -----------------------------------------------------------------------------
    for (const std::string &w : report.warnings)
        log.out << log.yellow("warning") << " " << w << "\n";
    log.out << "\n" << log.bold("Campaign") << "  " << report.stopReason << "; " << report.runs.size() << " run(s) this session\n\n";
    printCoverage(log, net, session.coverage());
    printRunsAndFindings(log, session.coverage());
    printCrashPredictions(log, session.coverage());  // GLM-5.3-Flash
    printHookRuns(log, session.coverage());  // GLM-5.3-Flash

    // GLM-5.3-Flash: say which guidance steered the campaign, so a switched-off
    // ant colony is visible in the output (--no-ants).
    log.out << "\n" << log.bold("Guidance") << "  ";
    if (session.antsEnabled()) {
        const pn::AntStats &ants = session.ants().stats();
        log.out << "ant colony: " << ants.iterations << " round(s), " << ants.ants << " ant walk(s)\n";
    } else {
        log.out << "suggester only: the ant colony is switched off (--no-ants)\n";
    }

    // GLM-5.3-Flash: the remaining guidance switches, same words and order as
    // in the --json report, so the console output and the JSON agree.
    const SessionConfig &sessionCfg = session.config();
    log.out << "  targets: deadlock " << toString(sessionCfg.targets.deadlock) << ", livelock "
            << toString(sessionCfg.targets.livelock) << "; coverage threshold " << sessionCfg.coverage.threshold
            << "\n";
    log.out << "  post-mortems: on deviation " << toString(sessionCfg.dumps.onDeviation) << ", on deadlock "
            << toString(sessionCfg.dumps.onDeadlock) << ", on livelock " << toString(sessionCfg.dumps.onLivelock)
            << "\n";

    // GLM-5.3-Flash: the catastrophe-pheromone pricing switch, plus how many
    // transitions the model tags as temporally unpredictable (human operator
    // in the loop; runs through them keep the deposit flat). Same words and
    // order as in the --json report.
    std::size_t unpredictableCount = 0;
    for (const Transition &t : net.transitions)
        if (t.unpredictable)
            ++unpredictableCount;
    log.out << "  catastrophes: "
            << (sessionCfg.catastropheTimePricing ? "priced by the way in" : "flat (--flat-catastrophe)")
            << "; unpredictable transitions " << unpredictableCount << "\n";

    // GLM-5.3-Flash: the milestone transitions of the model and the attract
    // deposit the session lays for reaching one (per rank), so the milestone
    // feature is visible next to the other pheromone switches.
    std::size_t milestoneCount = 0;
    for (const Transition &t : net.transitions)
        if (t.milestone)
            ++milestoneCount;
    log.out << "  milestones: deposit " << sessionCfg.ants.depositMilestone
            << " per rank; milestone transitions " << milestoneCount << "\n";

    // GLM-5.3-Flash: the command hooks of the model and their bound, so the
    // feature is visible next to the other switches. Nothing when the model
    // declares none (doku section 18). Same words and order as in the --json
    // report.
    std::size_t commandCount = 0;
    for (const Transition &t : net.transitions)
        if (t.command)
            ++commandCount;
    if (commandCount > 0)
        log.out << "  commands: " << commandCount << " transition(s), timeout "
                << sessionCfg.hookTimeoutSeconds << " s\n";

    // GLM-5.3-Flash: the time criterion next to coverage, and how much the
    // runs of this campaign overlap.
    printTime(log, report);
    printOverlap(log, runOverlapMatrix(session.coverage()));

    if (args.given("json")) {
        std::ofstream f(args.get("json"));
        f << campaignJson(net, session, report).dump() << "\n";
        if (!f)
            log.err << "mbt run: cannot write " << args.get("json") << "\n";
    }

    bool failures = false;
    for (const RunReport &r : report.runs)
        failures = failures || isFailure(r.outcome);
    if (failures)
        return kExitFailures;
    if (report.stopReason == "too many aborted runs in a row")
        return kExitInfra;
    if (args.has("require-complete") && !report.complete)
        return kExitIncomplete;
    return kExitOk;
}

}  // namespace cli

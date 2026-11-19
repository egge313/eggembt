cmake_minimum_required(VERSION 3.16)
project(eggembt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# DeepSeek V4.1 Flash: the product version lives in src/Version.h, where it follows
# the git branch (doku section 20); the build no longer defines it.

find_package(Threads REQUIRED)

# GLM-5.3-Flash: build with warnings enabled, as doku.md §10 promises for this
# code (the original Qt-based CMakeLists had no warning flags).
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra)
endif()

enable_testing()

# Engine: plain C++17, no Qt, no POSIX beyond a thread. Semantics, reachability
# graph, reverse index, trace tree, coverage database, belief tracker, livelock
# analysis, target phases, suggester, ant colony, asynchronous event queue,
# session, plug-in protocol, run-log replay.
add_library(mbt_engine STATIC
    src/model/Net.h
    src/model/NetBuilder.h
    src/model/Fingerprint.h
    src/model/Fingerprint.cpp
    src/engine/Events.h
    src/engine/Semantics.h
    src/engine/Semantics.cpp
    src/engine/ReachabilityGraph.h
    src/engine/ReachabilityGraph.cpp
    src/engine/ReverseIndex.h
    src/engine/ReverseIndex.cpp
    src/engine/TraceTree.h
    src/engine/TraceTree.cpp
    src/engine/CoverageDb.h
    src/engine/CoverageDb.cpp
    src/engine/OverlapAnalysis.h
    src/engine/OverlapAnalysis.cpp
    src/engine/BeliefTracker.h
    src/engine/BeliefTracker.cpp
    src/engine/Scc.h
    src/engine/Scc.cpp
    src/engine/LivelockAnalysis.h
    src/engine/LivelockAnalysis.cpp
    src/engine/TargetBoard.h
    src/engine/TargetBoard.cpp
    src/engine/Suggester.h
    src/engine/Suggester.cpp
    src/engine/AntColony.h
    src/engine/AntColony.cpp
    src/engine/EventQueue.h
    src/engine/EventQueue.cpp
    src/engine/SutPlugin.h
    src/engine/Session.h
    src/engine/Session.cpp
    src/engine/Json.h
    src/engine/Json.cpp
    src/engine/Protocol.h
    src/engine/Protocol.cpp
    src/engine/RemotePlugin.h
    src/engine/RemotePlugin.cpp
    src/engine/RunLog.h
    src/engine/RunLog.cpp
)
target_include_directories(mbt_engine PUBLIC src)
target_link_libraries(mbt_engine PUBLIC Threads::Threads)

# I/O at the edge: JSON model loader, PNML model loader (GLM-5.3-Flash,
# doku section 12 item 6) and JSON-lines run log. Pure C++17 as well; the JSON
# it reads and writes is the engine's own (engine/Json.h).
add_library(mbt_core STATIC
    src/io/JsonNetLoader.h
    src/io/JsonNetLoader.cpp
    src/io/PnmlLoader.h
    src/io/PnmlLoader.cpp
    src/io/PnmlWriter.h
    src/io/PnmlWriter.cpp
    src/io/RunLogIO.h
    src/io/RunLogIO.cpp
)
target_include_directories(mbt_core PUBLIC src)
target_link_libraries(mbt_core PUBLIC mbt_engine)

# The process boundary: stdio/pipe/tcp transports, the generic agent shim and
# the simulated SUT. POSIX-only pieces are compiled where they work; the
# stream transport covers the rest.
add_library(mbt_agent STATIC
    src/engine/AgentServer.h
    src/engine/AgentServer.cpp
    src/engine/SimulatedSut.h
    src/engine/SimulatedSut.cpp
    src/io/Transports.h
    src/io/Transports.cpp
)
target_include_directories(mbt_agent PUBLIC src)
target_link_libraries(mbt_agent PUBLIC mbt_engine)

# The `eggembt` command line tool: check, explore, run, report, agent-sim.
add_executable(eggembt
    src/main.cpp
    src/Version.h
    src/cli/Commands.h
    src/cli/Commands.cpp
    src/cli/Report.h
    src/cli/Report.cpp
    src/cli/Args.h
    src/cli/Console.h
)
target_link_libraries(eggembt PRIVATE mbt_agent mbt_core)

# GLM-5.3-Flash: the reference agent shim for C++ SUTs (doku section 12,
# item 7): a hand-written SUT wrapped in ISutPlugin + AgentServer, spawned by
# 'eggembt run --agent' in the cli tests.
add_executable(ref-agent examples/agents/ref_agent.cpp)
target_link_libraries(ref-agent PRIVATE mbt_agent)

# Engine tests (plain C++, no Qt, no network at run time)
foreach(t tst_engine tst_coverage tst_belief tst_livelock tst_suggest tst_ants tst_async tst_overlap tst_crash)
    # DeepSeek V4.1 Flash: tst_pheromone is added below (it needs a plug-in, hence mbt_engine
    # only -- the synchronous benchmark SUT is local to the test, no agent library).
    add_executable(${t} tests/${t}.cpp)
    target_include_directories(${t} PRIVATE tests)
    target_link_libraries(${t} PRIVATE mbt_engine)
    add_test(NAME ${t} COMMAND ${t})
endforeach()

# DeepSeek V4.1 Flash: the pheromone benchmark (doku section 25): a 1000-state
# model with a crashing shortcut at every run's start, run without the colony,
# with the edge pheromone alone, and with the (marking, T-list) pair layer.
add_executable(tst_pheromone tests/tst_pheromone.cpp)
target_include_directories(tst_pheromone PRIVATE tests)
target_link_libraries(tst_pheromone PRIVATE mbt_engine)
add_test(NAME tst_pheromone COMMAND tst_pheromone)

# DeepSeek V4.1 Flash: the reserved lifecycle labels (doku section 26): the
# engine-landed start/restart/resume, the pause/down gate, and the campaign
# that presses the model's own start before it tests anything.
add_executable(tst_lifecycle tests/tst_lifecycle.cpp)
target_include_directories(tst_lifecycle PRIVATE tests)
target_link_libraries(tst_lifecycle PRIVATE mbt_agent)  # the campaign test drives SimulatedSut
add_test(NAME tst_lifecycle COMMAND tst_lifecycle)

# Session and remote tests drive the simulated SUT plug-in.
foreach(t tst_session tst_remote)
    add_executable(${t} tests/${t}.cpp)
    target_include_directories(${t} PRIVATE tests)
    target_link_libraries(${t} PRIVATE mbt_agent)
    add_test(NAME ${t} COMMAND ${t})
endforeach()

# Transports, agent shim and simulated SUT: stdio, pipes, child processes, TCP.
add_executable(tst_transports tests/tst_transports.cpp)
target_include_directories(tst_transports PRIVATE tests)
target_link_libraries(tst_transports PRIVATE mbt_agent)
add_test(NAME tst_transports COMMAND tst_transports)

# JSON model loader (checked against the examples) and run-log I/O.
add_executable(tst_jsonnetloader tests/tst_jsonnetloader.cpp)
target_include_directories(tst_jsonnetloader PRIVATE tests)
target_link_libraries(tst_jsonnetloader PRIVATE mbt_core)
target_compile_definitions(tst_jsonnetloader PRIVATE
    EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples")
add_test(NAME tst_jsonnetloader COMMAND tst_jsonnetloader)

add_executable(tst_runlogio tests/tst_runlogio.cpp)
target_include_directories(tst_runlogio PRIVATE tests)
target_link_libraries(tst_runlogio PRIVATE mbt_core)
add_test(NAME tst_runlogio COMMAND tst_runlogio)

# GLM-5.3-Flash: PNML import (doku section 12, item 6), checked against the
# shipped example; the session part uses the simulated SUT plug-in.
add_executable(tst_pnml tests/tst_pnml.cpp)
target_include_directories(tst_pnml PRIVATE tests)
target_link_libraries(tst_pnml PRIVATE mbt_core mbt_agent)
target_compile_definitions(tst_pnml PRIVATE
    EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples")
add_test(NAME tst_pnml COMMAND tst_pnml)

# DeepSeek V4.1 Flash: installing the two programs (INSTALL.md). The product is the
# command-line tool; ref-agent is the reference agent shim, useful to check a
# fresh install against a SUT without writing one. The internal static
# libraries are not installed -- nothing outside links against them.
include(GNUInstallDirs)
install(TARGETS eggembt ref-agent RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# End-to-end tests of the tool: real processes, pipes and sockets.
add_test(NAME cli_test
    COMMAND ${CMAKE_COMMAND} -E env
        "EGGEMBT=$<TARGET_FILE:eggembt>"
        "EXAMPLES=${CMAKE_CURRENT_SOURCE_DIR}/examples"
        "REF_AGENT=$<TARGET_FILE:ref-agent>"
        sh ${CMAKE_CURRENT_SOURCE_DIR}/cli_test.sh)

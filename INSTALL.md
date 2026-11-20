<!-- DeepSeek V4.1 Flash: installation guide (doku.md section 23). -->
# Installing eggembt

`eggembt` — *Egge's Model-Based Tester* — is a single command-line program
built from source. There are no packages, no third-party libraries, no
daemon, no configuration file and no state outside the files you name. A run
log is an ordinary file you give (`--log`), reports are files you give, and
nothing is read from `$HOME`.

## What you need

| need | why |
|------|-----|
| **CMake ≥ 3.16** | the only build tool; any generator works |
| **a C++17 compiler** | the code is C++17, built with `-Wall -Wextra` |
| **a thread library** | the transports and the event queue use `std::thread`; CMake's `find_package(Threads)` finds it |
| **Ninja** *(optional)* | faster; the examples below use `-G Ninja`, omit it without Ninja |
| **a POSIX shell** *(optional)* | only the end-to-end suite `cli_test.sh` needs `sh` |
| **a POSIX system** *(recommended)* | Linux, macOS and the BSDs get the full feature set: `--agent`, `--connect`, `--listen`, pipes, TCP. Elsewhere only the stream transport is compiled, so `--simulate` and `--stdio` work and the agent options are refused with a message saying why (`src/io/Transports.h`, `MBT_POSIX_IO`) |

Nothing has to be installed system-wide beyond a compiler and CMake.

## Build

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j

`cmake -S . -B build` only *configures*; `cmake --build build` compiles. Both
steps are needed. `Release` is the recommendation — a campaign is CPU-bound
(the reachability graph and the ant colony) — but every build type compiles
and passes the tests.

The build produces two programs and the test executables:

| path | what |
|------|------|
| `build/eggembt` | the tester (the product) |
| `build/ref-agent` | the reference agent shim: a hand-written SUT in the plug-in protocol, for trying `--agent` without writing a wrapper |
| `build/tst_*` | the unit test suites, one executable each, sources in `tests/`; plain programs that take no arguments |

The internal static libraries (`mbt_engine`, `mbt_core`, `mbt_agent`) stay in
the build tree; nothing outside links against them.

## Test before you install

    ctest --test-dir build

That is the whole suite: **19 entries** — 18 unit suites plus `cli_test.sh`,
which spawns the real binary against the examples, over pipes and sockets. It
takes about a minute on a laptop (the pheromone benchmark is the slow one,
about nine seconds on its own).

Run the end-to-end suite on its own if `ctest` is not at hand (or to see each
scenario's name):

    EGGEMBT=$PWD/build/eggembt EXAMPLES=$PWD/examples \
        REF_AGENT=$PWD/build/ref-agent sh cli_test.sh

It ends with `40 tests, 0 failure(s)`; without `REF_AGENT` set, the
reference-agent scenario is skipped and announced as such. Both invocations
assume the source tree is the working directory, because the tests read
`examples/`.

## Use it from the build tree

Installing is optional — the binary does not look for data files beside
itself, so it already works where it was built:

    ./build/eggembt version
    ./build/eggembt check examples/elevator-door.json
    ./build/eggembt run examples/drink-machine.json --simulate --quiescence 40

The campaign above should end `coverage complete`; `--simulate` needs no SUT
at all.

## Install

    cmake --install build --prefix "$HOME/.local"

or system-wide:

    sudo cmake --install build --prefix /usr/local

This installs exactly two programs:

    $PREFIX/bin/eggembt      the tester
    $PREFIX/bin/ref-agent    the reference agent shim

`$PREFIX` defaults to `/usr/local`. Put `$PREFIX/bin` on your `PATH` — for a
user-local install, add this to `~/.profile` or `~/.bashrc`:

    export PATH="$HOME/.local/bin:$PATH"

Then check the version:

    eggembt version            # eggembt v0.0.4

The version follows the git branch and lives in `src/Version.h` only; there is
no configure-time version to pass. Nothing else is installed — no libraries,
headers, manual page, shell completion or data files. The examples stay in
the source tree, which is also where the tests read them.

## Verify the installation

Without leaving the source tree (so `examples/` is at hand):

    eggembt version
    eggembt check examples/drink-machine.json
    eggembt run examples/drink-machine.json --simulate --quiescence 40
    eggembt help

The run should print `coverage complete` and six of six transitions. To check
the process boundary as well — an agent started as a child process, speaking
the protocol over pipes — use the installed reference agent:

    eggembt run examples/agents/coffee.json --agent ref-agent --quiescence 40

Expected: `Testing coffee-agent against agent "ref-agent" (eggembt-ref-agent;
...)`, one `completed` run and `coverage complete`.

## Where files are written

There is no configuration and no hidden state. Everything is a path you give:

- **run log** — `--log FILE`, appended; `report` and `compare` read it back.
  The log carries its own format version (`1`); an older or newer format is
  refused with `unsupported run log version` rather than misread.
- **reports** — `--json FILE`, `--dot FILE`, `--dump-pheromone FILE`, `--csv
  FILE` (see `userdoc.md` §8.3, §10).
- **post-mortem dumps** — not written by the tester: the plug-in reports the
  names of the files it produced (`files` in the post-mortem reply). The
  built-in simulator only reports `dump-N.core` / `dump-N.snapshot` without
  creating anything; a real agent wrapper decides where its dumps go.
- **nothing in `$HOME`**, no registry, no cache, no daemon.

The only environment variables the tool sets are the ones a `command` hook
sees for the length of that command: `EGGEMBT_TRANSITION`,
`EGGEMBT_TRANSITION_LABEL`, `EGGEMBT_RUN`, `EGGEMBT_LOG` (`userdoc.md` §9).
They are not read from your environment.

## Upgrading and removing

- **Upgrade** — pull, rebuild, reinstall:

      cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
      cmake --build build -j
      ctest --test-dir build
      cmake --install build --prefix "$HOME/.local"

  Existing models and run logs keep working as long as the model text (its
  fingerprint) and the log format version are unchanged; a log recorded for a
  different model is refused by `report`/`compare`.

- **Remove** — delete the two files:

      rm "$HOME/.local/bin/eggembt" "$HOME/.local/bin/ref-agent"

  (Nothing else was installed.)

## Troubleshooting

- **`cmake: command not found`** — install CMake ≥ 3.16 from your package
  manager, or unpack the official tarball.
- **`-G Ninja` fails** — Ninja is not installed; drop the flag and let CMake
  pick its default generator.
- **`ctest` says no tests** — the configure step did not run in that build
  directory; reconfigure with `cmake -S . -B build`.
- **`cli_test.sh` reports missing examples** — run it from the source tree and
  pass `EXAMPLES=$PWD/examples` (see above).
- **`--agent` / `--connect` / `--listen` are refused** — this build has no
  POSIX transports (non-POSIX platform); use `--simulate` or `--stdio`.
- **A campaign is slow** — build with `-DCMAKE_BUILD_TYPE=Release`; the
  default build type is unoptimized.

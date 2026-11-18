# The mbt plug-in protocol (version 1)

How the tracker (`mbt run`, a `Session`) talks to an **agent** — a small
program that sits next to the system under test (SUT), possibly in another
process, on another machine, written in another language. The agent starts the
SUT in a requested state, hands over the inputs the tracker suggests, takes
post-mortems on request, and reports everything the SUT does as events.

One JSON object per line. UTF-8, `\n` terminated, no raw newline inside a
message (escape them as `\n` inside JSON strings). Both sides flush after every
line. Unknown fields are ignored in every message, so the protocol can grow; an
unknown `type` is an error. The current version is 1 (`pn::kProtocolVersion`).

Wire encoding and decoding live in `src/engine/Protocol.h` / `Protocol.cpp`
(`encodeMessage`, `decodeMessage`). Transports (standard streams, file
descriptors, child processes, TCP) live in `src/io/Transports.h`; they all
implement the three-method `ILineTransport` interface (`sendLine`,
`setLineHandler`, `setCloseHandler`). The generic agent shim, which turns any
`ISutPlugin` into a speaking agent, is `src/engine/AgentServer.h`; the tracker
side is `src/engine/RemotePlugin.h`. `mbt agent-sim MODEL` is a ready-made
reference agent over the built-in simulated SUT.

Direction of the five message types per side:

| agent → tracker        | tracker → agent  |
| ---------------------- | ---------------- |
| `hello`                | `start_run`      |
| `run_started`          | `stimulate`      |
| `event`                | `post_mortem`    |
| `post_mortem_result`   | `stop_run`       |
| `fault`                | `shutdown`       |

## 1. Handshake

The agent speaks first. On connecting, it sends:

```json
{"type":"hello","protocol":1,"name":"drink-agent","snapshot":true,"crash":false,"restart_ms":2000}
```

| field        | required | meaning                                                        |
| ------------ | -------- | -------------------------------------------------------------- |
| `protocol`   | yes      | must be `1`; anything else ends the session with an error      |
| `name`       | no       | free text, shown in the tracker's log                          |
| `snapshot`   | no       | the agent can take non-destructive dumps (default false)       |
| `crash`      | no       | the agent can crash-dump the SUT (default false)               |
| `restart_ms` | no       | typical time to restart the SUT; informational                 |

The tracker waits for hello up to `--hello-timeout` (default 10000 ms). The
capabilities decide what it will ever ask for: without `snapshot` it never
requests a snapshot post-mortem, without `crash` never a crash.

## 2. Messages from the tracker

### start_run

```json
{"type":"start_run","run":1,"initial":"start","quiescence_ms":500}
```

| field           | required | meaning                                                                 |
| --------------- | -------- | ----------------------------------------------------------------------- |
| `run`           | no       | the number of this run; echo it in every event of the run               |
| `initial`       | yes      | name of the model's initial marking to start the SUT in                 |
| `quiescence_ms` | no       | after this much SUT silence, send a `quiescent` event (0: never)        |

Start (or restart) the SUT in that state and answer with `run_started` — the
tracker waits up to ten seconds. Do not wait for the SUT's first event first;
a run that cannot be started must be reported, not hung.

### stimulate

```json
{"type":"stimulate","label":"coin.insert"}
```

Hand this input (a `controllable` transition's label) to the SUT. The
acknowledgement is the `event` with `kind` `stimulated` you send once the input
has actually been handed over — if it never arrives, the tracker aborts the run
after `--ack-timeout` (default 5000 ms). If you cannot hand the input over at
all, say so with a `fault` message.

### post_mortem

```json
{"type":"post_mortem","id":1,"mode":"snapshot"}
```

| field   | required | meaning                                                |
| ------- | -------- | ------------------------------------------------------ |
| `id`    | yes      | echo it in the `post_mortem_result`                    |
| `mode`  | yes      | `snapshot`: non-destructive dump, the SUT keeps running and its events keep arriving. `crash`: end the SUT in a controlled way for a genuine crash dump; the next `start_run` restarts it. |

The tracker waits up to 60 seconds for the result.

### stop_run

```json
{"type":"stop_run","reason":"finished"}
```

`reason` is `finished` (the run ended normally) or `aborted`. Clean up after
the SUT and stop sending events of this run; be ready for the next
`start_run` immediately. There is no reply to `stop_run` — the reply to the
next `start_run` doubles as the acknowledgement.

### shutdown

```json
{"type":"shutdown"}
```

The session is over. Flush, close the connection, exit cleanly (status 0).
Nothing is persisted between sessions: the next one is a fresh process.

## 3. Messages from the agent

### run_started

```json
{"type":"run_started","ok":true}
```

| field   | required | meaning                                             |
| ------- | -------- | --------------------------------------------------- |
| `ok`    | yes      | whether the SUT was started in the requested state  |
| `error` | no       | why not, when `ok` is false                         |

`ok:false` aborts the run; runs that cannot be started again and again abort
the campaign (three aborted runs in a row by default).

### event

```json
{"type":"event","kind":"observed","label":"coffee.served","run":1,"t":730}
```

| field   | required | meaning                                                                       |
| ------- | -------- | ----------------------------------------------------------------------------- |
| `kind`  | yes      | `observed` (an output or traced internal step of the SUT), `stimulated` (the acknowledgement of an input), or `quiescent` (the SUT has been silent for `quiescence_ms`) |
| `label` | see text | the label of the transition; required for `observed` and `stimulated`, absent for `quiescent` |
| `run`   | no       | the run number, echoed from `start_run` (send it; see below)                  |
| `t`     | no       | capture timestamp in milliseconds, from your own clock; the tracker only compares timestamps within one run |

Rules that matter more than the fields:

- Send each event as soon as it is captured, in capture order. The arrival
  order is the one truth the tracker works with; a race between a suggested
  input and a fast SUT is preserved, not smoothed away.
- The tracker never blocks you: it does not answer events, so never wait for
  an answer. Events go on the tracker's asynchronous queue; if that is full it
  refuses and the run is aborted — never dropped silently.
- Echo the `run` number. Events carrying an older run number are dropped by
  the tracker as late arrivals of a previous run, so tagging is what keeps a
  stale event from poisoning the next run.

### post_mortem_result

```json
{"type":"post_mortem_result","id":1,"ok":true,"files":["/tmp/core.1234"],"stack":"..."}
```

| field   | required | meaning                                                  |
| ------- | -------- | -------------------------------------------------------- |
| `id`    | yes      | the `post_mortem` request this answers                   |
| `ok`    | yes      | whether a dump was taken                                 |
| `files` | no       | paths of the dump files (core, minidump, stack file, …)  |
| `stack` | no       | the SUT's stack as text, if you can produce one          |
| `error` | no       | why not, when `ok` is false                              |

If the requested mode is impossible for you, answer `ok:false` with the reason
(the session records it as a note); a plug-in that was asked for a crash and
can only take a snapshot may fall back to one and report its files.

### fault

```json
{"type":"fault","message":"could not send input \"coin.insert\" to the SUT: connection lost"}
```

An unrecoverable problem of the agent's own: the SUT refused an input, the
connection to the SUT is gone, the tracker sent something unparseable
(`"bad message from the tracker: …"`). The tracker aborts the run. Dying
without a word is also noticed — the closing connection is treated as a fault
without a message — but a fault lets you say why.

## 4. A run, line by line

Using the drink machine of `examples/drink-machine.json` (left column: agent,
right column: tracker):

```
→ {"type":"hello","protocol":1,"name":"drink-agent","snapshot":true,"crash":false,"restart_ms":2000}
← {"type":"start_run","run":1,"initial":"start","quiescence_ms":500}
→ {"type":"run_started","ok":true}
← {"type":"stimulate","label":"coin.insert"}
→ {"type":"event","kind":"stimulated","label":"coin.insert","run":1,"t":104}
← {"type":"stimulate","label":"button.coffee"}
→ {"type":"event","kind":"stimulated","label":"button.coffee","run":1,"t":210}
→ {"type":"event","kind":"observed","label":"coffee.served","run":1,"t":730}
→ {"type":"event","kind":"quiescent","run":1,"t":1240}
← {"type":"stop_run","reason":"finished"}
← {"type":"start_run","run":2,"initial":"start","quiescence_ms":500}
→ {"type":"run_started","ok":true}
… (further runs, e.g. tea or a deviation)
← {"type":"shutdown"}
```

Everything the agent sent after `shutdown` would be ignored; the tracker then
closes the transport.

## 5. Checklist for agent authors

1. One JSON object per line, flushed immediately, over any byte transport.
   Ignore unknown fields; treat unknown message types as an error and say so
   in a `fault`.
2. Send `hello` before reading anything else. Do nothing to the SUT before
   `start_run`.
3. Answer `start_run` as soon as the SUT is started (or report `ok:false`
   with the reason). Never wait for the SUT's first event first.
4. Tag every event with the `run` number, stamp `t` at capture, send events in
   capture order, and never wait for an answer to an event.
5. After `quiescence_ms` of SUT silence, send a `quiescent` event (no label).
   The tracker has no clock of its own; without your quiescent event it cannot
   notice a SUT that fell silent while it owed an output.
6. Acknowledge every `stimulate` with a `stimulated` event once the input has
   actually been handed over — not when the SUT has reacted to it.
7. On `post_mortem`, take the dump and answer with the same `id`, the file
   paths, and the stack if you can produce one. Never stop the SUT for a
   snapshot; after a `crash`, expect the next `start_run` to restart it.
8. On `stop_run`, clean up and stop sending events of that run. Keep the
   process alive: the next run starts with a new `start_run`, not a new agent.
9. Report unrecoverable problems with `fault` instead of dying silently.
10. On `shutdown`, exit(0).
11. To try your agent: `mbt run MODEL --agent "./your-agent MODEL"`, or keep it
    long-lived and use `--connect HOST:PORT` / `--listen [ADDR:]PORT`, or talk
    standard input/output with `--stdio` (the protocol then owns the pipe, so
    the tracker's human-readable output goes to stderr).

## 6. Where the pieces live

| what                              | where                            |
| --------------------------------- | -------------------------------- |
| wire format, encode/decode        | `src/engine/Protocol.h/.cpp`     |
| generic agent shim                | `src/engine/AgentServer.h/.cpp`  |
| tracker-side plug-in              | `src/engine/RemotePlugin.h/.cpp` |
| line transports                   | `src/io/Transports.h/.cpp`       |
| the plug-in interface underneath  | `src/engine/SutPlugin.h`         |
| protocol and transport tests      | `tests/tst_remote.cpp`, `tests/tst_transports.cpp` |
| a working reference agent         | `mbt agent-sim MODEL`            |

---

The design context for this protocol is `doku.md` §8. Document written on
14 November 2026 by GLM-5.3-Flash.

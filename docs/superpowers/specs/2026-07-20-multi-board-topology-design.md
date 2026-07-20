# Multi-Board Topology Support (Core + TUI)

Date: 2026-07-20
Status: Approved

## Scope

Add support for connecting to and managing multiple PSB boards from one client
session — different board variants, a variable number of boards, and boards
that may or may not share a physical serial bus (RS-485 multi-drop). This
document covers `tools/psb_modbus_core` and `tools/psb_demo_app/tui`.
`psb_demo_gui` multi-board support is explicitly deferred (see Out of Scope).

The existing single-board connection flow — `-p/-b/-i/-t` flags, with
`psb_modbus_core`'s `ConfigManager` (`~/.psb_demo_app.toml`) supplying
fallback defaults when a flag is omitted, and `psb_demo_cli --save`
persisting the currently-used settings back to that file — is the baseline
and stays the default, most-frequently-used path. Nothing about it should
look or feel different for a user who only ever connects to one board;
`TopologyConfig` (below) supersedes `ConfigManager` and preserves this exact
convenience (auto-load-as-default, a `--save` flag), just on the new format.

## Motivation

Every client (`psb_modbus_core`'s `PsbModbusClient`, `psb_demo_tui`,
`psb_demo_gui`) currently assumes exactly one board: `PsbModbusClient` owns
one serial port exclusively and bakes in one slave ID at `connect()` time.
There is no way to address two boards from one client process today, and no
safe way to have two boards share one physical RS-485 line — two independent
`PsbModbusClient` instances opening the same port path would either fail
(exclusive lock) or corrupt each other's traffic (two masters on one line;
see `docs/guide/client-architecture-and-pitfalls.md` §3's `mbpoll`
contention warning, which is the same failure mode).

The existing config precedent (`ConfigManager`) is single-board-only and
flat (`[connection]` port/baud/slave-id/timeout, `[display]` poll interval
— see `config_manager.cpp`) with no notion of multiple boards or buses, so
it can't be extended in place to a nested topology; it needs a like-for-like
replacement (`TopologyConfig`) rather than an extension. `psb_demo_tui` also
parses its flags with a hand-rolled `argv` loop rather than CLI11 (which
`psb_demo_cli` already uses), which the new `--topology` flag is a natural
occasion to fix.

## Design Decisions

### Bus/Session split, not a shared-port hack

Three approaches were considered for letting boards share a physical bus:

1. **Chosen: split `PsbSerialBus` (owns the port) from `PsbBoardSession`**
   (one per slave ID, references a bus). All Modbus I/O for every board on a
   bus routes through that bus's single serialized transaction primitive.
   Collapses to today's exact behavior when there's one bus with one board.
2. **Rejected: shared mutex-guarded port handle inside an otherwise-unchanged
   `PsbModbusClient`.** Smaller diff, but every I/O path in the class has to
   remember to take the lock — turns "don't let two masters share a line"
   from a documented user hazard into an easy-to-miss internal bug. This is
   the exact failure mode the feature exists to prevent.
3. **Rejected: process-per-bus with IPC.** Only relevant if boards needed
   controlling from separate machines. Not the case here — unnecessary
   complexity.

`PsbSerialBus` is **not** internally thread-safe, by design — it mirrors
`PsbModbusClient`'s existing contract (see `client-architecture-and-pitfalls.md`
§2.1: serial is inherently single-writer). The safety guarantee comes from
"exactly one worker thread drives a given bus," enforced by the calling
application (the TUI's per-bus worker thread — see below), not by locking
inside the library.

### Unified runtime path

The simple single-board flags (`-p/-b/-i/-t`, no `--topology`) synthesize an
in-memory one-bus/one-board topology and run through the same
`PsbSerialBus`/`PsbBoardSession` machinery as the multi-board path, rather
than keeping two parallel implementations. `PsbModbusClient`'s public API is
kept unchanged and becomes a thin facade over exactly that (a bus with one
board) — see Architecture below — so `psb_demo_gui`, which isn't touched in
this phase, keeps compiling and working with zero changes.

### Config format: TOML, not YAML

TOML was chosen over YAML: `toml++` is already a linked dependency (used by
the dead `ConfigManager`), its array-of-tables syntax (`[[bus]]`,
`[[bus.board]]`) maps naturally onto the nested bus→boards structure, and it
avoids YAML's well-known hand-editing footguns (indentation sensitivity,
implicit-typing surprises) that matter here specifically because users are
expected to hand-manage and reuse these files.

### CLI11 for flags, toml++ for the topology file body

CLI11 is used for all command-line flags on both `psb_demo_tui` (replacing
its hand-rolled `argv` loop) and `psb_demo_cli`, including a
`--topology <path>` flag. CLI11's own config-file auto-loading
(`set_config()`) is *not* used for the topology file itself — it expects a
flatter key=value format, poorly suited to a nested multi-bus/multi-board
structure. CLI11's job is only to decide *which* topology file to open;
`toml++` parses its contents into structured data.

### Board identity: required user nickname

Every board entry has a required, user-chosen nickname (e.g. `hvb-bench`,
`lvb-rack3-ch0`), set during interactive setup or by hand-editing the file.
The nickname is what's shown everywhere in the UI (board switcher, status
messages) instead of raw `bus+slave-id`, and is stable across slave-ID
renumbering.

### Variant is never stored in config

A board's variant (HVB/LVB/etc.) is always discovered live via
`readSystemInfo()` at connect time, exactly as today — the topology file only
records *how to reach* a board (bus, slave ID, nickname), never *what it is*.
This avoids the config going stale relative to the actual hardware.

## Architecture

### `psb_modbus_core`

```
psb_serial_bus.h/.cpp       new: owns one physical serial connection
psb_board_session.h/.cpp    new: one board (slave ID) on a PsbSerialBus
psb_modbus_client.h/.cpp    unchanged public API; internally a PsbSerialBus
                             + PsbBoardSession(bus, slaveId) owned together
topology_config.h/.cpp      new: TOML topology load/save (supersedes config_manager)
config_manager.h/.cpp       removed — live today (single-board load/--save
                             fallback in both tools), behavior migrated to
                             topology_config, see CLI flags precedence below
```

`PsbSerialBus` (sketch):

```cpp
class PsbSerialBus {
public:
    bool connect(const std::string& port, int baud = 115200, int timeoutMs = 500);
    void disconnect();
    bool isConnected() const;
    std::string lastError() const;

    // Serialized transaction primitives — not thread-safe; caller (one
    // worker thread per bus) must serialize its own access. Every
    // PsbBoardSession attached to this bus routes through these.
    bool readInputRegs(int slaveId, uint16_t addr, uint16_t count, uint16_t* out,
                        int timeoutOverrideMs = -1);
    bool readHoldingRegs(int slaveId, uint16_t addr, uint16_t count, uint16_t* out,
                          int timeoutOverrideMs = -1);
    bool writeReg16(int slaveId, uint16_t addr, uint16_t value);

    // Frame callback gains a slaveId parameter vs. today's PsbModbusClient
    // signature, so a shared bus's frame log can attribute each exchange.
    using FrameCallback = std::function<void(int slaveId, bool tx, const std::vector<uint8_t>&)>;
    void setFrameCallback(FrameCallback cb);

    static std::vector<std::string> scanPorts();
    static std::vector<int> availableBaudRates();
};
```

`PsbBoardSession` (sketch): same high-level read/write method set as
today's `PsbModbusClient` (`readSystemInfo`, `readChannelInfo`,
`writeConfiguredTargetVoltage`, ... — the entire existing surface,
unchanged signatures), each internally calling
`m_bus->readInputRegs(m_slaveId, ...)` etc. instead of owning a port. Holds
its own `currentUnitExp` cache (legitimately per-board, since different
boards can report different values).

`TopologyConfig` (sketch):

```cpp
struct BoardConfig { std::string nickname; int slaveId; };
struct BusConfig {
    std::string name;             // optional, defaults to "bus1"/"bus2".../ by position
    std::string port;
    int baudRate = 115200;
    std::vector<BoardConfig> boards;
};
struct TopologyConfig {
    std::vector<BusConfig> buses;

    static TopologyConfig load(const std::string& path);
    bool save(const std::string& path) const;
    static TopologyConfig singleBoard(const std::string& port, int baud,
                                       int slaveId, const std::string& nickname = "board1");
    static std::string defaultPath();  // ~/.psb_demo_app/topology.toml
    int totalBoardCount() const;       // sum of boards across all buses
};
```

TOML schema:

```toml
[[bus]]
name = "bus1"
port = "/dev/ttyUSB0"
baud_rate = 115200

  [[bus.board]]
  nickname = "hvb-bench"
  slave_id = 1

  [[bus.board]]
  nickname = "hvb-bench-2"
  slave_id = 2

[[bus]]
name = "bus2"
port = "/dev/ttyUSB1"
baud_rate = 115200

  [[bus.board]]
  nickname = "lvb-rack3"
  slave_id = 1
```

`timeout_ms`/`poll_interval_ms` are session-wide CLI flags (`-t`/`-s`,
unchanged from today), not per-bus topology fields — they're runtime tuning
knobs, not connection topology, and stay out of the file to keep its scope
purely "how do I physically reach this hardware."

### CLI flags (CLI11, both `psb_demo_tui` and `psb_demo_cli`)

```
-T, --topology <path>    Topology config file (default: ~/.psb_demo_app/topology.toml)
-p, --port <path>        Quick single-board: serial port
-b, --baud <rate>        Quick single-board: baud rate (default 115200)
-i, --slave-id <id>      Quick single-board: slave ID (default 1)
-t, --timeout <ms>       Response timeout ms (existing default per tool)
-s, --poll-interval <ms> Poll interval ms, session-wide across all buses (TUI only)
--save                   psb_demo_cli only: persist the settings just used as a
                          one-board topology at the --topology path (or default
                          path if none given) — supersedes ConfigManager's --save
--setup                  TUI only: launch interactive topology setup wizard
```

Precedence, replacing today's `ConfigManager` load order like-for-like:

1. `-p` given → quick single-board connect, exactly as today; ignores any
   topology file for connecting (`--save`, if also given to `psb_demo_cli`,
   still persists these settings to the topology path afterward).
2. Else `--topology <path>` given (explicitly, or defaulted) and that file
   **exists** → load and use it, same as `ConfigManager.load()` succeeding
   today. Must resolve to exactly one board in this phase (§ below) or error
   clearly naming the multi-board restriction.
3. Else `--topology <path>` given and that file **doesn't exist**:
   `psb_demo_tui` auto-launches `--setup` pre-targeting that path as the Save
   destination (new — today's `ConfigManager.load()` would just silently
   fall through to hardcoded defaults); `psb_demo_cli` errors, since it has
   no interactive path to create one.
4. Else (neither given) → same as case 2/3 but against the **default**
   topology path, matching today's implicit `ConfigManager` auto-load: if it
   exists, connect with it directly, no prompt, exactly like today picking
   up a previously-`--save`d `~/.psb_demo_app.toml`; if it doesn't exist,
   `psb_demo_tui` falls back to today's hardcoded `/dev/ttyUSB0` guess for
   a genuinely first-ever run rather than erroring — matching current
   behavior exactly, an interactive wizard is offered but connecting is
   still attempted first, same as today never having required setup before
   first connecting.

`psb_demo_cli` gains `--topology <path> --board <nickname>` as an
alternative to typing `-p/-b/-i` for a board already saved in a topology
file, for a single one-shot command against that one board. `--board` is
required whenever `--topology` resolves to more than one board total
(across all buses); if the topology has exactly one board, `--board` may be
omitted and that board is used implicitly. It does **not** gain the ability
to address multiple boards within one invocation — that's out of scope (see
below).

### `psb_demo_tui` runtime

Generalizes today's single `modbusWorker` thread (§2.1 of the architecture
doc) to **one worker thread per `PsbSerialBus`**, each running its own
`doPollScan`/`doFullScan`/write-queue-drain loop over that bus's attached
boards. Boards on different buses poll fully concurrently (independent
physical connections); boards sharing a bus are naturally serialized by
sharing that bus's one thread — no bus-level locking needed, same principle
as today's one-thread-per-port rule, just applied per-bus instead of
globally. Per-board write jobs route to the correct bus's queue based on
which board they target.

`ScannedData` becomes per-board (a `std::vector` of board handles, each
wrapping its own `ScannedData`/`ConfigInputs`), using the same informal
atomic-flag-publish pattern as today (§2.2) — no new locking, since each
board's data is still written by exactly one thread (its bus's worker) and
read by the render thread.

UI becomes a board switcher (one entry per nickname) + the existing
Monitor/Channel tab content reused per-board almost as-is. Offline
detection (§2.7) and capability self-heal (§1.5) are unchanged, just
running once per board instead of once total; a whole board can also go
"offline" (bus or board-level failure) via the same consecutive-failure
pattern generalized one level up. With exactly one bus/one board, the
switcher stays hidden and the layout is pixel-identical to today.

### Interactive setup wizard (TUI)

Entry points: `--setup` flag (launches directly into the wizard), and an
in-dashboard action to reach it mid-session (e.g. to add a board without
restarting).

Screen: a list of buses (each showing its boards), with actions:

- **Add Bus** — port (using the existing `PsbSerialBus::scanPorts()` list)
  + baud rate.
- **Add Board — Manual** — slave ID + nickname, typed directly.
- **Add Board — Scan** — probes a slave-ID range on the bus being
  configured (default 1–32, user-adjustable — not the full 1–247 spec
  range, to keep it fast) by attempting a `PsbBoardSession(bus, candidateId)`
  + one `readSystemInfo()` call per candidate with a short timeout override;
  reports variant/name for every candidate that responds. This is exactly
  today's connect-time capability probe, just swept across a slave-ID range
  instead of run once at a known ID — no new probing logic, only a new
  driver loop around it. User then picks which discovered boards to add and
  assigns nicknames. Since a scan is a one-time setup-time cost (not a
  runtime polling cost), the sequential per-candidate latency is acceptable
  and unrelated to the §1.2/§2.4 poll-cadence concerns elsewhere in the
  architecture doc.
- **Edit/Remove** bus or board.
- **Connect Now** — jump into the live dashboard using the current
  in-memory topology, even unsaved.
- **Save** (to the `--topology` path, prompting for one the first time) /
  **Save As** / **Load** another file.

## Testing

### `psb_modbus_core` (Catch2, `psb_tests`)

- `PsbSerialBus`/`PsbBoardSession` transaction serialization across
  multiple sessions on one bus, using the existing `attachTestArrays()`
  test-mode injection extended to be bus-aware.
- Single-board collapse: every existing `PsbModbusClient` test in the
  current suite passes unchanged against the refactored facade — the
  regression backstop proving the split didn't change single-board
  behavior.
- `TopologyConfig` TOML round-trip (parse → struct → serialize → re-parse
  equality); malformed-file error handling.
- CLI11 flag precedence (`-p` overrides `--topology`; default path used
  when neither given).

### `psb_demo_tui`

- Extends the existing `tmux`-driven interactive test methodology (§3 of
  the architecture doc) to multi-board: launch with a `--topology` file
  pointing at test-mode-injected boards, drive the board switcher via
  `tmux send-keys`, same `capture-pane` verification already used.
- Live-hardware pass before calling any phase done: re-measure poll cadence
  and connect-scan timing on real boards once multi-board polling is real,
  same discipline as prior sessions' work — §2.4/§2.5's lessons (pacing
  bounded by genuine round-trip time) apply per-bus now and are exactly the
  kind of thing that looks fixed on paper while being re-broken in practice.

## Rollout — phased implementation

1. **Core**: `PsbSerialBus`/`PsbBoardSession` split, `TopologyConfig` +
   TOML schema, CLI11 flag wiring (`--topology`, single-board synthesis),
   `ConfigManager` removal, unit tests. Landable alone — no UI changes;
   existing single-board TUI/CLI/GUI keep working against the refactored
   core unchanged.
2. **TUI multi-board dashboard**: per-bus worker threads, per-board
   `ScannedData`, board switcher UI. Depends on (1).
3. **TUI interactive setup wizard**: `--setup` flow, scan-assisted
   discovery, save/load. Depends on (1); benefits from (2) existing, but is
   UI-additive and lower-risk.

## Out of Scope

- `psb_demo_gui` multi-board support — deferred to a future spec once
  (1)-(3) are proven out.
- `psb_factory_tool` multi-board support — not requested.
- `psb_demo_cli` orchestrating multiple boards within a single invocation
  (it gains `--topology <path> --board <nickname>` for addressing one
  saved board only).
- Modbus TCP / networked boards — RTU/serial only, matching the existing
  architecture.
- Live hot-plug/re-discovery of new boards appearing on an already-connected
  bus at runtime — adding a board requires the setup wizard and (re)joining
  as part of a session's topology, not automatic runtime insertion.
- Cross-board atomic/transactional operations (e.g. "set voltage on every
  board simultaneously" as one guaranteed-atomic action) — writes to
  different boards stay independent, sequential operations.
- Changes to the existing per-transaction inter-byte timeout or busy-wait
  fixes (§1.2/§1.3 of `client-architecture-and-pitfalls.md`) — unchanged,
  just now apply per-bus instead of globally.

# Modbus Client Architecture & Hard-Won Lessons

A synthesis of `psb_demo_tui`'s architecture and every non-obvious bug found while
hardening it against a real board over USB-serial. Written for three audiences:

1. **Whoever builds out `psb_demo_gui`** (Qt/QML) — several of these bugs already
   exist there in latent form; this doc tells you exactly where.
2. **Whoever designs the next TUI/GUI** for a future variant or feature.
3. **Anyone writing their own client** against `psb_modbus_core`, or directly
   against raw Modbus RTU — the pitfalls in §1 are library/protocol-level, not
   TUI-specific, and will bite any client that doesn't know about them.

If you only read one section, read **§1.2 (the inter-byte timeout bug)** — it
was responsible for the single largest, most confusing performance report in
this project's history (a "~10s connect, ~3s uptime lag" that turned out to be
a two-line formula bug), and it will affect *any* client built on this library
with a generous overall timeout.

---

## 1. Shared foundation: `psb_modbus_core`

`psb_demo_cli`, `psb_demo_tui`, and `psb_demo_gui` all wrap the same
`PsbModbusClient` (`tools/psb_modbus_core/psb_modbus_client.{h,cpp}`). Every
lesson in this section applies to all three, and to any new client — the bugs
lived in the shared library, not in any one UI.

### 1.1 Two read-function contracts — know which one you're calling

The client has **two families** of read methods that look similar but behave
very differently on a transient failure:

| Family | Examples | On failure |
|---|---|---|
| **Value-returning** | `readSystemInfo()`, `readChannelInfo()`, `readSystemConfig()` | Returns a **fresh, default-constructed struct** — every field silently resets to 0/false, including fields that were correct moments ago |
| **Merge-on-success** (reference-taking) | `readSystemStatus(info&)`, `readChannelStatus(ch, caps, info&)`, `readChannelConfig(ch, caps, cfg&)`, `readChannelCalConfig(ch, caps, cfg&)`, `readChannelCapabilities(ch, caps&)`, and the five `readChannel*Block()` methods | Returns `bool` (success); on failure, **the caller's struct is left untouched** — only fields from a successful sub-read are written |

**Why this matters:** if you keep a long-lived cache (any dashboard, any
polling loop) and call a value-returning function every refresh cycle, one
single transient Modbus error will flash your entire display to zeros/defaults
for that frame — protocol version "0.0", uptime "0s", every channel "OFF" —
even though the board never actually reset. This is not hypothetical: it's the
exact bug found live in both `psb_demo_tui`'s original poll loop and
`psb_demo_cli`'s `monitor` command (both fixed; see §3.2 and §4).

**Rule:** any function that runs in a loop against a cache must use the
merge-on-success family. Value-returning functions are fine for genuine
one-shot reads (a CLI command that reads once and exits).

### 1.2 The inter-byte timeout formula — the big one

`PsbModbusClient::connect()` configures two *conceptually unrelated* serial
timeouts:

```cpp
settings.timeoutFirstByte = timeoutMs;                       // wait for a response to START
settings.timeoutInterByte = /* must NOT derive this from timeoutMs */;  // wait for the NEXT byte once one has arrived
```

The bug (now fixed, but worth understanding in depth because it's the kind of
thing that's trivial to reintroduce): the old code computed
`timeoutInterByte = max(10, timeoutMs / 10)`. That formula ties a
**serial-link property** (how long to wait between bytes of an in-flight
frame before deciding it's complete) to an **application-level property**
(how long to wait for a possibly-slow board to start responding at all).

At the TUI's typical `timeoutMs = 3000` (a deliberately generous "don't give
up on a real board too fast" value), this computed to **300ms**. The
underlying Modbus library (`ModbusClientPort`, vendored) waits out the *full*
inter-byte timeout after the last received byte before concluding a frame is
complete and returning control — **even for an already-complete, healthy
response**. That means every single transaction, not just failed ones, paid
this cost in full.

Confirmed via `strace -tt` (absolute timestamps) against a live board: every
transaction measured **~306-308ms**, essentially zero variance — a strong
signal it's a fixed formula artifact, not hardware jitter. `psb_demo_cli`
happened to dodge this by accident: its default `timeoutMs = 500` computed
`timeoutInterByte = 50ms`, which is small enough not to matter — so CLI
commands "looked fine" while the TUI, doing the exact same kind of reads, was
consistently ~6x slower than necessary. A ~60-transaction connect scan
(10 channels × ~6 transactions each) went from **~25-33s down to ~3.83s**
once fixed.

**The fix:** hardcode `timeoutInterByte` to a small constant independent of
`timeoutMs` — 50ms in the current code (`psb_modbus_client.cpp`, `connect()`).
That's generous margin over both the Modbus RTU spec's frame-silence
threshold (~0.3ms at 115200 baud) and real USB-serial adapter buffering
behavior (CH340 and similar commonly have several-ms latency timers) —
small enough to detect a genuinely stalled/corrupted frame quickly, nowhere
near large enough to matter for a healthy one.

**If you write your own client against raw ModbusLib or a different library
entirely: never derive an inter-byte/inter-character timeout from an overall
response timeout.** They answer different questions and should be configured
independently. A dashboard with a patient 3-5s "give the board time to boot"
timeout should NOT pay that patience on every single healthy transaction.

### 1.3 The non-blocking busy-wait — real, but not the bottleneck you'd guess

`readRegsInternal`/`writeRegsInternal` drive the port in non-blocking mode via:

```cpp
do {
    s = m_impl->port->readInputRegisters(...);
} while (Modbus::StatusIsProcessing(s));
```

The underlying library's non-blocking state machine
(`ModbusClientPort::process()`) is explicitly designed to be pumped from an
*external* timer/event loop — it returns `Status_Processing` immediately
while waiting for I/O rather than blocking (only *blocking* mode sleeps
internally; see its `STATE_TIMEOUT` case). A zero-delay `do/while` spin around
it — the "obvious" way to drive a non-blocking API to completion — pegs one
CPU core at ~98-100% for the *entire duration of every transaction*, confirmed
via `ps -T` (per-thread CPU).

This is a real bug (fixed by adding a 1ms `sleep_for` between checks — cheap
next to a Modbus RTU round-trip of tens of ms), but it's worth recording
honestly: **fixing it alone did not fix the reported slowness.** CPU dropped
from ~98% to ~2%, and per-transaction/connect-scan timing was unchanged. The
inter-byte timeout bug (§1.2) was the actual bottleneck; the busy-wait was a
real, independent, worth-fixing problem discovered along the way, not the
answer. Don't let a legitimate-looking finding stop the investigation before
you've actually re-measured against a clean baseline.

**If you write your own client against a non-blocking Modbus API:** never spin
a tight loop on it. Either drive it from a real timer/event loop (the
library's intended usage), or add an explicit small sleep between polls.

### 1.4 Capability-gated register batches — Exception 0x02

Several channel register ranges are only valid to read if the channel has the
matching capability bit (`CH_CAP_VOLTAGE_MEASUREMENT`,
`CH_CAP_CURRENT_MEASUREMENT`, `CH_CAP_RAW_OUTPUT_DRIVE`,
`CH_CAP_OUTPUT_ENABLE`). Reading a register the channel doesn't support **in
the same batch as ones it does** causes the firmware to reject the *entire*
batch with Modbus exception 0x02 — not just return garbage for the
unsupported register. This is why every read function in this library takes
a `caps` parameter and branches its batch boundaries around it (see
`readChannelInfo`, `readChannelStatus`, the five `readChannel*Block()`
methods). If you write your own client, **always fetch/cache capability
flags first** (`readChannelCapabilities()` or the `CH_CAPABILITY_FLAGS`
register directly) and gate every subsequent batch read around them — do not
assume a fixed register layout across channel types.

### 1.5 Capability flags can be "never captured," not just "empty"

`chCapFlags == 0` is not a valid hardware state — every real channel reports
at least one capability bit. If a channel ends up at `0`, it means a
transient read failure during the one-shot connect-time capability probe was
never retried successfully, and (before this was fixed) **every subsequent
poll would keep reusing the cached 0 forever**, since `readChannelStatus()`
takes `caps` as an input, not something it re-derives.

The fix, worth copying into any new client: (a) retry harder specifically
when the connect-time probe comes back `0` (it's not a normal "maybe
succeeds, maybe doesn't" case — 0 is a strong anomaly signal), and (b)
self-heal during ongoing polling: if a channel is still stuck at `0`,
opportunistically re-probe just that one register (`readChannelCapabilities()`,
cheap — a single register) every poll cycle until it succeeds. See
`doFullScan()`/`doPollScan()` in `tools/psb_demo_app/tui/main.cpp`.

---

## 2. `psb_demo_tui` architecture

### 2.1 Threading model

```
┌─────────────────────────────────────────────────────────────────┐
│ Main / render thread (screen.Loop(root))                         │
│  - FTXUI event loop: keyboard/mouse input, redraws               │
│  - Reads ScannedData `data` directly (no lock — see §2.2)         │
│  - Button callbacks push write jobs onto workQueue, return        │
│    immediately (never block the UI on I/O)                        │
└─────────────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────────────┐
│ modbusWorker thread — the ONLY thread that touches the serial port│
│  loop:                                                             │
│    wait_for(workCv, ...)               // see §2.4 for the wait   │
│    drain workQueue (writes, queued from UI thread)                 │
│    if connected: doPollScan(...)        // periodic status poll   │
└─────────────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────────────┐
│ animThread — sleeps 80ms, PostEvent(Custom) to drive the ~12Hz    │
│ breathing-LED connection indicator. Touches no shared data besides │
│ triggering a repaint.                                              │
└─────────────────────────────────────────────────────────────────┘
```

A serial line is inherently single-writer/half-duplex — the one-thread rule
for `modbusWorker` isn't a stylistic choice, it's the only correct design.
The subtlety that *did* need real design work is everything about what
happens on top of that one thread: how much work it does per wake, how often
it wakes, and how it hands results back to the render thread. That's §2.3-2.6.

### 2.2 Shared data model — informal, not lock-free by accident

`ScannedData` (`tui/tui_format.h`) is shared between the worker thread (writer)
and the render thread (reader) with **no mutex** for most fields — only
`valid` is `std::atomic<bool>`. This is intentional, not an oversight: the
existing pattern relies on `valid`/`chLoaded[]`-style flags as informal
acquire/release fences (write the data, *then* flip the flag, *then*
`PostEvent`), and accepts that a renderer reading mid-update sees a torn but
harmless mix of old/new POD values (ints, floats, small enums — no pointers,
no invariants that matter mid-tear). **Do not add a real mutex around
`ScannedData` without first reading §2.5** — the two together (staged writes,
one atomic "publish" point) is what actually fixes torn-read symptoms; the
right fix is at the call site (batch writes into a staging copy, publish all
at once), not global locking, which would just serialize the UI thread behind
the worker thread's think time.

`ConfigInputs` (`tui/widgets.h`) holds the *editable* string/index state each
input widget binds to directly (`Input(&inputs.targetV[ch], ...)`). It's kept
separate from `ScannedData` and synced one-directionally
(`syncDataToInputs()`) after any read that might have changed the underlying
value — never written to from the worker thread directly, to avoid
fighting with in-progress user keystrokes.

### 2.3 Connect-time scan (`doFullScan`) — three separate lessons compressed into one function

1. **Only fetch what the destination view actually displays.** The original
   version fetched full `ChannelConfig` (5 register blocks) for every
   channel at connect — including `recoveryPolicyMode`/derate step, which
   Monitor never renders (only the per-channel Channel tab does). Now
   `doFullScan` fetches only the output/protection/output-enabled blocks
   (what Monitor shows); recovery/derate are fetched lazily, once, the first
   time a user actually opens that channel's tab (guarded by
   `chDetailLoaded[]`, set in `tab_channel.h`'s `Renderer` — FTXUI only
   renders the *active* tab, so this genuinely never fires for tabs nobody
   visits). **Generalizable rule: connect-time cost should scale with what
   the landing view needs, not with the union of every view's needs.**

2. **Publish progress, not partial data.** An earlier iteration revealed
   per-channel data as each channel finished scanning (a natural first
   instinct — "show something as soon as we have it"). Live use showed this
   was *more* confusing than a blank screen: it reads as a torn/inconsistent
   table, not an obviously-still-loading one. The fix: stage every channel's
   result locally, publish **all channels atomically** at the very end, and
   show a single `"Scanning channels... X/N"` progress message for the whole
   duration instead of per-row reveal. **If you're tempted to make a scan
   "feel" faster via incremental reveal, ask a user first — atomic-with-a-
   progress-message reads as far more trustworthy than piecemeal.**

3. **Interleave a cheap read to keep unrelated telemetry alive.** Because the
   whole scan runs as one queued item on the single I/O thread, the periodic
   status poll (§2.5) literally cannot run concurrently — so without special
   handling, the uptime/temperature readout in the menu bar would freeze for
   the entire scan. Fix: call the cheap, 1-transaction `readSystemStatus()`
   after every channel during the scan and repaint. It's still bounded by the
   same serial line, but keeps the readout at most one channel's-worth stale
   instead of stuck for the whole scan.

### 2.4 Poll-loop wake cadence

The worker's `wait_for` uses a short floor (50ms) while connected, falling
back to the configured `-s` interval only when idle/disconnected — **not**
because polling should literally happen every 50ms, but because the *actual*
pacing should come from how long a real sweep takes (bounded by genuine
serial round-trip time), not from an artificial extra delay stacked on top of
it. An earlier version waited up to a full second between poll cycles
*regardless of how long the previous sweep took* — meaning on top of the
inter-byte timeout bug (§1.2), there was a second, independent source of
needless latency. Both needed fixing; neither alone was sufficient.

### 2.5 Periodic poll (`doPollScan`) — the torn-table vs. frozen-uptime tension (and why it isn't actually a tension)

Two symptoms were reported that look like they pull in opposite directions:

- *"The uptime counter updates too slowly."*
- *"The Monitor table updates channel-by-channel, which looks torn/inconsistent — update it all at once."*

The naive read is "pick one": publish everything together (fixes torn table,
but couples uptime to the whole channel sweep's duration) or publish
per-channel as you go (keeps uptime fast, but reintroduces the torn table).
**Both requirements can be satisfied simultaneously because they're about
different data with different consistency needs**: system status (uptime,
temp, humidity) has no correctness relationship to the channel table, so it
can publish immediately. Channels have a *table-wide* consistency
relationship (rows are compared against each other visually), so they should
publish as one atomic set. `doPollScan()`'s actual shape:

```cpp
readSystemStatus(data.sysInfo);   // published + repainted immediately
PostEvent();

// channel sweep, staged into a local copy — data.chInfo[] untouched so far
for (ch : channels) {
    if (hasPendingWork()) break;              // let a queued write cut in — see below
    ... readChannelStatus(ch, caps, staging[ch]) ...
    // track consecutive failures per channel (offline detection, §2.7)
}
for (ch : channels) data.chInfo[ch] = staging[ch];  // one atomic publish
PostEvent();
```

**A real regression happened here once already**: a later change (fixing the
torn table) accidentally merged the system-status publish back into the same
single end-of-sweep publish point, silently undoing the earlier
uptime-decoupling fix. The bug was caught only by re-measuring live after the
"fix," not by re-reading the diff — **if you touch this function, re-measure
uptime cadence with a live board afterward, don't just re-read the code and
reason about it.** (See §5 for how to measure it in ~30 seconds.)

`hasPendingWork()` (checked between channels, not just once per loop
iteration) lets a queued write cut into the sweep instead of waiting for all
10 channels — the channel(s) not yet reached that cycle simply get read next
cycle; there's no data loss since this is continuous live polling, not a
one-shot scan.

### 2.6 Write path — narrow, action-specific refresh

`postWrite()` (`tui/widgets.h`) queues a write job and returns immediately —
the UI thread never blocks on I/O. The queued job does the write, then calls
a caller-supplied `refreshFn`. **The refresh function used to be one
monolithic `refreshCh`, re-reading the entire `ChannelConfig` (up to 5
register blocks) after literally every action** — including ones like "Clear
Fault," which only ever changes `ChannelInfo.activeFault`/`faultHistory`
(different struct entirely, no `ChannelConfig` fields involved at all). That
made every click cost ~7 sequential Modbus transactions queued behind
whatever the poll loop happened to be doing.

The fix: a distinct narrow-refresh lambda per action, each re-reading only
the specific block that action's write actually touches (`refreshOutput`,
`refreshProtection`, `refreshOutputEnabled`, `refreshStatus`,
`refreshRecovery`, `refreshDerate`, and a conservative `refreshFull` reserved
for Save/Load/Factory, where the blast radius genuinely is unknown). This
relies entirely on the per-block merge-on-success methods from §1.1/1.4 —
narrow refresh and the merge-on-success contract are the same idea applied
at two different layers (library, then call site).

### 2.7 Offline-channel detection

`readChannelStatus()`/`readSystemStatus()` return `bool` specifically so a
polling caller can count consecutive failures. A channel that fails
`kChannelOfflineThreshold` (5) polls **in a row** — not one, which is
expected noise on real hardware — is marked offline: rendered as an explicit
red "OFFLINE" row (not stale cached values dressed up as live ones), plus a
one-time status-bar notification at the moment it crosses the threshold. A
single subsequent success clears it. This is the same underlying idea as
§1.5's capability self-heal: **don't let "read failed" be silently
indistinguishable from "read succeeded with a boring value."**

### 2.8 One race worth remembering: `data.valid` must reflect user intent, not just the connection object's state

`doDisconnect()` sets `data.valid = false` synchronously on the UI thread.
But the worker thread's poll loop, if already mid-sweep when Disconnect is
clicked, would finish that sweep and then execute
`data.valid = g_client.isConnected();` — and `isConnected()` can still read
`true` at that exact moment, because the queued `g_client.disconnect()` job
hasn't drained yet (it's behind the in-flight sweep on the same worker
thread). Net effect: `data.valid` gets silently reasserted `true` right after
the user explicitly disconnected, and the Monitor table gets stuck showing
the old connection's data indefinitely (nothing else will ever revisit
`data.valid` until the next connect). The `bConnToggle` button itself looked
fine throughout (it reads `g_connected` directly, a plain flag set
synchronously) — only the table content was stuck, which made this
confusing to diagnose from the symptom alone.

**Fix:** gate every `data.valid` assignment on `g_connected.load() &&
g_client.isConnected()`, not `isConnected()` alone — `g_connected` is the
authoritative, synchronously-set user intent, immune to the queued-job
timing race. **General lesson: when a background worker's "am I still
supposed to be doing this?" check reads from an object whose state changes
asynchronously relative to the check, prefer a plain flag the initiating
action sets synchronously over asking the object itself.**

### 2.9 Capability-aware status — one function, not one-per-view

Two different UI surfaces (Monitor's Status button, the Channel tab's Live
panel) each independently computed "is this channel on" from raw status
bits, and they disagreed for output-enable-only channels (no drive
concept) — one path checked `OUTPUT_DRIVE_NONZERO` (meaningless when there's
no drive at all), the other correctly used a capability-aware helper. The
fix was **not** "make the second one match the first" (they'd drift apart
again the next time either was edited) — it was extracting `channelIsOn()`
and `channelStatusBadge()` into `tui/tui_policy.h` as the single shared
implementation, so both call sites use the same logic by construction and
literally cannot express the same field twice with different rules.
**Generalizable rule: any derived value computed from raw device state and
displayed in more than one place belongs in exactly one function, not
copy-pasted-and-adapted.**

---

## 3. Testing methodology that actually found these bugs

Worth recording because most of these findings came from *measuring*, not
from reading code harder:

- **`mbpoll` run completely alone (no other process on the port) is your
  ground truth.** It gave the "healthy" baseline (~32ms/transaction on this
  board/cable) that every "is the TUI slow" question was measured against.
- **Never run `mbpoll`/a second client against the same serial port while
  your own client is also connected.** Two masters on one line corrupt each
  other's traffic — an early round of "comparison testing" produced
  misleadingly slow numbers for *both* tools simply from self-inflicted
  contention. Kill one before measuring the other.
- **`ps -T -p <pid>` (per-thread CPU)** found the busy-wait bug directly — a
  worker thread pinned at ~98% CPU continuously is not subtle once you look.
- **`strace -f -tt -e trace=read,write -o log.txt <cmd>`** (run inside `tmux`,
  not backgrounded directly — FTXUI needs a real PTY) with absolute
  timestamps (`-tt`) is what actually found the inter-byte timeout bug: grep
  for `write(3` (the serial fd) and diff consecutive absolute timestamps.
  Fixed, near-identical gaps across many transactions is a strong signal of
  a configuration constant, not hardware jitter — real hardware failures
  look irregular, not metronomic.
- **Driving an interactive FTXUI TUI from a test script**: launch inside
  `tmux new-session -d -s NAME -x COLS -y ROWS "command"`, poll state with
  `tmux capture-pane -t NAME -p` (add `-e` to keep ANSI codes if you need to
  distinguish styled/focused text from plain). Mouse clicks can be injected
  as raw SGR escape sequences via `tmux send-keys -H <hex bytes>` — but
  **compute the target column by counting Unicode *characters*, not
  bytes**: any row containing box-drawing characters (│╭╮ etc., UTF-8
  multi-byte) will have byte-offset ≠ column-offset, and a byte-based
  `grep -bo` will click the wrong element.
- **Always kill your test session before drawing conclusions from the next
  one.** A `tmux kill-session` doesn't always guarantee the child process
  (and anything it was launched under, e.g. `strace`) actually exits and
  releases the port — check `ps aux` / `fuser <device>` before the next test
  if you get a suspicious "no response from board" on a board you know is
  fine.

---

## 4. Current state of `psb_demo_gui` (Qt/QML) — what already benefits, what still needs porting

`psb_demo_gui`'s `ModbusWorker`/`ModbusBackend` (`tools/psb_demo_app/gui/`)
shares the same `psb_modbus_core` library, so:

- **§1.2 and §1.3 are already fixed for the GUI too, with no GUI-side
  changes needed** — it calls the same `connect()`, and confirmed it also
  passes `timeoutMs = 3000` (`modbus_backend.cpp::connectToDevice()`), so it
  was equally affected by the inter-byte timeout bug before the library fix,
  and is not anymore.
- Threading is Qt-idiomatic and doesn't have the TUI's manual busy-wait risk
  of its own: `ModbusWorker` lives on a dedicated `QThread`
  (`moveToThread()`), driven by a `QTimer` (`m_pollInterval = 1000ms`) firing
  `pollTick()` → `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`. Qt's
  event loop handles the cross-thread handoff; there's no equivalent of the
  TUI's manual condition-variable-and-queue design to get wrong here.
- `doPollStatus()` (the realtime poll path) **already uses the
  merge-on-success `readSystemStatus()`/`readChannelStatus()`** — someone
  already applied §1.1's lesson here, whether independently or by copying
  the TUI's pattern.
- `setFrameCallback()` **is actually wired up** in the GUI (feeds a raw
  tx/rx hex log, `onFrame()`) — better than the TUI, which sets a callback
  that's stored but never invoked (dead code, harmless but worth knowing if
  you go looking for a tracing hook — use the GUI's wiring as the reference
  if you want to add this to the TUI).

**Not yet ported — concrete, worth doing before the GUI is release-ready:**

- `doRefreshSystemInfo()`, `doRefreshChannelInfo()`, and
  `doReadChannelConfig()` still call the **value-returning**
  `readSystemInfo()`/`readChannelInfo()`/`readChannelConfig()`/`readChannelCalConfig()`
  (§1.1). These are connect-time/on-demand refreshes, not the steady polling
  path, but they have the exact same silent-reset-to-default risk on a
  transient failure — the same bug class already fixed in `psb_demo_tui`'s
  `doFullScan` and `psb_demo_cli`'s `monitor`. Recommended fix: switch these
  to the reference-taking overloads, merging into `m_cachedSysInfo`/
  `m_cachedChInfo[ch]` in place rather than reassigning wholesale.
- `doPollStatus()`'s channel loop emits `channelInfoReady(ch, ...)`
  **per-channel, incrementally**, inside the same loop that reads them —
  conceptually the same shape as the TUI's original (fixed) per-channel
  reveal, just expressed as Qt signals instead of FTXUI PostEvents. Whether
  this actually produces a visibly torn QML view depends on how the QML side
  binds to these signals (Qt/QML's property-binding model behaves
  differently from FTXUI's immediate-mode redraw, so it may or may not be
  user-visible in practice) — but it's the same underlying race in the C++
  layer, and worth testing for specifically (rapid-fire updates while
  watching the Monitor table) rather than assuming QML's binding model makes
  it moot.
- No offline-channel detection (§2.7) — `doPollStatus()` doesn't track
  consecutive per-channel failures, so a channel that stops responding will
  silently keep showing its last-known values with no visual distinction
  from a genuinely healthy, freshly-polled one.
- No capability self-heal (§1.5) — if a channel's `chCapFlags` never gets
  captured correctly at connect, nothing in the GUI's poll path will ever
  retry that specific probe.

---

## 5. Checklist for a new client (GUI, TUI, or raw-Modbus integration)

- [ ] Never derive an inter-byte/inter-character serial timeout from an
      overall response timeout — configure them independently (§1.2).
- [ ] Never spin a tight loop on a non-blocking Modbus call without a sleep
      or a real timer/event-loop driving it (§1.3).
- [ ] For any read used in a repeating poll/cache-refresh: use (or add) a
      merge-on-success variant, not a value-returning one (§1.1).
- [ ] Fetch/cache capability flags before any capability-gated batch read,
      and treat `caps == 0` as "unknown, keep retrying," not "no
      capabilities" (§1.4, §1.5).
- [ ] If a write's follow-up refresh re-reads more than the write actually
      touched, narrow it — both for latency and to reduce the surface area
      for the value-returning-reset bug (§2.6).
- [ ] If a table/dashboard has cross-row consistency requirements, stage
      updates into a local copy and publish atomically — but don't
      couple *unrelated* telemetry (e.g. a clock/uptime readout) to that same
      publish point just because it's convenient (§2.5).
- [ ] Any device-state-derived value computed and displayed in more than one
      place lives in exactly one function (§2.9).
- [ ] A background worker's "should I still be doing this?" check should read
      a plain flag the initiating action set synchronously, not infer intent
      from an object whose own state updates asynchronously relative to that
      action (§2.8).
- [ ] After any change to poll cadence, timeouts, or publish timing:
      re-measure live (§3) — these bugs are very easy to "fix" on paper while
      re-breaking something the fix doesn't touch, or to declare fixed
      because a plausible-looking cause was addressed without confirming the
      symptom actually improved.

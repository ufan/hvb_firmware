# Connection Success Requires a Complete Channel Scan — Design

## Context

`board.connected` currently flips `true` immediately after Modbus protocol
verification succeeds — before the channel-discovery scan (`doFullScan`)
even starts. Every UI surface that reads `connected` (the per-board status
dot, the sidebar connection indicator, the toggle button's label, the
Connect/Disconnect gating) therefore shows "connected" while the board is
still mid-scan ("Scanning channels... X/N"). Requested fix: connection
success should only be declared once the board has discovered all its
channels, not merely once the protocol handshake passes.

There are three places in the code that establish a connection and follow
this same "mark connected, then scan" sequence:
1. `board_dashboard.h`'s `doConnect` — the per-board toggle/Connect All path
   (just reworked in the prior round to route through the bus worker queue).
2. `main.cpp`'s `buildRuntime` — the initial connect-all-boards sweep run
   once at launch when `autoConnectAll` is set.
3. `main.cpp`'s `applyNewBoardsLive` — hot-attaching a board mid-session
   (Setup → Add Board, applied while boards are already running).

## Confirmed Decisions

- **Scope**: all three call sites change for consistency — a board counts
  as connected the same way regardless of how it got connected.
- **Success criterion**: reuse `ScannedData::allChannelsLoaded()` verbatim
  (already used by `tab_monitor.h` to decide when the Monitor table reveals
  itself) — no new per-channel-read verification. This already returns
  `false` whenever `numChannels() == 0` (e.g., the initial `readSystemInfo()`
  call failed), so a board that verifies protocol but never successfully
  reports its channel count still correctly counts as not-connected.

## Architecture

**`doFullScan`'s abort signal** (`board_session.h`): the function currently
takes `std::atomic<bool>& connected` and uses it once, partway through, to
detect whether the in-flight scan was invalidated by a concurrent disconnect
(`data.valid = connected.load();`). This only worked because `connected`
was already `true` for a healthy in-progress scan and flipped back to
`false` by `doDisconnect` if the user bailed out mid-scan. Once `connected`
no longer flips `true` until *after* the scan succeeds, that read would
always see `false` and break the check. Fix: change the parameter to
`std::atomic<bool>& abortConnect` — a flag that already exists on
`BoardSession` and already means exactly "this connect attempt should be
treated as cancelled," independent of when `connected` itself changes:

```cpp
inline void doFullScan(PsbBoardSession& client, std::atomic<bool>& abortConnect,
                       ScannedData& data, ftxui::ScreenInteractive& screen,
                       std::atomic<bool>& running) {
    ...
    data.valid = !abortConnect.load();   // was: connected.load()
    ...
}
```

Every call site passes `board.abortConnect` (or the equivalent board
pointer's member) instead of `board.connected`. The two `main.cpp` call
sites have no user-facing way to interrupt their scan (nothing is rendered
yet at initial launch; the hot-attach path isn't abortable either), so
`abortConnect` simply stays `false` there throughout — passing it is a
type-signature match, not a new behavior for those two paths.

**The success gate moves to after the scan**, in all three call sites,
replacing the current "set `connected`, then scan" order:

```cpp
// old (all three sites, same shape):
board.connected = ok && !board.abortConnect;
if (ok) {
    doFullScan(*board.client, board.connected, board.data, screen, running);
    board.data.valid = board.connected.load();
    ...
}

// new:
if (ok) {
    doFullScan(*board.client, board.abortConnect, board.data, screen, running);
}
bool channelsOk = ok && board.data.allChannelsLoaded();
board.connected = channelsOk && !board.abortConnect;
board.data.valid = board.connected.load();
```

`doConnect`'s existing status-message logic (blank on success, `"Error:
..."` otherwise) keys off the same `ok`/success value it already uses, so a
board that verified protocol but discovered zero/incomplete channels now
correctly falls into the error branch instead of silently reporting
success. The two `main.cpp` call sites get the equivalent adjustment to
their own inline status-message logic.

**Unaffected**: `pendingChannelCount`/`pendingSync` (still set right after
`doFullScan`, using whatever `board.data.numChannels()` ends up being —
still correct, just now published alongside a `connected` flag that
actually reflects success). `doDisconnect`, the toggle button, the sidebar
indicator, `Connect All`/`Disconnect All` — none of these need code changes;
they all already just read `board.connected`, and now get a more accurate
signal for free.

## Error Handling

No new error paths — a board that fails the channel scan already had an
error path (the `ok == false` branch); it now also covers "verified but
scan came back empty/incomplete," which previously fell through to a false
"success" (blank status, green dot) despite not actually having usable
data.

## Testing

No automated test surface for this (FTXUI/threading-coupled connect flow,
consistent with this codebase's established practice) beyond confirming
`psb_tests` is unaffected (no `psb_modbus_core` files change). Manual tmux
verification: reconnect a board and confirm the sidebar/dashboard dot stays
non-green through the "Scanning channels..." phase and only turns green
once the Monitor table's channel columns actually appear; spot-check that a
normal connect/disconnect/reconnect cycle still ends up fully connected
with correct channel data. Protecting the real `~/.psb_demo_app/topology.toml`
the same way every prior round did.

## Out of Scope

- Per-channel read-failure tracking inside `doFullScan` itself — channels
  are still marked "loaded" once the sweep reaches them, not individually
  verified. `allChannelsLoaded()` is being reused exactly as it already
  behaves elsewhere, not redefined.
- Any change to `doPollScan` (the ongoing per-cycle poll after a board is
  already connected) — this is purely about the initial connect-to-first-
  successful-scan transition.

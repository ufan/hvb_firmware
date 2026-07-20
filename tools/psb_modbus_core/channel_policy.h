#pragma once

// Capability-aware channel on/off derivation — shared by every client
// (psb_demo_tui, psb_demo_gui, ...) so "is this channel on" can never be
// computed two different ways that disagree with each other. See
// docs/guide/client-architecture-and-pitfalls.md §2.9.

namespace psb {

// A plain OUTPUT_ENABLE_ACTIVE or OUTPUT_DRIVE_NONZERO status bit alone is
// wrong for at least one channel shape each:
//   - OUTPUT_ENABLE + RAW_OUTPUT_DRIVE (e.g. jw_hvb): both the enable gate
//     and the drive value matter — enabled but driving 0 still reads "off".
//   - OUTPUT_ENABLE only (e.g. jw_lvb fixed-voltage channels): no drive
//     concept exists at all, so the enable gate alone decides.
//   - RAW_OUTPUT_DRIVE only (a DAC with no enable gate — locked always-on,
//     still drive-adjustable): on purely reflects the drive value.
//   - Neither: no output capability at all: never on.
inline bool channelIsOn(bool hasOutputEnable, bool hasRawOutputDrive,
                        bool enableActive, bool driveNonzero) {
    if (hasOutputEnable && hasRawOutputDrive) return enableActive && driveNonzero;
    if (hasOutputEnable) return enableActive;
    if (hasRawOutputDrive) return driveNonzero;
    return false;
}

} // namespace psb

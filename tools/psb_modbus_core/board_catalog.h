#pragma once
// Copyright (c) 2026 Jianwei
// SPDX-License-Identifier: Apache-2.0
//
// Host-tool-side lookup table for board variant identity, per
// docs/superpowers/specs/2026-07-19-version-management-contract-design.md
// §4: the wire protocol stays numeric-only (VARIANT_ID, BOARD_HW_REVISION),
// so translating those into human-readable names/labels is a host-tool
// concern, not a protocol concern. Extend this table when a new board
// variant ships — this is the one place that needs updating.

#include <string>

namespace psb::catalog {

// VARIANT_ID -> board name. Matches VC_VARIANT_ID's Kconfig documentation
// (lib/voltage_control/Kconfig: "1 = HVB, 2 = LVB").
inline std::string variantName(int variantId) {
    switch (variantId) {
        case 1: return "jw_hvb";
        case 2: return "jw_lvb";
        default: return "unknown (id=" + std::to_string(variantId) + ")";
    }
}

// VARIANT_ID -> board family label — organizational grouping only (design
// spec §2: "never bumped — it's a label, not a version"), never gates
// behavior.
inline std::string variantFamily(int variantId) {
    switch (variantId) {
        case 1: return "HVB family";
        case 2: return "LVB family";
        default: return "unknown family";
    }
}

// BOARD_HW_REVISION -> human label. No board in this tree declares real
// board.yml revisions yet (every board defaults to index 0 — see
// CONFIG_VC_BOARD_HW_REVISION), so this is a generic 0=rev A/1=rev B/...
// scheme pending real per-variant revision names once board.yml revisions
// exist.
inline std::string hwRevisionLabel(int hwRevision) {
    if (hwRevision >= 0 && hwRevision < 26) {
        return std::string("rev ") + static_cast<char>('A' + hwRevision);
    }
    return "rev #" + std::to_string(hwRevision);
}

} // namespace psb::catalog

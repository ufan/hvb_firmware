# HVB Demo TUI Responsive Polish — Design Spec

**Status:** Approved  
**Date:** 2026-07-01

## Scope

Polish the host-side FTXUI application in `tools/hvb_demo_app/tui/`. This design extends the approved 2026-06-30 TUI design with four behavioral corrections and responsive layout requirements. It does not change firmware or Modbus protocol behavior.

## Connection settings

Before the connection dialog becomes visible, scan the host's serial ports through `HvbModbusClient::scanPorts()`. Use that existing client API rather than duplicating platform-specific enumeration in the TUI.

The Port control is selection-only. Lay out its label, selection control, and `[ Rescan ]` button on one row. Rescan immediately refreshes the choices. Preserve the current selection if it remains available; otherwise select the first result. When no ports are found, show an explicit empty state and prevent Connect from issuing a connection attempt.

The initial scan and user-triggered rescan run synchronously. Local port enumeration is short, and adding another asynchronous state machine is not justified for this host tool.

## Monitor status action

The channel Status button must not issue a Modbus command when all of these are true:

- configured target voltage is zero;
- operational target voltage is zero;
- output drive is off.

This represents an already-off channel with no nonzero target to enable. The button remains visually `[ OFF ]`; clicking it does not enter `[ RAMP ]`, enqueue work, or update the status message.

Existing behavior remains unchanged for a ramp in progress and for channels with a nonzero configured target.

## Channel tab capability behavior

Move the target-voltage input out of Live and into Control. It is the first field on Control's second row, before ramp-up and ramp-down.

Protection Policy exists only when the channel exposes both voltage-measurement and current-measurement capabilities. If either capability is absent, omit the panel and its controls from both rendering and keyboard focus. Recovery uses the vacated right-column space.

## Responsive layout

All tabs expand to the terminal's available width and height. No tab uses a fixed outer width or height.

The supported design baseline is 80 columns by 24 rows. At and above that size:

- panels remain top-aligned;
- flexible elements consume surplus width and height;
- Monitor table columns distribute surplus width;
- System and Channel content expands without stretching labels unpredictably;
- editable fields retain readable minimum widths.

Below 80 by 24, preserving control meaning and focus behavior takes priority over perfect presentation. Clipping is acceptable; controls must not be silently reordered.

The Channel tab uses two balanced, equally expanding columns:

- left column: Control above Setting;
- right column: Protection above Recovery.

Labels within a panel use consistent widths so fields align vertically. Control lays out Vset, Ru, and Rd as aligned fields. Extra vertical space is consumed after the panels, keeping content at the top.

## Internal seams and tests

Extract small policy helpers where necessary so behavior is testable without a live serial device:

- choose the selected port after a scan;
- determine whether a Monitor status click may issue an output action;
- determine whether Protection Policy is available from capability flags.

Tests cover selection preservation/fallback/empty results, the zero-target off guard, normal enable/disable decisions, and every relevant capability combination. Rendering remains verified through build checks and focused manual QA because character-cell geometry is owned by FTXUI.

## Verification

1. Open Connection Settings and confirm the list reflects a scan performed immediately beforehand.
2. Attach or remove a serial device, press Rescan, and confirm choices and selection update correctly.
3. Confirm Connect cannot run with an empty port list.
4. On an off channel with configured and operational targets both zero, click Status and confirm no write or transient RAMP state occurs.
5. Confirm nonzero targets still allow enable and active outputs still allow graceful disable.
6. Confirm Channel Live is read-only and Vset appears before Ru/Rd in Control.
7. Exercise all voltage/current capability combinations and confirm Protection exists only when both are present and no hidden protection control receives focus.
8. Resize each tab from 80×24 upward and confirm it fills available space without ragged alignment or fixed-size islands.
9. Build the TUI with warnings enabled and run the host-side tests.

## Non-goals

- No new serial-port discovery backend.
- No hot-plug monitoring while the dialog is closed.
- No protocol, register-map, firmware, GUI, or CLI changes.
- No redesign of the established group names or behavior.

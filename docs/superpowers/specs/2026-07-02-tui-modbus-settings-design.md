# TUI Modbus Settings Design

**Date**: 2026-07-02
**Status**: Approved

## Summary

Extend the status-bar `Setting` dialog in `tools/hvb_demo_app/tui` with controls
for the device's Next-Boot Modbus Configuration. The controls stage a slave
address and baud rate until the user selects a dedicated `Save Modbus` button.
The existing system `Save` button and its behavior remain unchanged.

## Observable Behavior

The `Setting` dialog displays:

- a slave-address input accepting decimal values from 1 through 247;
- a baud-rate selector containing the protocol-supported values 115200 and 9600;
- a dedicated `Save Modbus` button.

Opening or synchronizing the dialog initializes both controls from the device's
current Modbus configuration register values. Editing either control changes only
the local staged value. It does not perform Modbus I/O.

Selecting `Save Modbus` writes only the fields whose staged values differ from
the last synchronized device values. Slave address is written with the existing
slave-address register command, and baud rate is written with the existing
baud-rate-code register command. The operation does not invoke the system
parameter `Save` action because Modbus configuration-register writes already
persist the Next-Boot Modbus Configuration.

After all required writes succeed, the status-bar message area displays:

`OK: Modbus config saved — takes effect after reset`

The dialog remains open and reset remains an explicit user action through the
existing `Reset` button. The live connection settings are not changed locally.

## Error Handling

Invalid slave-address text or a value outside 1 through 247 is rejected before
any write and produces an `Error:` message in the status bar.

When both fields changed, writes are sequential because the protocol provides
independent registers and no atomic multi-field transaction. Processing stops on
the first failed write, and the existing client error is shown in the status bar.
No success message is shown after a partial failure.

Selecting `Save Modbus` when neither field changed performs no register write and
still reports the successful saved/current state.

## Structure

The dialog uses the existing `ConfigInputs` values synchronized from
`ScannedData::sysCfg`. Staging and dirty-value comparison stay in the TUI layer;
the Modbus client API and firmware register behavior remain unchanged. All I/O is
submitted through the existing serialized TUI worker queue.

## Tests

Host-side tests cover:

- device values populate the staged Modbus controls;
- editing a control does not write immediately;
- `Save Modbus` writes only a changed slave address;
- `Save Modbus` writes only a changed baud-rate code;
- invalid slave input is rejected without I/O;
- a write failure produces an error and suppresses the reset-required success
  message;
- successful save produces the reset-required status-bar message;
- the existing system `Save` action remains independent.

## Out of Scope

- Changing firmware register semantics or supported baud rates.
- Automatically resetting or reconnecting the device.
- Updating the host connection preferences from device configuration.
- Making the two independent configuration-register writes atomic.

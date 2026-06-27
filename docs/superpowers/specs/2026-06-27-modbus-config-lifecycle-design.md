# Modbus Configuration Lifecycle and Reset Design

**Date**: 2026-06-27
**Status**: Approved

## Summary

Give the technician-facing shell and end-user-facing Modbus interface distinct,
explicit configuration policies. Shell changes affect the live Modbus server
immediately and remain volatile until the technician saves them. Modbus register
writes persist a next-boot configuration immediately but never alter the live
connection; a software reset or power cycle is required before they take effect.

System reset requests must allow the initiating shell command or Modbus request
to complete before the board reboots. Factory defaults come from
`CONFIG_VC_MODBUS_UNIT_ID` and `CONFIG_VC_MODBUS_BAUD_RATE` and are stored as a
concrete settings value rather than represented by a missing settings key.

## Vocabulary

- **Active Modbus Configuration**: the slave address and baud rate currently used
  by the live Modbus server.
- **Next-Boot Modbus Configuration**: the validated slave address and baud rate
  that will become active after the next software reset or power cycle. When
  settings are enabled, this is the persisted configuration and the value exposed
  by Modbus holding-register readback.
- **Compiled Modbus Defaults**: the product defaults derived from
  `CONFIG_VC_MODBUS_UNIT_ID` and `CONFIG_VC_MODBUS_BAUD_RATE`.

## Required Behavior

### Startup

When settings are enabled, startup loads and validates the stored Modbus
configuration before starting the server. A valid stored value becomes both the
Active and Next-Boot Modbus Configuration. If no valid stored value exists, the
firmware stores the Compiled Modbus Defaults and uses them for both configurations.

When settings are disabled, startup uses the Compiled Modbus Defaults. There is no
persistent Next-Boot Modbus Configuration.

### Technician Shell

The `mb set slave` and `mb set baud` commands modify the Active Modbus
Configuration and reconfigure the live server immediately. These changes are
volatile until `mb save` succeeds.

With settings enabled:

- `mb save` stores the complete Active Modbus Configuration and makes it the
  Next-Boot Modbus Configuration.
- `mb load` loads the stored Next-Boot Modbus Configuration and applies it to the
  live server immediately.
- `mb factory` stores the Compiled Modbus Defaults and applies them to the live
  server immediately.

With settings disabled:

- `mb set` remains available for temporary debugging and maintenance.
- `mb save` and `mb load` report that persistence is unavailable.
- `mb factory` immediately restores the Compiled Modbus Defaults; the result is
  necessarily volatile but matches what the next boot will use.

`mb status` shows the Active Modbus Configuration. When settings are enabled, it
also shows the Next-Boot Modbus Configuration and clearly marks a pending restart
when the two differ.

### End-User Modbus Interface

With settings enabled, valid FC06 writes to the slave-address or baud-rate holding
register persist the complete updated Next-Boot Modbus Configuration before
returning success. The Active Modbus Configuration is unchanged. FC03 readback
returns the Next-Boot value so the client can verify exactly what will apply after
restart.

Each Modbus write is independently persistent; the client does not need a separate
save operation. A persistence failure returns Modbus device failure and leaves the
previous Next-Boot Configuration intact.

For adapter configuration, the Modbus system parameter actions behave as follows:

- save is a no-op because each valid configuration-register write is already
  persistent;
- load refreshes the Next-Boot Modbus Configuration from storage without changing
  the Active Modbus Configuration;
- factory reset stores the Compiled Modbus Defaults without changing the Active
  Modbus Configuration;
- system reset acknowledges the request and then activates the stored values by
  rebooting.

The Modbus system factory-reset action stores the Compiled Modbus Defaults as the
Next-Boot Modbus Configuration without changing the live server. The defaults take
effect only after software reset or power cycle.

With settings disabled, the slave-address and baud-rate holding registers remain
readable as the compiled values but reject writes with Modbus illegal address.
This makes the interface explicitly immutable while preserving configuration
discovery.

### System Reset Ownership

Reset is a system-level operation and is not part of the voltage-control domain.
The `sys_status` module exposes the system-reset request API and owns a statically
allocated Zephyr delayed-work item that performs a cold reboot. The delay is a
small, Kconfig-defined interval long enough for shell output or a Modbus response
to be transmitted. Repeated requests before execution reschedule the same static
work item; they do not allocate additional work objects.

The technician command is `ss reset`. The existing `ss` command continues to print
system status. The legacy `vc reset` and `vc sys reset` commands are removed rather
than retained as aliases.

The Modbus Adapter handles system parameter action value `255` directly through
the system-reset API after persisting any Next-Boot Modbus Configuration. It does
not route reset through `vc_dispatch()`. Channel parameter action value `255` is
rejected because reset has no channel scope. The software-reset member is removed
from the voltage-control parameter-action type and its controller handlers.

To avoid coupling reset availability to heartbeat or environmental-sensor
hardware, reset support is a separately selectable source within the
`lib/sys_status/` module. The product application enables it statically. A build
without reboot support rejects reset instead of reporting success without
rebooting.

## Internal Model

The Modbus adapter owns two fixed-size configuration snapshots:

- `active_cfg` for the live server;
- `boot_cfg` for persisted next-boot intent and Modbus readback.

Keeping both values explicit avoids overloading one structure with source-dependent
meaning. No dynamic allocation or runtime registration is introduced.

Configuration persistence writes a complete validated structure. State changes
are committed in memory only after the settings write succeeds, preventing FC03
from reporting a value that was not persisted.

## Validation and Defaults

Slave addresses are valid from 1 through 247. Supported baud-rate codes remain the
declared `enum vc_baud_rate_code` values. The configured baud rate in hertz is
converted to its protocol code when constructing the Compiled Modbus Defaults; an
unsupported build-time baud rate is rejected by Kconfig or the build.

Loading malformed, unsupported, or wrong-sized settings falls back to the Compiled
Modbus Defaults and replaces the invalid stored value.

## Error Handling

- Invalid shell input returns `-EINVAL` without changing either configuration.
- Live server reconfiguration failure returns an error and preserves the prior
  Active Modbus Configuration where the Zephyr API permits rollback.
- Settings save/load failure is reported to the shell and maps to Modbus device
  failure.
- Modbus configuration writes with settings disabled return illegal address.
- A build without reboot support rejects system-reset commands through the
  existing domain-to-frontend error mapping.

## Build-Hygiene Fixes

The baud-code-to-rate conversion helper is only needed by the live Modbus server
and is compiled only when `CONFIG_MODBUS` is enabled. This removes the existing
unused-function failure in native Twister builds with warnings treated as errors.

The missing `abs()` declaration in `sys_status_shell.c` was already corrected by
commit `6efc335`; no further change is required for that finding.

## Tests

Native Zephyr tests cover:

- settings-enabled startup from stored configuration and fallback to compiled
  defaults;
- shell live changes, explicit save, load, factory reset, and status output;
- Modbus writes updating persisted/FC03 values without changing active values;
- automatic persistence of each valid Modbus write and rollback on save failure;
- Modbus factory reset staging and persisting compiled defaults;
- settings-disabled Modbus write rejection and volatile shell changes;
- `ss reset` registration and removal of both legacy `vc` reset commands;
- direct Modbus system-reset scheduling and channel reset rejection;
- delayed system-reset scheduling without executing an inline reboot;
- default Twister builds with `-Werror` when Modbus is disabled.

The `hvb_controller` application is clean-built for `jw_hvb`, and the existing
voltage-control, Modbus-adapter, and shell suites are rerun.

## Out of Scope

- Changing the Modbus register layout or protocol version.
- Adding new baud-rate choices.
- Dynamic allocation or runtime registration of configuration providers.
- Persisting arbitrary shell changes without an explicit `mb save`.
- Retaining deprecated voltage-control reset aliases.

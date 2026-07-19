# HVB Firmware

Zephyr RTOS firmware workspace for Jianwei high-voltage board products and bring-up demos.

This repository is the west manifest/application repository in a Zephyr T2-style workspace. Zephyr and imported modules are managed by `west.yml`; product applications, demos, board definitions, devicetree bindings, custom drivers, and shared firmware libraries live in this repository.

## Layout

```text
applications/   Product-level firmware applications
demos/          Temporary bring-up and development applications
boards/         Shared out-of-tree Zephyr board definitions
dts/bindings/   Shared custom devicetree bindings
drivers/        Shared custom Zephyr drivers
include/        Public headers for shared firmware code
lib/            Shared firmware libraries
scripts/        Development and flashing helpers
doc/            Datasheets, schematics, and generated design documents
ref/            Human-maintained design references and interface notes
zephyr/         Zephyr module metadata for this repository
```

## Workspace Setup

From the west workspace root, initialize and update this manifest repository in the usual Zephyr workflow:

```sh
west init -l hvb_firmware.git
west update
```

## Builds

Build the product firmware skeleton (board-agnostic — works for any
`jianwei,vc-controller`-based board, e.g. `jw_hvb` or `jw_lvb`):

```sh
west build -b jw_hvb applications/psb_controller
west build -b jw_lvb applications/psb_controller
```

Build the current Modbus RTU smoke demo:

```sh
west build -b jw_hvb demos/modbus_smoke
```

Build the Modbus RTU simulator used for host-tool and new-board bring-up:

```sh
west build -b jw_hvb demos/modbus_sim
```

Use a separate build directory when switching between applications:

```sh
west build -b jw_hvb -d build/psb_controller applications/psb_controller
west build -b jw_hvb -d build/modbus_smoke demos/modbus_smoke
```

### Board SKU / channel-count overlays

Some boards ship in multiple channel-count SKUs from the same DTS base —
e.g. jw_hvb's 2-channel config is the default; a 1-channel BOM population
uses an explicit overlay:

```sh
west build -b jw_hvb applications/psb_controller \
    -- -DEXTRA_DTC_OVERLAY_FILE=boards/jianwei/jw_hvb/jw_hvb_1ch.overlay
```

Channel count is a devicetree build parameter, not a board variant or
hardware revision — see
`docs/superpowers/specs/2026-07-19-version-management-contract-design.md`
§5 for the full rationale and the mechanical test for when a hardware
change needs a new overlay vs. a new board name vs. a new
`board.yml` revision.

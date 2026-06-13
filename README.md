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

Build the product firmware skeleton:

```sh
west build -b jw_hvb applications/hvb_controller
```

Build the current Modbus RTU smoke demo:

```sh
west build -b jw_hvb demos/modbus_smoke
```

Use a separate build directory when switching between applications:

```sh
west build -b jw_hvb -d build/hvb_controller applications/hvb_controller
west build -b jw_hvb -d build/modbus_smoke demos/modbus_smoke
```

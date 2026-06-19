# AGENTS.md

Zephyr RTOS firmware for Jianwei Voltage Board Family. This repo is a west manifest/application repository in a T2-style workspace.

## Guide
- The firmware is to be deployed to resource constraint embedded system, keep this in mind when choosing implementation technology.
- Except for `tools/`, treat this repository as Zephyr-based firmware: code, build structure, board integration, and project composition should follow Zephyr conventions.
- Prefer Zephyr-standard APIs, services, data structures, devicetree, Kconfig, device model, kernel primitives, settings, shell, and protocol facilities where they fit.
- Use devicetree and Kconfig for board variants, feature selection, and build-time composition instead of hard-coded product assumptions.
- Prefer Zephyr static build-time composition over runtime wiring. Move runtime coordination problems into devicetree, Kconfig, static kernel objects, device instances, iterable sections, and linker-defined collections when that keeps the solution simpler and safer.
- For cross-layer seams, prefer statically declared buses, queues, slots, descriptors, and iterable-section records over runtime pointer assignment or dynamic registration, unless runtime lifetime or hot-plug behavior is explicitly required.
- Domain and runtime channel topology must be collected from static build-time sources such as devicetree, Kconfig, device instances, and iterable-section records. Avoid runtime channel wiring, runtime pointer patch-up, and magic macro numbers when topology can be declared statically.
- Keep low-level chip drivers hardware-shaped and policy-free.
- Put product behavior above low-level drivers.
- Keep frontend/protocol translation separate from product behavior.
- Use PRDs and design specs to define observable behavior, not private implementation details.
- Use tests to verify public behavior and stable seams, not private helper functions.

## Spec-Driven Development Workflow

This project follows a spec-driven process. Do not jump from ref docs straight into implementation. Use this sequence:

1. Clarify behavior against `ref/` docs and resolve ambiguities
2. Define domain vocabulary and sync it with `UBIQUITOUS_LANGUAGE.md`
3. Design interfaces and compare alternatives
4. Write design spec / PRD, get approval
5. Split into vertical slices
6. Write implementation plan
7. Implement test-first with Zephyr test targets where hardware is not needed
8. Verify before claiming completion

## Design Sources

- `UBIQUITOUS_LANGUAGE.md` defines canonical terminology.
- `CONTEXT.md` captures current project context and domain notes.
- `docs/superpowers/specs/` contains approved design direction.
- `docs/superpowers/plans/` contains implementation plans derived from specs.
- `ref/` contains authoritative reference behavior and external design inputs.

## Code Conventions

- Zephyr includes use `<zephyr/...>` paths
- SPDX header: `SPDX-License-Identifier: Apache-2.0` (no extra commentary)
- Copyright: `Copyright (c) 2026 Jianwei`
- `CONFIG_*` symbols go in `prj.conf` per application
- Board-level defaults in `boards/jianwei/jw_hvb/jw_hvb_defconfig` and `Kconfig.defconfig`
- Custom devicetree bindings go in `dts/bindings/`
- `zephyr/module.yml` sets `board_root: .` and `dts_root: .` — boards and bindings are discovered from repo root

## Module Layout

```
applications/     Product firmware apps (each has CMakeLists.txt, prj.conf, src/)
demos/            Bring-up demos (same structure)
boards/jianwei/   Out-of-tree board definitions
dts/bindings/     Custom devicetree bindings
drivers/          Custom Zephyr drivers (add CMakeLists.txt + Kconfig when populated)
lib/              Shared firmware libraries (add CMakeLists.txt + Kconfig when populated)
include/          Public headers for shared code
ref/              Authoritative reference behavior and external design inputs
tests/
docs/
tools/
```

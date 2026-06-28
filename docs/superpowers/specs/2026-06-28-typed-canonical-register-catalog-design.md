# Typed Canonical Register Catalog Design

Date: 2026-06-28
Status: Approved

## Decision

Firmware modules own their typed canonical state. A global, protocol-neutral
Register Catalog exposes selected state through 32-bit Semantic Register IDs
encoded as module, instance, and field. Static ROM descriptors are collected by
Zephyr iterable sections. There is no dense input/holding mirror and no
periodically copied Domain Snapshot.

This decision supersedes the coherent published-snapshot read mechanism in the
earlier runtime architecture specifications. Presentation snapshots may still be
assembled on demand, but they are compatibility helpers rather than canonical
storage and do not promise one generation across fields.

## Ownership and access

- Hardware providers own synchronized raw evidence buffers.
- VC owns calibrated measurements, configuration, runtime state, and policy.
- `sys_status` owns environment telemetry and uptime; firmware identity is ROM.
- Fixed registers reference ROM and allocate no mutable mirror.
- Descriptors either reference safely accessible canonical storage directly or
  delegate scalar access to the owning module for synchronization and policy.
- Config and runtime writes are synchronously routed to the owning module.
- Command registers are write-only operations and retain no command value.
- Rejected writes do not modify canonical state.
- Private execution state is not registered merely because it exists.

All frontend adapters use the Register Catalog accessors. Module policy code
uses its native typed state directly and does not perform semantic-ID lookups in
hot paths.

## Static composition

Shared host-safe X-macro schemas define semantic fields and Modbus placement.
The firmware expands them into Semantic Register IDs and wire views;
`reg_map.h` remains usable by generic host tools without Zephyr dependencies.
Devicetree determines instantiated channels and their capabilities. The fixed
Modbus v3 view retains 16 channel slots and reports unsupported access for slots
not present in a given build.

## Coherence

Each catalog read returns one coherent scalar value. Reads of separate values
may observe different generations. Raw value/timestamp pairs are protected by
their provider buffer lock for domain consumption. A normal ascending Modbus
read of a 32-bit scalar latches the high-word value for the immediately
following low word. Multi-register writes are sequential and have no rollback.

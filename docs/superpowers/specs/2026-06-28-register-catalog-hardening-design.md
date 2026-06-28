# Register Catalog Hardening Design

Date: 2026-06-28
Status: Approved direction; pending implementation

## Goal

Harden the owner-native Register Catalog integration without changing the
Modbus v3 wire map or reopening the canonical-state architecture. The work
closes lifecycle, capability, synchronization, composition, documentation,
and verification gaps found by the post-merge review.

## Runtime lifecycle

The VC catalog owner has an explicit availability lifecycle. Descriptor reads
and writes return `REG_BUSY` unless the runtime is fully initialized and able
to service commands. The runtime publishes itself only after its queue and
worker are ready, rejects a second concurrent initialization, and unpublishes
itself before shutdown prevents further command processing.

Shutdown must not leave a catalog writer waiting for a stopped worker. Runtime
availability and shutdown state use Zephyr synchronization primitives; a plain
cross-thread Boolean is not sufficient. Tests cover catalog reads and writes
before initialization, while running, and after destruction, plus repeated
initialization.

## Static channel handles and topology

The VC owner exports a stable function that resolves a configured channel and
register ordinal to a descriptor handle in constant time. The Modbus Register
View uses that function rather than naming the private descriptor array or
repeating its layout arithmetic.

Descriptor storage remains a const iterable-section array generated from
devicetree and schema definitions. Compile-time assertions require configured
channel instances to be contiguous from zero and no greater than the protocol
limit of 16. This makes the existing ordinal-major representation explicit and
prevents a DTS ordering change from silently corrupting adapter mappings.

No RAM handle index, runtime registration, dynamic allocation, or copied value
mirror is introduced.

## Capability-aware shell reads

The VC shell reads channel capability flags first. It fetches only registers
supported by those capabilities and treats unexpected catalog failures as
command failures instead of printing partially initialized presentation data.
Configuration and calibration displays follow the same rule.

Presentation snapshots remain temporary stack objects assembled on demand.
They are not canonical storage and do not provide multi-field generation
coherence.

## Modbus adapter synchronization

The adapter's existing mutex serializes all active and next-boot state changes,
settings reads and writes, and live-server reconfiguration. Settings load and
factory paths build and validate candidates while holding that mutex before
publishing state. No adapter worker thread is added.

The lock boundary remains internal to the owner callback. Register Catalog
callers receive the existing status and Modbus exception behavior.

## Interface and composition cleanup

The removed `vc_dispatch`/`vc_query` facade is completed by deleting public
`vc_cmd` value types and builders that no longer have a public dispatcher. The
shell maps parsed intent directly to Semantic Register IDs. Runtime command
types remain private to the runtime worker.

`UI_MODBUS_RTU` requires the VC runtime and DTS channel controller because its
v3 Register View includes VC-owned registers. Invalid build combinations are
rejected by Kconfig rather than at link time. Tracked architecture diagrams
are updated to show catalog reads, owner-mediated writes, and canonical state.
New register schema files receive the repository-required copyright header.

## Efficiency follow-up included in scope

Compile-time VC global values—variant ID, capability flags, supported-channel
count, and active-channel mask—bind directly to const scalar storage. The
runtime callback remains for mutable, computed, enum, and command fields. This
removes avoidable callback switch work and should recover some text growth
without adding RAM.

The system Modbus view remains generated from `modbus_view.def`; owner-specific
handle expressions are centralized so the wire map and descriptor mapping do
not drift into separate handwritten tables.

## Verification

Tests are added before production changes and must demonstrate their intended
failure. Coverage includes:

- catalog operations before initialization and after VC destruction;
- rejection of concurrent singleton initialization;
- partial-capability shell status, configuration, and calibration output;
- channel 15 reads and writes through the handle-backed Modbus view;
- adapter load/save/factory serialization seams;
- Kconfig dependency behavior and descriptor-ID uniqueness;
- unchanged Modbus v3 addresses and exception mapping.

Final verification runs all voltage-control Twister scenarios, architecture
checks, host protocol tests, clean product and simulator builds, and
`git diff --check`. Product image size is compared with commit `2c059b9`.

## Non-goals

- Changing Semantic Register IDs or the Modbus v3 wire map.
- Adding a general runtime registry or RAM lookup table.
- Providing multi-register transaction coherence.
- Redesigning VC policy, persistence semantics, or hardware providers.
- Adding new frontend protocols.

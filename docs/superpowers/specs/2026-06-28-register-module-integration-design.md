# Register Module Integration Design

Date: 2026-06-28
Status: Approved

## Decision

The firmware-wide Register Catalog is the only frontend-facing state interface.
Each firmware module declares its own typed Semantic Registers and retains
ownership of the canonical state behind them. The Modbus Register View composes
registers from multiple owners without changing the external v3 address map.

Semantic module IDs identify the actual owner: system status, voltage control,
and the Modbus adapter. Voltage-control module state uses a reserved global
instance and channel instances 0 through 15. Module schemas contain no Modbus
addresses; adapter-specific view definitions provide those mappings.

## Storage and access

- Descriptors bind directly to statically addressable canonical scalar fields
  when their representation and synchronization permit it.
- Owner callbacks handle locking, atomics, computed values, enum conversion,
  validation, persistence, and commands.
- Generated Register Views hold descriptor handles and do not perform a linear
  catalog search for every frontend read or write.
- Generic Semantic Register ID lookup remains available for diagnostics and
  infrequent access and does not allocate a RAM index.
- VC mutations remain serialized by the VC worker. Single-field writes validate
  and mutate one field without copying the complete configuration structure.
- The Modbus adapter uses one mutex to serialize its active and next-boot state,
  persistence, and live-server reconfiguration; it does not add a worker thread.
- System telemetry remains atomic. System reset remains delayed work and is
  exposed as a write-only system-status command.

## Frontends and compatibility

The Modbus adapter maps catalog status directly to Modbus exceptions and has no
VC context dependency. Its Active Modbus Configuration and Next-Boot Modbus
Configuration are distinct semantic fields. The existing source-specific
lifecycle remains unchanged: shell writes affect live state, while Modbus wire
writes stage and persist next-boot state.

VC, system-status, and Modbus shells read and write scalar catalog entries.
Presentation snapshot/query APIs and aggregate adapter getters are removed after
all in-repository callers migrate. Complete structures remain only at settings
serialization boundaries and private policy interfaces where the whole object
is the operation.

## Constraints

- Keep the Modbus v3 wire map and observable validation behavior unchanged.
- Keep per-scalar coherence; do not promise multi-field generation coherence.
- Keep raw measurement value/timestamp buffers as synchronized hardware evidence.
- Use devicetree, Kconfig, static objects, and iterable sections only; no dynamic
  registration, pointer patch-up, dense mirrors, or per-register RAM index.
- Support 16 statically composed channels and future frontend adapters.

## Verification

Tests cover owner namespaces, descriptor handles, direct bindings, catalog
commands, VC serialization and validation, Modbus configuration lifecycle,
system reset, unchanged wire addresses, unsupported capabilities, 32-bit word
latching, 16-channel composition, all shells, and the simulator. Final builds
compare RAM/ROM maps and reject reintroduced mirrors or per-channel overhead.

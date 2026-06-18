# Virtual Voltage Channel Provider Design

Date: 2026-06-19
Status: Draft for review

## Purpose

Define the Zephyr-native Virtual Voltage Channel Provider boundary: DTS bindings, shared provider API, controller aggregation pattern, and domain integration. This design replaces the hand-written `struct vc_variant_profile` in `hvb_variant.c` with devicetree-only channel composition and Zephyr device-model provider registration.

## Design Goals

- Channels are Zephyr devices with DTS-derived static config and `struct vc_channel_api` callbacks.
- Multi-compatible channels on one board: each channel node can have a different `compatible` string, inheriting a shared base binding for common properties.
- A voltage controller DTS node aggregates channels by phandle array so the domain can iterate channels in index order regardless of their concrete `compatible`.
- Eliminate `hvb_variant.c` as a hand-written C profile. Capabilities, limits, and defaults come from DTS/Kconfig.
- All providers implement `struct vc_channel_api`. The domain never calls `sensor_sample_fetch()`, `dac_write_value()`, or `gpio_pin_set()` directly.
- Follow Zephyr conventions: `DT_DRV_COMPAT`, `DEVICE_DT_INST_DEFINE`, `POST_KERNEL` init, GPIO phandle specs, SPI/I2C device phandles.

## DTS Layout

### Board DTS Example (jw_hvb)

```dts
// jw_hvb.dts

vc_controller: vc-controller {
    compatible = "jianwei,vc-controller";
    channels = <&vc_ch0 &vc_ch1>;
};

vc_ch0: vc-channel {
    compatible = "jianwei,hvb-vc-channel";
    channel-index = <0>;
    capabilities = <(CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE |
                     CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT)>;
    dac = <&ad5541_0>;
    adc = <&ads1232_hv1>;
    enable-gpios = <&gpioh 12 GPIO_ACTIVE_LOW>;
    interlock-gpios = <&gpioe 10 GPIO_ACTIVE_HIGH>;
    max-raw-dac = <0xFFFF>;
    sample-rate-ms = <100>;
};

vc_ch1: vc-channel {
    compatible = "jianwei,hvb-vc-channel";
    channel-index = <1>;
    capabilities = <(CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE |
                     CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT)>;
    dac = <&ad5541_1>;
    adc = <&ads1232_hv2>;
    enable-gpios = <&gpioh 13 GPIO_ACTIVE_LOW>;
    interlock-gpios = <&gpioc 4 GPIO_ACTIVE_HIGH>;
    max-raw-dac = <0xFFFF>;
    sample-rate-ms = <100>;
};
```

### Multi-compatible Board Example (future)

```dts
vc_controller: vc-controller {
    compatible = "jianwei,vc-controller";
    channels = <&vc_ch0 &vc_ch1 &vc_ch2>;
};

vc_ch0: vc-channel {
    compatible = "jianwei,hvb-vc-channel";    // GPIO + AD5541 + ADS1232
    channel-index = <0>;
    capabilities = <(CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE |
                     CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT)>;
    dac = <&ad5541_0>;
    adc = <&ads1232_0>;
    enable-gpios = <...>;
};

vc_ch1: vc-channel {
    compatible = "jianwei,smart-vc-channel";   // smart module over SPI
    channel-index = <1>;
    capabilities = <(CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE)>;
    spi-dev = <&spi_module>;
    enable-gpios = <...>;
};

vc_ch2: vc-channel {
    compatible = "jianwei,onoff-vc-channel";   // on/off only
    channel-index = <2>;
    capabilities = <(CH_CAP_OUTPUT_ENABLE)>;
    enable-gpios = <...>;
};
```

## DTS Bindings

### Base Binding (`jianwei,vc-channel-base.yaml`)

```yaml
# dts/bindings/voltage_control/jianwei,vc-channel-base.yaml
include: base.yaml

properties:
  channel-index:
    type: int
    required: true
    description: |
      Zero-based channel index. Must match the position in the
      controller's channels phandle array.

  capabilities:
    type: int
    required: true
    description: |
      Bitmask of CH_CAP_* flags from the shared register map header.
      Static board-design facts; not runtime-selectable.

  enable-gpios:
    type: phandle-array
    required: true
    description: |
      GPIO that gates channel output. Mandatory because on/off is the
      minimum virtual channel capability.
```

### HVB Provider Binding (`jianwei,hvb-vc-channel.yaml`)

```yaml
# dts/bindings/voltage_control/jianwei,hvb-vc-channel.yaml
include: jianwei,vc-channel-base.yaml

properties:
  dac:
    type: phandle
    required: true
    description: Phandle to the DAC device for raw output drive.

  adc:
    type: phandle
    required: true
    description: Phandle to the ADC device for voltage/current measurement.

  interlock-gpios:
    type: phandle-array
    required: false
    description: GPIO that reports hardware interlock status.

  max-raw-dac:
    type: int
    required: true
    default: 0xFFFF
    description: Maximum raw DAC code for this channel.

  sample-rate-ms:
    type: int
    required: false
    default: 100
    description: Target measurement sampling period in milliseconds.
```

### Smart Module Provider Binding (`jianwei,smart-vc-channel.yaml`)

```yaml
# dts/bindings/voltage_control/jianwei,smart-vc-channel.yaml
include: jianwei,vc-channel-base.yaml

properties:
  spi-dev:
    type: phandle
    required: true
    description: Phandle to the SPI device for communication.

  spi-command-address:
    type: int
    required: true
    description: Module command register address for output control.
```

### On/Off Provider Binding (`jianwei,onoff-vc-channel.yaml`)

```yaml
# dts/bindings/voltage_control/jianwei,onoff-vc-channel.yaml
include: jianwei,vc-channel-base.yaml

# No additional properties. enable-gpios from base is sufficient.
```

### Controller Binding (`jianwei,vc-controller.yaml`)

```yaml
# dts/bindings/voltage_control/jianwei,vc-controller.yaml
include: base.yaml

properties:
  channels:
    type: phandle-array
    required: true
    description: |
      Ordered list of VC channel device phandles. The order defines
      the channel index domain-side; each channel node's channel-index
      property must match its position in this array.
```

## Shared Provider API

```c
// include/voltage_control/vc_channel.h

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>

struct vc_channel_api {
    /** Set the raw output drive code.  Valid only if the channel
     *  advertises CH_CAP_RAW_OUTPUT_DRIVE. */
    int (*set_output)(const struct device *dev, uint16_t code);

    /** Enable or disable the channel output gate.  Mandatory. */
    int (*set_enable)(const struct device *dev, bool enable);

    /** Capture raw voltage.  Valid only if CH_CAP_VOLTAGE_MEASUREMENT
     *  is present.  Returns 0 on success. */
    int (*measure_voltage)(const struct device *dev, int32_t *value);

    /** Capture raw current.  Valid only if CH_CAP_CURRENT_MEASUREMENT
     *  is present.  Returns 0 on success. */
    int (*measure_current)(const struct device *dev, int32_t *value);

    /** Return the static capability bitmask for this channel.
     *  Implementations read from DTS config; domain caches on init. */
    uint16_t (*get_capabilities)(const struct device *dev);
};
```

The domain calls through `const struct device *dev` using `const struct vc_channel_api *api = (const void *)dev->api`.

A provider may leave optional callbacks as `NULL`. The domain checks capability flags before calling; a `NULL` callback for a capability the flags do not claim is not an error. If the flags claim a capability but the provider leaves the callback `NULL`, that is a provider bug.

## Controller Aggregation

The voltage controller is a Zephyr device with `compatible = "jianwei,vc-controller"`. Its DTS `channels` phandle-array lists all VC channels in order.

### Compile-time Table

```c
// In the controller's implementation
#define CH_ENTRY(node_id, prop, idx) \
    [idx] = { \
        .dev = DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, idx)), \
        .index = DT_PROP(DT_PHANDLE_BY_IDX(node_id, prop, idx), \
                         channel_index), \
        .capabilities = DT_PROP(DT_PHANDLE_BY_IDX(node_id, prop, idx), \
                                capabilities), \
    },

#define FOR_EACH_CH(n, p) \
    DT_FOREACH_PROP_ELEM(DT_NODELABEL(n), p, CH_ENTRY)

#define VC_CONTROLLER_NODE DT_NODELABEL(vc_controller)

static const struct vc_channel_entry {
    const struct device *dev;
    uint8_t            index;
    uint16_t           capabilities;
} domain_channels[] = {
    FOR_EACH_CH(vc_controller, channels)
};
```

The table is a compile-time constant. Channel count is `ARRAY_SIZE(domain_channels)`. The domain receives a pointer to this table at startup.

### Domain Integration

The domain initializer changes from `domain_create(variant_profile)` to `domain_create(domain_channels, ARRAY_SIZE(domain_channels))`. The hand-written variant profile struct is removed.

`domain_create` stores the channel entry table internally. Capability gating, output commands, and measurement reads all go through `ch->dev` + `ch->dev->api`.

System-level capabilities (Automatic Mode, Calibration Mode, environment sensor) remain in Kconfig or a system DTS node, not in per-channel entries. Channel-level capabilities are per-entry.

## Migration Path

Current state:

- `hvb_variant.c` defines `struct vc_variant_profile hvb_profile`.
- `domain_create(&hvb_profile)` initializes domain state from the struct.

Target state:

- `jw_hvb.dts` defines `vc-controller` + per-channel nodes.
- `domain_create(domain_channels, count)` uses the aggregated table.
- No `hvb_variant.c` or `struct vc_variant_profile`.

Migration order:

1. Define DTS bindings and controller + channel nodes in `jw_hvb.dts`. Initially mark channel nodes `status = "disabled"` or build behind a Kconfig guard.
2. Implement the HVB VC channel provider driver behind `DT_DRV_COMPAT jianwei_hvb_vc_channel`. It wraps the existing AD5541 (DAC API) and ADS1232 (ADC API) devices.
3. Implement the controller aggregation logic.
4. Switch domain initialization to consume the aggregated table.
5. Remove `hvb_variant.c` and `struct vc_variant_profile`.
6. Remove the static `CH_CAP_*` constant definitions from `hvb_variant.c` (they remain in `hvb_regs.h` as the protocol dictionary).

During migration, tests can create a fake channel table for domain tests without any hardware or DTS dependencies.

## Capability Flags

### Capability Constants in DTS

The `hvb_regs.h` capability bit definitions are the protocol-authoritative single source of truth. DTS files cannot include C headers directly, but two options exist:

**Option A (recommended):** Define the constants in a Zephyr DTS binding header include.

```yaml
# dts/bindings/include/jianwei,vc-channel-capabilities.h
#define CH_CAP_OUTPUT_ENABLE           0x0001
#define CH_CAP_RAW_OUTPUT_DRIVE        0x0002
#define CH_CAP_VOLTAGE_MEASUREMENT     0x0004
#define CH_CAP_CURRENT_MEASUREMENT     0x0008
#define CH_CAP_HARDWARE_STATUS         0x0010
```

Board DTS files include this header. The same values are repeated in `hvb_regs.h` for the shared protocol dictionary. A build-time CI check ensures the two copies stay synchronized.

**Option B:** Use raw hex literals in DTS with inline comments referencing the bit names.

```dts
capabilities = <(0x0001 | 0x0002 | 0x0004 | 0x0008)>;
/* CH_CAP_OUTPUT_ENABLE | RAW_OUTPUT_DRIVE | VOLTAGE_MEASUREMENT | CURRENT_MEASUREMENT */
```

Option A is preferred because it gives host tools and DTS readers named constants. The implementation plan resolves which option is used and adds the synchronization check if needed.

## Non-Goals

- This design does not define the concurrency transport between provider sampling workqueues and the domain runtime worker (deferred to a later runtime spec).
- This design does not define the Measurement Snapshot struct that providers publish (deferred to the runtime spec).
- This design does not define the Runtime Config Snapshot struct that the domain publishes to providers (deferred to the runtime spec).
- The controller itself is a Zephyr device but is a thin aggregator; it does not own product policy, protection, ramping, or recovery.

## References

- `docs/superpowers/specs/2026-06-18-zephyr-native-production-runtime-architecture.md` — production runtime architecture
- `docs/superpowers/specs/2026-06-15-voltage-control-domain-behavior.md` — domain behavior specification
- `include/regmap/hvb_regs.h` — shared register map and capability bit definitions
- `dts/bindings/dac/adi,ad5541.yaml` — existing DAC binding pattern
- `dts/bindings/sensor/ti,ads1232.yaml` — existing ADC binding pattern

# Modbus Simulator Split Design

Date: 2026-06-17
Status: Implemented

## Problem

`hvb_controller` used simulated voltage/current telemetry to exercise the Modbus interface before hardware runtime integration. That behavior remains useful for host-tool development and new-board RS-485 bring-up, but it should not live inside the production controller app now that the key hardware components have been brought up.

## Design

The Modbus register adapter is shared library code under `lib/voltage_control/` with its public header under `include/voltage_control/`. Both the production app and simulator app use the same adapter and domain APIs, so the Modbus register map remains identical.

`applications/hvb_controller` is the production app. It initializes the domain and Modbus server, updates uptime, and keeps the heartbeat LED alive. It does not call the simulated domain tick path until a hardware-backed runtime service is added.

`demos/modbus_sim` is the explicit Modbus simulator. It uses USART6/RS-485 and the normal Modbus adapter, then drives `vc_domain_tick()` with randomized telemetry noise. Host tools can use it as a real-wire simulator without treating that behavior as production firmware.

## Verification

`tests/architecture/controller_split.sh` guards the boundary: production firmware must not contain random/noise simulation support, and the simulator app must own the simulated tick path. Build verification should cover both `applications/hvb_controller` and `demos/modbus_sim`.

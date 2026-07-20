# Factory Tool Bundle

The full sequence a new board goes through after being soldered, in the
order you run them. Full procedure detail, per-board-variant branching,
and the known real-instrumental-calibration gap for jw_hvb:
[`docs/guide/factory-procedures.md`](../../docs/guide/factory-procedures.md).

| Step | Directory | Tool | Board(s) |
|---|---|---|---|
| 1 | `01_flash/` | `flash.sh` | jw_hvb, jw_lvb |
| 2 | `02_bringup/` | `factory_bringup.sh` | jw_hvb, jw_lvb |
| 3 | `03_feature_test/` | `board_test.sh` | jw_hvb, jw_lvb |
| 4 | `04_stress_test/` | `stress_test.py` | jw_hvb, jw_lvb |
| 5 | `05_sweep_test/` | `dac_sweep_test.sh` | jw_hvb only |
| 6 | `06_self_cal/` | `jw_hvb/jw_hvb_selfcal.py` (fallback), `jw_lvb/jw_lvb_calibrate.py` (official) | both, different roles |
| 7 | `07_instrumental_cal/` | `psb_factory_tool` (`psb_factory_tui` REPL) | jw_hvb only |

Each numbered directory has its own README with the full flag reference for
that step's tool.

# Claude Code guide — stevenrwood/iMXRT1062 fork

A **fork of [grblHAL/iMXRT1062](https://github.com/grblHAL/iMXRT1062)** — the Teensy 4.x (i.MXRT1062)
grblHAL driver. This driver repo carries the **build config + the tracker/index**; the actual code
changes live in **submodule forks** (`core`, `Plugin_SD_card`, `Plugin_networking`), each staged as a PR.

## Read first
- **`srw/ProposedPRs.html` / `srw/ProposedPRs.pdf`** — per-PR tracker (each submodule fork + branch,
  upstream/fork PR, status, "why").
- **`srw/README.md`** — the canonical `srw/` bundle (this + `firmware-forks.json`, `setup_forks.sh`).
- Cross-fork **`Overview.pdf`** is in the [ioSender fork](https://github.com/stevenrwood/ioSender).

## Build (PlatformIO — any host OS: macOS, Linux, Windows)
```
pip install platformio          # or the PlatformIO VSCode extension
git clone --recurse-submodules https://github.com/stevenrwood/iMXRT1062.git
cd iMXRT1062/grblHAL_Teensy4
pio run -e teensy41             # -> .pio/build/teensy41/firmware.hex   (teensy40 also available)
```
PlatformIO downloads the ARM toolchain per host OS; the `.hex` is identical everywhere. Flash with the
cross-platform Teensy Loader.

## All-or-nothing — default branch `srw/local-build-config`
Clone **`srw/local-build-config`** for the turnkey firmware: it pins every submodule to its fork branch
(the case-insensitive-littlefs fix is folded into the driver), so all the changes ride along with no
extra step. There is **no selective composer** for firmware — the changes interlock, and the per-submodule
`feat/*`/`fix/*` branches exist for **upstream submission / cherry-pick**, not subset builds.

To wire the `fork` remotes on each submodule (for pushing your own branches):
`srw/setup_forks.sh <path-to-this-checkout>` — reads `srw/firmware-forks.json`.

## Pairing
Pair with the **ioSender** and **Simulator** forks (see each repo's `Overview` / tracker): e.g. the ATC
tool-change macros, littlefs file ops and hostname-boot-info are driven/consumed by ioSender over the
connection.

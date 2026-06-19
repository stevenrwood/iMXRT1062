# SRW build-config branch — laptop bootstrap

You're on branch `srw/local-build-config` of `stevenrwood/iMXRT1062`, the SRW fork of the grblHAL Teensy 4.x driver. This branch carries everything a fresh machine needs to build SRW's firmware for the T41U5XBB breakout and pick up the 5 in-flight upstream PRs.

## What this branch adds on top of upstream `grblHAL/iMXRT1062` master

| Path | Purpose |
|---|---|
| `grblHAL_Teensy4/src/my_machine.h` | BOARD_T41U5XBB config — 17 Web Builder JSON symbols + `LITTLEFS_ENABLE=2` (PR #966) + `DEFAULT_MACRO_ATC_OPTIONS=2` (PR #14) |
| `grblHAL_Teensy4/platformio.ini` | Adds `extra_scripts = pre:scripts/touch_build_stamp.py` and `-DLFS_CASE_INSENSITIVE` (driver-local littlefs fix) |
| `grblHAL_Teensy4/src/littlefs/lfs.c` | **Driver-local** (folded in): case-folded name compare gated by `LFS_CASE_INSENSITIVE`, so LittleFS matches FatFs case-insensitivity and `O<name> CALL` resolves regardless of the parser upper-casing the label. Patches vendored code — not an upstream PR |
| `grblHAL_Teensy4/scripts/touch_build_stamp.py` | Pre-build hook that rewrites `src/build_stamp.h` with the current datetime on every compile. Companion to PR #967 |
| `.gitignore` | Excludes the generated `src/build_stamp.h` |
| `srw/` | This directory — tracker + bootstrap files |

## What's in `srw/`

| File | Purpose |
|---|---|
| `firmware-forks.json` | Maps each grblHAL submodule to its `stevenrwood` fork URL and lists branches pushed (with live PR numbers and status) |
| `proposedprs.html` | Human-readable summary of the 5 upstream PRs + the 2 driver-local branches. Open in a browser |
| `proposedprs.pdf` | Rendered `proposedprs.html` — views inline on GitHub (this fork's website link points here) |
| `setup_forks.sh` | One-shot script that configures a `fork` remote on each submodule from the JSON manifest |
| `README.md` | This file |

These tracker files are the **canonical** copies — this driver fork is where they are actually used (`setup_forks.sh` runs against the submodules here, and you bootstrap a build from a single clone of this repo). `proposedprs.pdf` is a rendered copy of `proposedprs.html` for inline viewing on GitHub. The hardware repo (`stevenrwood/grblHAL-teensy-4.x`) just links here rather than keeping its own copy.

## Fresh-machine bootstrap

```bash
# Clone the driver fork at this branch
git clone -b srw/local-build-config https://github.com/stevenrwood/iMXRT1062.git
cd iMXRT1062

# Initialize submodules (core, sdcard, networking, eeprom, etc.)
git submodule update --init --recursive

# Add the stevenrwood 'fork' remote on each PR-bearing submodule
./srw/setup_forks.sh .
```

After `setup_forks.sh`, each of `grblHAL_Teensy4/src/{grbl,sdcard,networking}` has a `fork` remote pointing at the matching `stevenrwood/*` repo, with all 5 PR branches already visible under `fork/*`.

## Check out the PR branches for a build that exercises all 5 PRs

The branches don't conflict — each PR touches one distinct file — so you can stack them per submodule:

```bash
# core: the two PRs + the driver-local read_command whitespace-skip are pre-combined
# on the fork, so just check it out (no manual stacking needed)
cd grblHAL_Teensy4/src/grbl
git checkout -b srw/combined fork/srw/combined   # = feat/littlefs-ymodem-combined + feat/build-timestamp-line + whitespace-skip

# sdcard: two PRs, stack them
cd ../sdcard
git checkout -b srw/combined fork/fix/reset-during-sd-streaming
git merge --no-edit fork/feat/default-macro-atc-options

# networking: one PR
cd ../networking
git checkout fork/feat/hostname-boot-info -b srw/combined
```

To use upstream master instead (no SRW PRs active), check out `origin/master` in each submodule. Note that `my_machine.h` on this branch has `LITTLEFS_ENABLE=2` and `DEFAULT_MACRO_ATC_OPTIONS=2`, both of which rely on PR features — without those PRs applied in the submodule trees, the build either falls back to `LITTLEFS_ENABLE=1` semantics or emits an "unknown define" diagnostic. Adjust `my_machine.h` if you want a plain upstream build.

## Build

```bash
cd grblHAL_Teensy4
pio run -e teensy41
```

You should see `build-stamp: wrote src/build_stamp.h = <date> <time>` in the build log, confirming the pre-build script ran. Resulting `.hex` lands in `.pio/build/teensy41/firmware.hex`.

## The 5 upstream PRs (against `grblHAL/*`)

| # | Repo | Branch (on `stevenrwood`) | Status |
|---|---|---|---|
| [#13](https://github.com/grblHAL/Plugin_SD_card/pull/13) | Plugin_SD_card | `fix/reset-during-sd-streaming` | Open |
| [#14](https://github.com/grblHAL/Plugin_SD_card/pull/14) | Plugin_SD_card | `feat/default-macro-atc-options` | Open |
| [#966](https://github.com/grblHAL/core/pull/966) | core | `feat/littlefs-ymodem-combined` | Open |
| [#967](https://github.com/grblHAL/core/pull/967) | core | `feat/build-timestamp-line` | Draft |
| [#22](https://github.com/grblHAL/Plugin_networking/pull/22) | Plugin_networking | `feat/hostname-boot-info` | Open |

Open `srw/proposedprs.html` for the per-PR summary and "why" notes.

## Driver-local branches on this fork (no upstream PR)

There are two driver-local fixes; both are now baked into a `srw/local-build-config` build (no extra overlay step for them):

| Branch | Purpose |
|---|---|
| `srw/local-build-config` (this one) | Build config + tracker bundle. **Now also carries the case-insensitive littlefs fix folded in** (`lfs.c` case-folded compare + `-DLFS_CASE_INSENSITIVE`), so a single clone builds firmware where `O<name> CALL` resolves a lower-case `cal.macro`. |
| grbl `srw/combined` (on `stevenrwood/core`) | The two core PRs (#966, #967) **plus** a driver-local **whitespace-skip** in `ngc_flowctrl` `read_command`, so `O<cal> CALL` parses with or without the space before the keyword. The whitespace-skip is driver-local — not an upstream PR. Checked out by the grbl overlay step above. |
| `pr/littlefs-case-insensitive` | The standalone `lfs.c` patch. **Superseded** — now folded into `srw/local-build-config`; kept for reference. |

## Companion repos

- **Hardware** (PCB, schematics, macros): `https://github.com/stevenrwood/grblHAL-teensy-4.x` — it links back to this `srw/` tracker rather than keeping its own copy.
- **ioSender** (Windows-only sender): `https://github.com/terjeio/ioSender` — used as the host-side sender on the laptop.

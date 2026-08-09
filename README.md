# emdoor-wmi

Linux driver for the Emdoor EmdAcpi ACPI firmware on Nimo, Xuanpai, and MetaMech laptop PCs. Talks to the embedded controller directly - the EmdAcpi WMI methods are broken (see "Why" below) so we route around them. Only the keyboard WMI method (`WMDA`) parses cleanly under Linux ACPICA and we evaluate that one directly.

| Subsystem | Linux interface | Path |
| --- | --- | --- |
| Power mode | `platform_profile` | `/sys/class/platform_profile/platform-profile-0/` |
| Fan RPM | `hwmon` `fan1_input` | `/sys/class/hwmon/hwmonN/fan1_input` |
| Fan mode (AUTO/MAX) | `hwmon` `fan_mode` (custom) | `/sys/class/hwmon/hwmonN/fan_mode` |
| Power limits (PL1/PL2/PL3/SYS/GPU) | `hwmon` `power[1-6]_cap` | `/sys/class/hwmon/hwmonN/powerN_cap` |
| Battery charge thresholds | `power_supply` extension on BAT0 | `/sys/class/power_supply/BAT0/charge_control_*_threshold` |
| Keyboard RGB (6 zones + side bars) | `led-class-multicolor` | `/sys/class/leds/emdoor:multicolor:*` |
| Animation mode | `leds` control surface | `/sys/class/leds/emdoor:rgb:mode/mode` |

A companion module **ecwmi** (`../ecwmi/`) provides diagnostic-only ECWR/PWMD/TLID access. Load on demand.

## Why this driver exists

14 of the 15 EmdAcpi `WMxx` control methods declare a 63-bit `BufferField` at offset `0x20` of an 8-byte argument buffer, which is structurally out-of-bounds. Linux ACPICA rejects the declaration with `AE_AML_BUFFER_LIMIT` before any case body runs and emits a kernel log error every probe; the unreferenced field is dead code. The firmware can't be patched.

For the methods we actually need:

- `WMBF` (power-mode): value lives in the EC `PWMD` register at `OperationRegion` offset `0x7C`. We read/write `PWMD` directly; `WMBF` is never evaluated, so no ACPI errors.
- `WMDD` (battery charge ratio): EC's `MICP`/`MXCP` registers at offsets `0xBB`/`0xBC` carry the same values. We read/write those directly.
- `WMDA` (keyboard): declares a 0x50-byte local buffer so the bogus `BufferField` sits inside the buffer instead of past the end. `WMDA` parses cleanly; we evaluate it via `wmidev_evaluate_method()`.

EC register names and offsets are documented in `/home/deck/control/analysis/EC-RAM-MAP.md`. The full WMI call table and WMDD method internals are in `/home/deck/control/analysis/ACPI-FIRMWARE-EXTRACTION.md`.

## Supported hardware

| Vendor | Product |
| --- | --- |
| Nimo Direct INC. | N161L (Axis) |

DMI whitelist gates the bind at module init. `force_load=1` skips it for diagnostics on unrecognised hardware. **Do not** add this to a persistent config - add explicit DMI strings for new vendors instead.

## Modules

`emdoor-wmi.ko` - production driver. Always loaded.

`ecwmi.ko` - diagnostic companion (sibling directory `../ecwmi/`). Exposes raw `pwmd_read`/`pwmd_write`, `tlid_read`/`tlid_write`, and `ecwr_trigger` on a WMI device under `/sys/bus/wmi/devices/D06385DE-...`. Load when debugging.

Both accept `force_load=1`.

## Subsystems

### Power mode (`platform_profile`)

Three profiles: `low-power`, `balanced`, `performance`. The driver reads/writes EC `PWMD` (0x7C) directly. After every successful write, it pulses EC `ECWR` (0x78) so the EC re-applies PL1/PL2/PL3 limits for the new mode.

| ECPM | Profile |
| --- | --- |
| 1 | Performance |
| 2 | Balanced |
| 3 | Power Saver (Quiet) |

### Fan control (`hwmon`)

`hwmon` device `emdoor_fan` on platform device `emdoor-power`. Found via `ls /sys/class/hwmon/`.

| Attribute | Access | Description |
| --- | --- | --- |
| `fan1_input` | ro | RPM (EC `FN1H`/`FN1L` at 0x76/0x77) |
| `fan_mode` | rw | 1 = AUTO, 2 = MAX (EC `TLID` at 0x32; requires XXTT arm sequence) |

On probe the driver forces fan mode to AUTO. EC's `TLID` register is undefined at first power-on; the BIOS leaves it at MAX. Forcing AUTO eliminates the boot-time mismatch between firmware idle behaviour and the value visible through `fan_mode`.

### Power limits (`hwmon`)

`power[1-6]_cap` write-only, microWatts (the EC CMS mailbox takes milliwatts; the driver divides on write). Reads return `EINVAL`.

| `hwmon` attr | EC register | Meaning |
| --- | --- | --- |
| `power1_cap` | 0x05 | PL1 (sustained) |
| `power2_cap` | 0x06 | PL2 (short turbo) |
| `power3_cap` | 0x07 | PL3 (peak) |
| `power4_cap` | 0x2E | PL1_DUP (firmware mirror of PL1) |
| `power5_cap` | 0x32 | SYS_PL (system total) |
| `power6_cap` | 0x13 | GPU_PL |

CMS mailbox protocol: `outb(0x0C, 0x72) -> outb(reg, 0x73) -> outl(value, 0x73)` with small udelay between each step. The driver does this internally.

### Battery charge thresholds (`power_supply`)

The driver registers as a `power_supply` extension on the existing `BAT0` (or `BAT1`-`BAT4` if `BAT0` is absent) via the ACPI `battery_hook` mechanism. Standard userspace tools (`tlp`, `auto-cpufreq`) see the new properties on BAT0 with no configuration:

```
/sys/class/power_supply/BAT0/charge_control_start_threshold   rw   MICP (0xBB)
                                              end_threshold    rw   MXCP (0xBC)
```

EC semantics (per `WMDD` case 2 in DSDT):
- `end_threshold = N` writes `MXCP = N` and `MICP = N - 1`
- `start_threshold = M` writes `MICP = M`; the kernel rejects writes where `start >= end`

Defaults read at boot: `MICP=0, MXCP=100` (no charge limit). All four EC firmware dumps in `/home/deck/control/` confirm this.

### Keyboard RGB (`led-class-multicolor`)

6 zones: 4 keyboard zones (F1..F12 area split into 4 quadrants) + 2 chassis side bars. Each is a multicolor LED class device at `/sys/class/leds/emdoor:multicolor:zone{1..4}` and `/sys/class/leds/emdoor:multicolor:bar-{left,right}`. Standard `multi_intensity` attribute: `R G B`, three integers 0-255.

Animation modes live on a virtual LED class device `/sys/class/leds/emdoor:rgb:mode/`:

| `mode` | EC behaviour |
| --- | --- |
| `always` | static, per-zone RGB |
| `twinkle`, `wave`, `breath`, `colorcycle`, `reactive`, `ripple`, `spiralrainbow`, `rainbowripple` | dynamic; EC owns animation timing |
| `off` | LEDs off |
| `reset` | factory state |

`modes` lists every valid value. SteamOS's `70-steam-jupiter-leds.rules` chowns the `mode` file to `deck:deck` so Decky Loader can switch modes without root.

The driver does **not** apply an initial RGB state on probe; hardware is left as the firmware last had it, so `rmmod`/`insmod` does not stomp user colours.

## Build

```sh
# Production driver
cd /home/deck/control/emdoor-wmi
make

# Diagnostic companion
cd /home/deck/control/ecwmi
make
```

Both `Makefile`s build against `/lib/modules/$(uname -r)/build` by default. Override with `KDIR=...`.

## Install via DKMS (recommended)

DKMS (Dynamic Kernel Module Support) automatically rebuilds the module when a new kernel is installed.

### Prerequisites

Install DKMS and kernel headers for your distribution:

```sh
# Debian/Ubuntu
sudo apt install dkms linux-headers-$(uname -r)

# Fedora
sudo dnf install dkms kernel-devel-$(uname -r)

# Arch
sudo pacman -S dkms linux-headers
```

### Installation

```sh
cd /home/deck/control/emdoor-wmi
sudo ./dkms-install.sh
```

The helper script copies the source to `/usr/src/emdoor-wmi-<version>/`, adds it to DKMS, builds for the current kernel, and installs it.

### DKMS commands

```sh
# Check status
sudo ./dkms-install.sh status
# or: dkms status -m emdoor-wmi

# Rebuild for current kernel only
sudo ./dkms-install.sh build

# Remove from DKMS (uninstalls from all kernels)
sudo ./dkms-install.sh remove
```

### Manual DKMS steps (if not using the helper script)

```sh
VERSION=$(cat VERSION)
sudo cp -r . /usr/src/emdoor-wmi-${VERSION}/
sudo dkms add -m emdoor-wmi -v ${VERSION}
sudo dkms build -m emdoor-wmi -v ${VERSION}
sudo dkms install -m emdoor-wmi -v ${VERSION}
```

## Load (manual, without DKMS)

```sh
sudo insmod /home/deck/control/emdoor-wmi/emdoor-wmi.ko
# Diagnostic companion (opt-in)
sudo insmod /home/deck/control/ecwmi/ecwmi.ko
```

udev auto-loads `emdoor-wmi` on boot once installed; the DMI whitelist gates the bind.

## Diagnostics

```sh
# Power mode
cat /sys/class/platform_profile/platform-profile-0/profile
echo balanced | sudo tee /sys/class/platform_profile/platform-profile-0/profile

# Fan
HWMON=/sys/class/hwmon/hwmon$(ls /sys/class/hwmon/ | grep -o '[0-9]*' | head -1)
cat $HWMON/fan1_input
echo 1 | sudo tee $HWMON/fan_mode     # AUTO

# Battery charge limits
cat /sys/class/power_supply/BAT0/charge_control_end_threshold
echo 80 | sudo tee /sys/class/power_supply/BAT0/charge_control_end_threshold

# RGB
echo 255 0 0 | sudo tee /sys/class/leds/emdoor:multicolor:zone1/multi_intensity
echo wave | sudo tee /sys/class/leds/emdoor:rgb:mode/mode

# Watch the kernel ring buffer
sudo dmesg -w | grep -E 'emdoor|EmdAcpi|ecwmi'
```

### Probe messages

| Message | Meaning |
| --- | --- |
| `EmdAcpi power bound (current profile=N, fan=AUTO)` | EC IO working; PWMD was N, fan forced to AUTO |
| `EmdAcpi charge control attached to BAT0` | Charge threshold extension registered |
| `EmdAcpi keyboard backlight bound (type=4zone max=0 mode=always)` | Keyboard WMI driver bound |
| No ACPI BIOS Errors at probe | Confirmed: driver doesn't evaluate broken WMI methods |

If `EmdAcpi power bound` doesn't appear, the DMI whitelist didn't match - try `force_load=1` to confirm.

## EC register map

Authoritative source: `/home/deck/control/analysis/EC-RAM-MAP.md` (DSDT line 8680, `OperationRegion (ECF2, EmbeddedControl, Zero, 0xFF)`).

Registers the driver uses:

| Offset | Name | Use |
| --- | --- | --- |
| 0x30 | XXTT | Fan control arm (write 0x11 before TLID) |
| 0x32 | TLID | Fan mode (1=AUTO, 2=MAX) |
| 0x76/0x77 | FN1H/FN1L | Fan 1 RPM |
| 0x78 | ECWR | Power-limit refresh trigger |
| 0x7C | PWMD | Power mode (1=perf, 2=bal, 3=quiet) |
| 0xBB | MICP | Min charge percent |
| 0xBC | MXCP | Max charge percent |

CMS mailbox ports (for `power[1-6]_cap`): cmd 0x72, data 0x73, ALIB command 0x0C.

## License

SPDX-License-Identifier: GPL-2.0-or-later.
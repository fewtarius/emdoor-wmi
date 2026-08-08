# emdoor-wmi

Linux driver for the Emdoor EmdAcpi ACPI firmware used in Nimo, Xuanpai,
and MetaMech laptop PCs. Provides:

- `platform_profile` integration: select Performance, Balanced, or
  Quiet via standard Linux sysfs.
- Battery charge ratio control: read MICP/MXCP and write charge limits
  via `WMDD` (with EC fallback).
- Power limit override: write PL1-PL3, SYS_PL, GPU_PL via EC CMS
  mailbox.
- Six-zone RGB control: four keyboard regions plus the two chassis
  side bars, exposed as Linux multicolor LED class devices.
- ECWR trigger: force EC to re-apply power limits for current mode.

The driver targets the AML produced by Emdoor's "EmdAcpi" compiler and
the EC firmware that pairs with it. It is **not** intended for
hardware from other vendors that happen to share a GUID.

## Why this driver exists

Fourteen of the fifteen EmdAcpi WMxx control methods declare a
63-bit `BufferField` at offset `0x20` of an 8-byte argument buffer,
which is structurally out-of-bounds. Linux ACPICA rejects the
declaration with `AE_AML_BUFFER_LIMIT` before any case body runs.

The unreferenced field is dead code. We cannot patch the BIOS or
make Linux ACPICA accept the methods. We route around them:

- `WMBF` (power-mode): the actual value lives in the EC's `PWMD`
  field at `OperationRegion` offset `0x7C`. The driver tries `WMBF`
  first; on `AE_AML_BUFFER_LIMIT` it sets a per-device quirk and
  reads/writes `PWMD` directly for the lifetime of the device. The
  only thing the EC fallback omits is the fan curve re-tuning that
  `E004()` performs after `PWMD = ECPM`; `platform_profile` is not
  affected.
- `WMDA` (keyboard): the method is well-formed (0x50-byte local
  buffer instead of 0x08), so we evaluate it directly via
  `wmidev_evaluate_method()`.
- `WMDD` (battery charge ratio): parses cleanly with an 8-byte
  buffer. The driver tries the WMI path first and falls back to
  direct EC reads/writes of MICP (0xBB) and MXCP (0xBC) on failure.

Because `WMBF` and `WMDD` may evaluate on healthy firmware and fail on
broken firmware, the driver probes the WMI path first and switches to
the EC path transparently. The user-visible behaviour is the same in
either case.

## Supported hardware

| Vendor | Product |
| --- | --- |
| Nimo Direct INC. | N161L (Axis)|

The driver carries a DMI whitelist to avoid binding to unrelated
EmdAcpi devices. The whitelist is consulted in `emd_dmi_check()`
during module init; if no entry matches the driver prints a message
and returns `-ENODEV`.

A `force_load=1` module parameter skips the whitelist for
diagnostics on unrecognised hardware. **Do not** add this to a
persistent config; the whitelist exists for a reason. New vendors
should be added with explicit DMI strings, not by silencing the
check.

## Module parameters

| Parameter | Access | Description |
| --- | --- | --- |
| `force_load` | bool, 0444 | Skip the DMI whitelist |

The driver depends on the standard Linux WMI subsystem and on
`CONFIG_ACPI_EC` for the EC IO fallback. There is no firmware upload
or BIOS lock manipulation.

## Power mode (`platform_profile`)

GUID `4BCA6480-4D03-4674-84CB-26B4C8F5CFC2`. Bound as
`emdoor-power`.

The driver registers with the `platform_profile` framework, which
exposes the standard interface at:

```
/sys/bus/wmi/devices/4BCA6480-4D03-4674-84CB-26B4C8F5CFC2-10/platform-profile/platform-profile-0/
```

| Attribute | Access | Values |
| --- | --- | --- |
| `profile` | rw | `low-power`, `balanced`, `performance` |
| `choices` | ro | the three values above, space-separated |
| `name` | ro | `epm` |

Power-mode select values in the ECPM enumeration:

| ECPM | Profile |
| --- | --- |
| 1 | Performance |
| 2 | Balanced |
| 3 | Power Saver (Quiet) |

`WMBF` case 4 (ECPM=4) reads back the current mode. The driver uses
that case on healthy firmware and the EC path on broken firmware,
transparently. The kernel's `platform_profile` core persists the
last-selected profile across reboots; the EC firmware stores the
current profile in `PWMD` so both paths agree on reload.

### ECWR attribute

```
/sys/bus/wmi/devices/4BCA6480-4D03-4674-84CB-26B4C8F5CFC2-10/ecwr
```

Write-only attribute that triggers the EC's E4 interrupt handler to
re-apply power limits for the current mode. Used for debugging and
manual limit refresh after CMS mailbox changes.

## Battery Charge Ratio (WMDD)

GUID `6B40A935-7FEF-42B6-B08D-6C79B57D6C35`. Bound as `emdoor-charge`.

The driver exposes three sysfs attributes on the WMI device for controlling
the battery charge thresholds via the EC's MICP (Min Charge Percent) and
MXCP (Max Charge Percent) registers:

```
/sys/bus/wmi/devices/6B40A935-7FEF-42B6-B08D-6C79B57D6C35-11/
├── charge_micp              ro
├── charge_mxcp              ro
└── charge_ratio             wo
```

| Attribute | Access | Description |
| --- | --- | --- |
| `charge_micp` | ro | Current minimum charge percent (0-100) |
| `charge_mxcp` | ro | Current maximum charge percent (0-100) |
| `charge_ratio` | wo | Write new charge limit; sets MXCP=val, MICP=val-1 |

The driver tries the WMI path first (`WMDD` case 1 for read, case 2 for
write). If the firmware method fails, it falls back to direct EC IO on
registers `0xBB` (MICP) and `0xBC` (MXCP), latching a per-device quirk
flag so subsequent operations use the EC path directly.

Per the DSDT, `WMDD` case 2 writes `Arg2` to MXCP and `Arg2-1` to MICP.
The `charge_ratio` attribute accepts a single value (0-255, typically
50-100) and applies this mapping automatically.

This is a write-only control for the charge limit; there is no firmware
readback path for the current charge *level* (that comes from the standard
battery sysfs).

## Power Limit Override (CMS mailbox)

The EC exposes a Command/Data mailbox at I/O ports `0x72`/`0x73`.
Command `0x0C` (ALIB) writes a 32-bit value to a specific register:

```
outb(0x0C, 0x72) -> outb(reg, 0x73) -> outl(value, 0x73)
```

The driver creates a platform device `emdoor-power-limits` with a
`power_limits` sysfs group containing write-only attributes for each
power limit register:

```
/sys/devices/platform/emdoor-power-limits/power_limits/
├── pl1              wo (sustained power limit, mW)
├── pl2              wo (short turbo limit, mW)
├── pl3              wo (peak power limit, mW)
├── pl1_dup          wo (PL1 duplicate, some firmware mirrors PL1 here)
├── sys_pl           wo (system total power limit, mW)
└── gpu_pl           wo (GPU power limit, mW)
```

All values are in **milliwatts**. Reads return `write-only (mW)` as a
reminder of the units. This interface is intended for debugging and
manual tuning; the EC firmware manages limits automatically based on
the current power mode (see `ecwr` below).

## ECWR - Power Limit Refresh

The power mode driver also exposes a write-only `ecwr` attribute on the
WMI device:

```
/sys/bus/wmi/devices/4BCA6480-4D03-4674-84CB-26B4C8F5CFC2-10/ecwr
```

Writing any non-zero value triggers the EC's E4 interrupt handler,
forcing it to re-evaluate the current power mode and re-apply the
corresponding PL1/PL2/PL3 limits. This is useful when manually
adjusting limits via the CMS mailbox and wanting them to take effect
immediately without a power mode change.

## Keyboard RGB

GUID `8600ACCE-FB9B-443E-86F4-3C867398AAE5`. Bound as `emdoor-kbd`.

### Topology

The EC exposes six logical zones. 

| Logical zone | Linux sysfs path | Physical location | Wire selector |
| --- | --- | --- | --- |
| zone1 | `emdoor:multicolor:zone1` | keyboard left quadrant | FACS bit 0 |
| zone2 | `emdoor:multicolor:zone2` | keyboard left-of-center | FACS bit 1 |
| zone3 | `emdoor:multicolor:zone3` | keyboard right-of-center | FACS bit 2 |
| zone4 | `emdoor:multicolor:zone4` | keyboard right quadrant | FACS bit 3 |
| bar-left | `emdoor:multicolor:bar-left` | left chassis side bar | FLAB bit 4 (LLB0=0) |
| bar-right | `emdoor:multicolor:bar-right` | right chassis side bar | FLAB bit 5 (LLB0=1) |

Keyboard physical layout (zones 1-4 left to right, chassis bars on
each end):

```
+------+------+------+------+------+------+
|      |      |      |      |      |      |
| bar- | z1   | z2   | z3   | z4   | bar- |
| left | left | left | right| right| right|
|      | quad | ctr  | ctr  |      |      |
|      |      |      |      |      |      |
+------+------+------+------+------+------+
```

Each keyboard zone is one BST2 `FACS` selector. FACS `0x0F` would
write all four keyboard zones in a single BST2, but the driver does
not use it: it would re-arm the FLAB channel and bleed into the
side bars. Per-zone writes keep each channel isolated.

The FACS all-zones selector (`0x0F`) and the FLAB both-bars
selector (`0x30`, 8-byte payload with the RGB duplicated into LLB0
and LLB1) are valid wire encodings but the driver does not issue
them; per-zone writes avoid the FACS-all write because it would
re-arm the LLBR channel and re-introduce the chassis bars.

### Sysfs attributes

The `type` attribute lives on the keyboard WMI device. The `mode` and
`modes` attributes live on a dedicated `emdoor:rgb:mode` LED class
device, which is a virtual LED class device (no real LEDs) that exists
purely as a control surface under `/sys/class/leds/`. Putting them
under the LED class subsystem lets SteamOS's
`70-steam-jupiter-leds.rules` udev rule auto-chown the `mode` file
to `deck:deck`, so Decky Loader (running as the `deck` user) can
switch animation modes without root.

```
/sys/bus/wmi/devices/8600ACCE-FB9B-443E-86F4-3C867398AAE5-12/
└── type                    ro
```

```
/sys/class/leds/emdoor:rgb:mode/
├── mode                    rw
├── modes                   ro
├── brightness              rw   (no-op; control-surface only)
└── max_brightness          ro   (= 1)
```

| Attribute | Values |
| --- | --- |
| `type` (WMI) | `4zone` (only supported value) |
| `mode` (LED class) | any of the values listed in `modes` (see below) |
| `modes` (LED class) | space-separated list of all valid values for `mode` |

`modes` mirrors the kernel's `platform_profile_choices` sysfs
attribute: read-only, space-separated list of all valid values
accepted by `mode`. Userspace can read it to discover the supported
set without hardcoding the OEM names. The set is fixed at module
load and does not change at runtime.

The `emdoor:rgb:mode` LED class device is a control surface, not a
real LED. Its `brightness` and `max_brightness` attributes are
standard LED class boilerplate; writing to `brightness` is a no-op
because there is no LED to set. The `mode` and `modes` files are
the actual interface.

Under `/sys/class/leds/` the six zone multicolor LED class devices
also live here:

```
emdoor:multicolor:zone1
emdoor:multicolor:zone2
emdoor:multicolor:zone3
emdoor:multicolor:zone4
emdoor:multicolor:bar-left
emdoor:multicolor:bar-right
```

Each zone exposes the standard `led-class-multicolor` attributes:

| Attribute | Description |
| --- | --- |
| `multi_intensity` | `R G B`, three integers 0-255 separated by a single space |
| `multi_index` | `red green blue` (read-only) |
| `brightness` | `0` (off) or `1` (on); `max_brightness` is `1` |

### Modes (`mode`)

Every effect below corresponds to one `FA06` value plus the EC's `EDTA`
enumeration; the EC owns the animation timing and curve.

| `mode` | FA06 | EC EDTA | Animation |
| --- | --- | --- | --- |
| `always` | 0 | 2 | static, no animation; per-zone writes are layered on top via FA06=0xFE |
| `twinkle` | 1 | 5 | stars fade in and out independently; the EC picks which keys light up each tick |
| `wave` | 2 | 4 | bright band sweeps across the keyboard in `EffectDirection` (default left to right) |
| `breath` | 3 | 3 | all zones fade brightness up and down together |
| `colorcycle` | 4 | 6 | full-keyboard rainbow cycle; zone RGB drives only the static-baseline reference |
| `reactive` | 5 | 7 | reactive press flash; not animated without a key-event source, so on Linux it sits on the zone RGB until input attaches |
| `ripple` | 6 | 8 | ripple spreads out from a key press; without a key source this renders as a static gradient on the EC |
| `spiralrainbow` | 7 | 9 | spiral/centripetal rainbow sweep |
| `rainbowripple` | 8 | 10 | rainbow-tinted ripple |
| `off` | - | - | FA06=0xFE, FA07=0; turns the keyboard off |
| `reset` | - | - | FA06=0xFF, FA07=0xFF; factory state |

In `always` mode the driver sends a BST2 baseline first (which also
runs `ECMB=0x18` with `FA01`/`FA02`/`FA03`), then per-zone writes
with `FA06=0xFE` so BST2 skips the baseline and only updates the
targeted FACS or FLAB channel. Re-running the baseline between
zones would clobber every other zone with the per-zone colour;
empirically this presents as flicker.

Dynamic modes (anything other than `always`) only need one BST2 per
write: `FA00=0`, `FA06=mode`, the per-zone RGB goes into the EC, and
the EC animates it. The driver does not re-issue every zone on each
dynamic write; a single BST2 with `FA00=0` is enough.

### Probe behaviour

On probe the driver:

1. Reads the keyboard type via `WMDA` case 2 (`DTID=2`). Anything
   other than `KBTE=2` (4-zone) is rejected with `-ENODEV`. Devices
   reporting `KBTE=1` (BST1 single-colour) or `KBTE=3` (BST3
   per-key) deliberately do not bind; the BST2 wire format is
   meaningless to them.
2. Registers six multicolor LED class devices, one per logical zone.
3. Registers the `type` attribute on the WMI device and the
   `mode` / `modes` attributes on the `emdoor:rgb:mode` LED class
   control surface.
4. **Does not** apply an initial RGB state. The hardware is left
   exactly as the firmware last had it, so a reload does not stomp
   the user's colours.

### Behaviour on `rmmod`

The keyboard driver implements `remove` to unregister LED class
devices without writing to hardware. The power mode and RGB state
survive `rmmod`/`insmod` cycles; only the sysfs interface comes and
goes.

This is intentional: the firmware holds the last configuration, and
there is no per-zone RGB readback path. The driver cannot recover
the cached values on a fresh probe, so sysfs shows the conservative
default `brightness=1 intensity=0 0 0` per zone on load. Writing to
`multi_intensity` applies to the hardware normally; the next
`rmmod`/`insmod` cycle still leaves the colours in place.

### Caveats

- The intensity value is canonical in Linux's per-led-class-attribute
  way; `brightness` is a 0/1 toggle. The driver intentionally keeps
  `brightness=1` (logical "on") at probe so that `multi_intensity`
  writes flow through to hardware. The kernel's
  `multi_intensity_store` calls
  `led_set_brightness(cdev, cdev->brightness)` after caching the
  new subled intensities; if the cached brightness were `0`, every
  subsequent write would silently reduce to "off" because the
  handler would be called with `brightness=0`. With `brightness=1`,
  brightness is left untouched by colour writes and the cached
  intensity reaches the EC.
- The driver's `mode_store` and brightness handlers all go through
  `priv->lock`. Multi-zone updates are not atomic across
  `multi_intensity` writes because Linux exposes per-zone
  attributes, but each individual zone write is atomic at the WMI
  boundary.
- `force_load=1` is **not** a workaround for unsupported keyboards;
  if the firmware reports `KBTE != 2` the driver still refuses to
  bind.

## Build

```sh
make
```

The `Makefile` builds against `/lib/modules/$(uname -r)/build` by
default. To target a different tree:

```sh
make KDIR=/path/to/kernel/build
```

`make install` runs `modules_install` and `depmod -a`.

## Load

```sh
sudo insmod emdoor-wmi.ko
sudo depmod -a
```

udev auto-loads on boot once the module is installed; the DMI
whitelist gates the bind.

## Diagnostics

```sh
# Confirm the WMI devices exist
ls /sys/bus/wmi/devices/ | grep -E '4BCA|8600ACCE|6B40A935'

# List valid modes
cat /sys/class/leds/emdoor:rgb:mode/modes

# Check battery charge ratio
cat /sys/bus/wmi/devices/6B40A935-7FEF-42B6-B08D-6C79B57D6C35-11/charge_micp
cat /sys/bus/wmi/devices/6B40A935-7FEF-42B6-B08D-6C79B57D6C35-11/charge_mxcp

# Check power limit override interface
ls /sys/devices/platform/emdoor-power-limits/power_limits/

# Tail the kernel ring buffer
sudo dmesg -w | grep -E 'emdoor|EmdAcpi'
```

Probe failure modes and what they mean:

| Symptom | Likely cause |
| --- | --- |
| No `emdoor-power`, `emdoor-charge`, or `emdoor-kbd` devices | DMI whitelist did not match (load with `force_load=1` to confirm) |
| `EmdAcpi power mode bound (...) WMI path` | Healthy firmware, fast path |
| `EmdAcpi power mode bound (...) EC IO fallback` | `WMBF` is broken; driver routed around it via `PWMD` |
| `EmdAcpi battery charge ratio bound (...) EC IO fallback` | `WMDD` is broken; driver routed around it via EC MICP/MXCP registers |
| `unsupported keyboard type 0xNN` | `KBTE` is not 2; this driver only supports the 4-zone layout |
| `WMDA DTID=2 returned RTS0=N` | First-byte check failed; firmware returned an unexpected value (do not retry, the firmware is misbehaving) |

## License

SPDX-License-Identifier: GPL-2.0-or-later

The driver is distributed under the GNU General Public License
version 2 or later, the same terms as the Linux kernel.

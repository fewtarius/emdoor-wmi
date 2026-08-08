# AGENTS.md

Developer reference for emdoor-wmi. Project methodology in `.clio/instructions.md`.

## Project Overview

**emdoor-wmi** is an out-of-tree Linux kernel module for the Emdoor EmdAcpi ACPI firmware on Nimo, Xuanpai, and MetaMech laptop PCs.

- **Language:** C (Linux kernel)
- **Architecture:** Two platform drivers (power + charge) and one WMI driver (keyboard) in a single module. EC IO only; no WMI evaluation for power or charge.
- **License:** GPL-2.0-or-later
- **Target kernel:** Linux 6.x+
- **Companion:** `../ecwmi/` is a separate diagnostic-only module.

Prior firmware analysis lives in `../analysis/` (DSDT dump, EC RAM map, WMI call table). Refer to those when working on EC registers or WMI methods.

## Layout

```
emdoor-wmi.c          single source file, all 3 subsystem drivers
Makefile              kbuild entry point
test_zones.sh         interactive keyboard zone identifier
README.md             user-facing: what it does, sysfs paths, build, load

../ecwmi/ecwmi.c      ECWR trigger + raw PWMD/TLID sysfs
../ecwmi/Makefile
../analysis/EC-RAM-MAP.md            DSDT field map (offset + name)
../analysis/ACPI-FIRMWARE-EXTRACTION.md  WMI call table + WMDD internals
../analysis/LINUX-IMPLEMENTATION-MAP.md  design notes from before the driver existed
../acpi_dump/DSDT.dsl                decompiled DSDT (canonical AML reference)
```

Why one source file? The three subsystems share EC IO helpers (`emd_ec_read`/`write`, `emd_cms_write`), the DMI whitelist, and `emd_wmi_call` (used only by the keyboard). No build complexity worth splitting.

## Driver Structure

```
emdoor-wmi.ko
├── emdoor-power          platform_device `emdoor-power`
│   ├── platform_profile  /sys/class/platform_profile/platform-profile-0/
│   └── hwmon `emdoor_fan`
│       ├── fan1_input, fan_mode (RO/RW)
│       └── power[1-6]_cap (WO, microWatts via CMS mailbox)
│
├── emdoor-charge         platform_device `emdoor-charge`
│   └── power_supply ext on ACPI BAT0/1/2/3/4
│       charge_control_start_threshold, charge_control_end_threshold
│
└── emdoor-kbd            WMI GUID 8600ACCE-FB9B-443E-86F4-3C867398AAE5
    ├── 6× emdoor:multicolor:{zone1..4,bar-left,bar-right}
    └── emdoor:rgb:mode (control surface for animation mode)
```

Three modules in one: power + charge are platform drivers backed by `platform_device_register_simple`; keyboard is a WMI driver because that's the only way to reach `WMDA`.

`module_init` order: DMI check -> platform_device + driver for power -> platform_device + driver for charge -> wmi_driver_register for keyboard. Cleanup is reverse order.

## EC Registers

Authoritative source: `../analysis/EC-RAM-MAP.md` (DSDT line 8680). Registers the driver touches:

| Offset | Name | Purpose |
| --- | --- | --- |
| 0x30 | XXTT | Fan control arm; write 0x11 before TLID |
| 0x32 | TLID | Fan mode (1=AUTO, 2=MAX) |
| 0x76/0x77 | FN1H/FN1L | Fan 1 RPM (big-endian) |
| 0x78 | ECWR | Power-limit refresh trigger |
| 0x7C | PWMD | Power mode (1=quiet, 2=balance, 3=perf) |
| 0xBB | MICP | Min charge percent |
| 0xBC | MXCP | Max charge percent |

CMS mailbox (for `power[1-6]_cap`): cmd port 0x72, data port 0x73, ALIB command 0x0C. Sequence: `outb(0x0C, 0x72) -> udelay(1) -> outb(reg, 0x73) -> udelay(1) -> outl(value, 0x73) -> udelay(10)`.

The DSDT field labels for MICP/MXCP (line 8766) and `../analysis/EC-RAM-MAP.md` swap the names of 0xBB and 0xBC relative to firmware behaviour and the WMDD source code. The driver's register definitions match actual firmware semantics (0xBB=MICP, 0xBC=MXCP); the firmware's reset defaults are 0xBB=0, 0xBC=100, which only makes sense with that mapping.

## Why EC IO Only

The EmdAcpi `WMxx` methods declare a 63-bit `BufferField` at offset `0x20` of an 8-byte buffer - structurally out of bounds. Linux ACPICA rejects with `AE_AML_BUFFER_LIMIT`, generating an `ACPI BIOS Error` log every probe. We cannot patch the BIOS.

We route around the broken methods:
- `WMBF` (power-mode): value is at EC PWMD (0x7C). Driver writes/reads PWMD directly; `WMBF` is never evaluated.
- `WMDD` (charge ratio): EC's MICP/MXCP (0xBB/0xBC) carry the same values. Direct EC IO.
- `WMDA` (keyboard): well-formed (0x50-byte local buffer). Evaluated via `wmidev_evaluate_method()`.

There is no `quirk_broken_wmbf` flag any more. Power and charge never probe broken WMI methods; no ACPI errors at load.

## Code Style

Linux kernel coding style (tabs, K&R braces, 100-col soft limit, C89-compatible). `pr_fmt` at top, `dev_*` for per-device logs, `pr_*` for module-level. Pack on-wire structs with `__packed`. Use `devm_*` for everything with a clear device lifetime.

Naming:

| Prefix | Meaning |
| --- | --- |
| `emd_` | module-local functions |
| `EMD_` | module-local constants/macros |
| `emd_power_*` | power subsystem |
| `emd_charge_*` | charge subsystem |
| `emd_kbd_*` | keyboard subsystem |

## Patterns

### EC IO

```c
static int emd_ec_read(struct device *dev, u8 reg, u8 *val)
{
    int ret = ec_read(reg, val);
    if (ret)
        dev_err(dev, "EC read 0x%02x failed: %d\n", reg, ret);
    return ret;
}
```

`ec_read`/`ec_write` are exported by `CONFIG_ACPI_EC`. The ACPI EC driver serialises them internally; no extra locking needed. CMS mailbox writes (ports 0x72/0x73) bypass the EC driver and need their own udelay sequence.

### hwmon chip info

Standard hwmon attributes via `hwmon_chip_info` + `hwmon_ops` callbacks. Custom attributes that don't fit standard channels go in an `extra_groups` array:

```c
static const struct hwmon_ops emd_hwmon_ops = {
    .is_visible = emd_hwmon_is_visible,    /* returns 0 or S_IWUSR/S_IRUSR */
    .read       = emd_hwmon_read,
    .write      = emd_hwmon_write,
};

priv->hwmon_dev = devm_hwmon_device_register_with_info(
    &pdev->dev, "emdoor_fan", priv,
    &emd_hwmon_chip_info, emd_hwmon_extra_groups);
```

Channel configs: `HWMON_F_INPUT` for fan RPM, `HWMON_P_CAP` (write-only, microWatts) for power caps.

### power_supply extension on ACPI BAT0

Use `devm_battery_hook_register()` to find the ACPI battery and attach a `power_supply_ext`. Adds properties to the existing BAT0 instead of registering a duplicate:

```c
static int emd_charge_add_battery(struct power_supply *psy,
                                  struct acpi_battery_hook *hook)
{
    struct emd_charge_priv *priv = container_of(hook, struct emd_charge_priv, hook);
    if (priv->hooked_psy)
        return 0;
    return power_supply_register_extension(psy, &emd_charge_ext,
                                           priv->dev, priv);
}
```

The extension struct declares the property list plus `get_property`/`set_property`/`property_is_writeable` callbacks. Standard tools (`tlp`, `auto-cpufreq`) find the properties via the existing BAT0 sysfs unchanged.

### WMI call (keyboard only)

`emd_wmi_call` wraps `wmidev_evaluate_method()` for `WMDA` cases 2 (status) and 3 (set zone). The keyboard is the only subsystem that uses WMI. `AE_AML_BUFFER_LIMIT` surfaces as `-EIO`; nothing falls back to EC for keyboard.

### LED multicolor

`mc->led_cdev.brightness` MUST be `1` (max_brightness) at init. The kernel's `multi_intensity_store` calls `led_set_brightness(cdev, cdev->brightness)` after caching new subled intensities. If the cached brightness were 0, every subsequent write would silently reduce to "off". See `emd_kbd_init_led` comment.

## Boot-Time Fan Fix

`emd_power_probe` calls `emd_fan_set_mode(pdev, EMD_FAN_AUTO)` after the initial PWMD read. EC's `TLID` register is undefined at first power-on; the BIOS leaves it at MAX; firmware idle is AUTO. Forcing AUTO eliminates the mismatch between firmware idle behaviour and the value visible through `fan_mode`.

## Removal Pattern

Keyboard driver: `priv->removing = true` in `remove`. All LED class devices are devm-managed; they unregister after `remove` returns. The driver intentionally does **not** touch hardware in `remove`; firmware holds the last configuration and there is no per-zone RGB readback path. `rmmod`/`insmod` does not stomp user colours.

## DMI Whitelist

`emd_dmi_table[]` and `ecwmi_dmi_table[]` are identical (one board today: Nimo Direct INC. / N161L). `force_load=1` skips the whitelist. **Do not** add `force_load=1` to a persistent config - add an explicit DMI entry for new vendors instead.

## Testing

### Build

```bash
# Production
cd /home/deck/control/emdoor-wmi
make KCFLAGS="-Wall -Wextra"

# Diagnostic
cd /home/deck/control/ecwmi
make KCFLAGS="-Wall -Wextra"
```

### Hardware smoke tests

```bash
# Power mode
echo balanced | sudo tee /sys/class/platform_profile/platform-profile-0/profile
cat /sys/class/platform_profile/platform-profile-0/profile

# Fan (find hwmonN first: ls /sys/class/hwmon/)
HWMON=/sys/class/hwmon/hwmonN
cat $HWMON/fan1_input
echo 1 | sudo tee $HWMON/fan_mode     # AUTO
echo 2 | sudo tee $HWMON/fan_mode     # MAX - fans should audibly spin up

# Power limits
echo 15000000 | sudo tee $HWMON/power1_cap   # 15 W PL1

# Battery charge
echo 80 | sudo tee /sys/class/power_supply/BAT0/charge_control_end_threshold
cat /sys/class/power_supply/BAT0/charge_control_start_threshold

# RGB
echo 255 0 0 | sudo tee /sys/class/leds/emdoor:multicolor:zone1/multi_intensity
echo wave | sudo tee /sys/class/leds/emdoor:rgb:mode/mode

# Kernel log
sudo dmesg -w | grep -E 'emdoor|EmdAcpi|ecwmi'
```

### Probe messages

| Message | Meaning |
| --- | --- |
| `EmdAcpi power bound (current profile=N, fan=AUTO)` | EC IO working |
| `EmdAcpi charge control attached to BAT0` | Extension registered |
| `EmdAcpi keyboard backlight bound (type=4zone max=0 mode=always)` | Keyboard bound |
| No `ACPI BIOS Error` at probe | Confirmed: broken WMI methods not evaluated |

### Test zones (interactive)

```bash
sudo ./test_zones.sh
```

Walks each zone with a unique colour; press enter after identifying.

## Commit Format

Linux kernel conventions:

```
emdoor-wmi: <subsystem>: <one-line description>

<What changed and why. Reference EC register offsets if applicable.>

Tested-by: <hardware owner and platform>
Signed-off-by: <author>
```

`Signed-off-by` mandatory for kernel work (DCO). Use `git commit -s`.

### Pre-commit checklist

- [ ] `make KCFLAGS="-Wall -Wextra"` builds clean (no new warnings) for both modules
- [ ] `modinfo` shows correct metadata
- [ ] No commented-out code blocks (`test_zones.sh` is the only intentional shell exception)
- [ ] Hardware smoke tests pass (Power, Fan, Battery, RGB, ecwmi if applicable)
- [ ] `Signed-off-by` line

## Anti-Patterns

| Don't | Why | Do |
| --- | --- | --- |
| Probe broken WMI methods | Generates `ACPI BIOS Error` every probe | Use EC IO directly for power and charge; only WMDA is well-formed |
| Use a WMI GUID path as the primary sysfs target | Breaks userspace tools that look in `/sys/class/...` | Use hwmon / power_supply / platform_profile / led-class paths |
| Add `force_load=1` to persistent config | Whitelist exists for a reason | Add DMI entry for new vendors |
| Patch the BIOS | Out of scope | Route around the AML bug |
| Touch hardware in `remove` | Cannot recover state | Set `removing = true`, leave hardware alone |
| Set `mc->led_cdev.brightness = 0` at init | Subsequent writes become no-op | Use `brightness = 1`; comment why |
| Use the FACS all-zones selector (0x0F) | Re-arms LLBR channel, bleeds into side bars | Per-zone writes keep channels isolated |

## Quick Reference

```bash
# Build & install
make && sudo make install

# Load
sudo modprobe emdoor-wmi          # after install
sudo insmod emdoor-wmi.ko         # in-tree build

# Diagnostic companion
cd ../ecwmi && make && sudo insmod ecwmi.ko

# Tail kernel log
sudo dmesg -w | grep -E 'emdoor|EmdAcpi|ecwmi'

# Power mode
cat /sys/class/platform_profile/platform-profile-0/profile

# Fan + power limits
HWMON=/sys/class/hwmon/hwmonN
cat $HWMON/fan1_input
echo 1 | sudo tee $HWMON/fan_mode

# Battery
echo 80 | sudo tee /sys/class/power_supply/BAT0/charge_control_end_threshold

# Keyboard
echo wave | sudo tee /sys/class/leds/emdoor:rgb:mode/mode
echo 255 0 0 | sudo tee /sys/class/leds/emdoor:multicolor:zone1/multi_intensity

# Test zones
sudo ./test_zones.sh
```

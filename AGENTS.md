# AGENTS.md

**Version:** 1.0
**Date:** 2026-08-05
**Purpose:** Technical reference for emdoor-wmi development (methodology in .clio/instructions.md)

---

## Project Overview

**emdoor-wmi** is an out-of-tree Linux kernel module for the Emdoor EmdAcpi ACPI firmware
used in Nimo, Xuanpai, and MetaMech laptop PCs.

- **Language:** C (Linux kernel)
- **Architecture:** Two WMI drivers in a single module: power-mode (`platform_profile`) and
  4-zone RGB keyboard (`led-class-multicolor`)
- **License:** GPL-2.0-or-later (matches Linux kernel)
- **Target kernel:** Linux 6.x+

### Why this driver exists

The vendor firmware's EmdAcpi WMxx control methods declare a 63-bit `BufferField` at offset
`0x20` of an 8-byte argument buffer, which is structurally out-of-bounds. Linux ACPICA rejects
the declaration with `AE_AML_BUFFER_LIMIT` before any case body runs; Microsoft ACPI.SYS does
not enforce this check. The unreferenced field is dead code. We cannot patch the BIOS or
relax the interpreter, so we route around the broken methods:

- `WMBF` (power-mode): the actual value lives in the EC's `PWMD` field at OperationRegion
  offset `0x7C`. The driver tries `WMBF` first; on `AE_AML_BUFFER_LIMIT` it sets a per-device
  quirk flag and reads/writes `PWMD` directly for the lifetime of the device. The only thing
  the EC fallback omits is the fan-curve re-tuning that `E004()` performs after
  `PWMD = ECPM`; `platform_profile` is unaffected.
- `WMDA` (keyboard): the method is well-formed (0x50-byte local buffer instead of 0x08),
  so evaluate it directly via `wmidev_evaluate_method()`.

Because `WMBF` may evaluate on healthy firmware and fail on broken firmware, the driver probes
the WMI path on `WMBF` first and switches to the EC path transparently. User-visible behavior
is identical in either case.

---

## Quick Setup

```bash
# Build against the running kernel
make

# Build against a different kernel tree
make KDIR=/path/to/kernel/build

# Install
sudo make install

# Load
sudo modprobe emdoor-wmi        # after install
sudo insmod emdoor-wmi.ko       # in-tree build

# Confirm WMI devices exist
ls /sys/bus/wmi/devices/ | grep -E '4BCA|8600ACCE'

# Tail kernel ring buffer
sudo dmesg -w | grep -E 'emdoor|EmdAcpi'
```

Manual zone identification (interactive, requires hardware):

```bash
sudo ./test_zones.sh
```

---

## Architecture

```
                 emdoor-wmi.ko
                 /           \
                /             \
               v               v
         emd_power        emd_kbd
         (WMI driver)     (WMI driver)
             |                |
             |                v
             |        led-class-multicolor
             |                |
             |                +-- 6 zones:
             |                      4 keyboard regions (FACS bits 0-3)
             |                      2 chassis side bars (FLAB bits 4-5)
             v
      platform_profile
             |
             v
         WMBF (probe first)
             |
             v healthy
         WMI path
             |
             v no (AE_AML_BUFFER_LIMIT)
         EC IO fallback (PWMD register 0x7C)
```

Two subsystems share a single source file but register as independent WMI drivers:

| Driver | GUID | Bound name |
|--------|------|------------|
| `emd_power_driver` | `4BCA6480-4D03-4674-84CB-26B4C8F5CFC2` | `emdoor-power` |
| `emd_kbd_driver` | `8600ACCE-FB9B-443E-86F4-3C867398AAE5` | `emdoor-kbd` |

Both use `PROBE_PREFER_ASYNCHRONOUS` and `no_singleton = true`. The module init function
(`emd_wmi_init`) runs the DMI whitelist check before registering either driver.

---

## Directory Structure

| Path | Purpose |
|------|---------|
| `emdoor-wmi.c` | Single source file with both subsystem drivers |
| `Makefile` | kbuild entry point |
| `test_zones.sh` | Interactive zone identifier (turns each zone on with a unique color) |
| `README.md` | User-facing documentation |
| `.gitignore` | Kernel build artifacts + CLIO internal state |

**Why one file?** The module is small enough to keep in a single translation unit. Both
subsystems share the GUID macros, the WMI helper (`emd_wmi_call`), and the DMI whitelist.
There is no build complexity worth splitting.

---

## Code Style

### Linux Kernel Coding Style

This is a Linux kernel module. Follow the kernel coding style strictly.

- **Tabs** for indentation (8-wide tabs, kernel default)
- **K&R braces** - opening brace on the same line for control flow, newline for functions
- **Line length** - 100 columns is the soft limit; respect 80 when reasonable
- **C89-compatible** constructs preferred (unless a newer feature is genuinely needed)
- **No typedef structs** for short-lived types; use `struct foo` directly
- **SPDX license header** at the top of every source file (`GPL-2.0-or-later`)
- **Module metadata** at the bottom: `MODULE_AUTHOR`, `MODULE_DESCRIPTION`, `MODULE_LICENSE`

### Naming Conventions

| Prefix | Meaning | Examples |
|--------|---------|----------|
| `emd_` | Module-local functions | `emd_wmi_call`, `emd_dmi_check` |
| `EMD_` | Module-local constants/macros | `EMD_NUM_ZONES`, `EMD_FA00_FACS_ZONE1` |
| `emd_power_*` | Power subsystem | `emd_power_priv`, `emd_power_driver` |
| `emd_kbd_*` | Keyboard subsystem | `emd_kbd_priv`, `emd_kbd_driver` |

### Kernel Idioms

Use managed allocations throughout:

```c
/* Allocates on attach, releases on detach automatically */
priv = devm_kzalloc(&wdev->dev, sizeof(*priv), GFP_KERNEL);
if (!priv)
    return -ENOMEM;
```

This is the standard pattern. Use `devm_*` for everything that has a clear lifetime tied to
the device. Avoid manual `kfree` unless you have a documented reason.

### On-Wire Structs

Pack all on-wire / ACPI-aware structs with `__packed`:

```c
struct emd_return {
    u16 rt_code;
    u16 value;
    u8  reserved[4];
} __packed;

struct emd_bst_input {
    u8 fa00;        /* bits 0-3: keyboard zone, bits 4-5: side bar */
    u8 r;
    u8 g;
    u8 b;
    u8 control[2];  /* firmware controls, 0xFE to preserve */
    u8 effect;      /* FA06: effect mode */
    u8 on;          /* FA07: 0 = turn off, non-zero = leave on */
} __packed;
```

Tied to the firmware wire format. Do not add fields without firmware documentation.

### Logging

Use `pr_fmt` at the top of the source file and prefer `dev_*` for device-attached logs:

```c
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

dev_info(&wdev->dev, "EmdAcpi power mode bound (current=%d, %s)\n",
         probe_val,
         priv->quirk_broken_wmbf ? "EC IO fallback" : "WMI path");
dev_err(&wdev->dev, "WMDA DTID=2 returned RTS0=%u (expected 1)\n", resp[2]);
dev_warn(&wdev->dev, "unknown power-mode value %d\n", val);
dev_dbg(&wdev->dev, "WMI method 0x%02x: %s\n", sub, acpi_format_exception(status));
```

Use `dev_*` for per-device messages (the kernel stamps with the device), `pr_*` for
module-level messages.

### Per-Device Quirk Flag

The `quirk_broken_wmbf` flag is a per-device pattern: probe the WMI path once, and if it
fails with `AE_AML_BUFFER_LIMIT`, latch the flag and use the EC path for the lifetime of the
device. Don't re-probe every call.

```c
priv->quirk_broken_wmbf = true;
return emd_ec_pwmd_write(priv, (u8)profile);
```

### Removal Pattern

The keyboard driver implements `remove` to unregister LED class devices. The `removing`
flag prevents writes during unload - the firmware holds the last configuration and there is
no per-zone RGB readback path:

```c
if (priv->removing)
    return 0;
```

The driver intentionally does NOT touch hardware in `remove`. Reload does not stomp the
user's colors and the hardware state survives `rmmod`/`insmod` cycles.

---

## Module Parameters

| Parameter | Access | Description |
|-----------|--------|-------------|
| `force_load` | bool, 0444 | Skip DMI whitelist (diagnostics only) |

The whitelist gates the bind intentionally. `force_load=1` is not a workaround for
unsupported hardware; if the firmware reports `KBTE != 2` the driver still refuses to bind.

---

## Probe Behaviour

### Power (`emd_power_probe`)

1. Probe the WMI path - if `WMBF` is broken, the first call fails with `AE_AML_BUFFER_LIMIT`.
   `emd_power_get()` notices that, sets `quirk_broken_wmbf`, and retries via EC IO so the
   initial read returns a usable value.
2. Register with `platform_profile` framework.
3. Log the path taken.

### Keyboard (`emd_kbd_probe`)

1. Read the keyboard type via `WMDA` case 2 (DTID=2). Anything other than `KBTE=2` (4-zone)
   is rejected with `-ENODEV`.
2. Register six multicolor LED class devices.
3. Register the `type`, `mode`, and `modes` attributes.
4. Do NOT apply an initial RGB state - leave hardware exactly as the firmware last had it.

---

## Common Patterns

### WMI Method Call

```c
static int emd_wmi_call(struct wmi_device *wdev, u8 sub,
                       const void *in, size_t in_len,
                       void *out, size_t out_size)
{
    struct acpi_buffer result = { ACPI_ALLOCATE_BUFFER, NULL };
    struct acpi_buffer in_buf = { in_len, (void *)in };
    acpi_status status;
    union acpi_object *obj;
    int ret = 0;

    status = wmidev_evaluate_method(wdev, 0, sub, &in_buf, &result);
    if (ACPI_FAILURE(status)) {
        if (status == AE_AML_BUFFER_LIMIT)
            ret = -EBUSY;       /* vendor BIOS bug, caller may fall back */
        else
            ret = -EIO;
        kfree(result.pointer);
        return ret;
    }
    /* ... copy out, kfree(result.pointer), return ret ... */
}
```

`AE_AML_BUFFER_LIMIT` is the vendor BIOS bug. Surface it as `-EBUSY` so callers can decide
whether to fall back to the EC path.

### Quirk Fallback Pattern

```c
ret = emd_wmi_call(priv->wdev, (u8)profile, NULL, 0, &r, sizeof(r));
if (ret == 0 && r.rt_code == EMD_RT_OK)
    return 0;

priv->quirk_broken_wmbf = true;     /* latch: don't re-probe */
return emd_ec_pwmd_write(priv, (u8)profile);
```

### EC IO Access

```c
ret = ec_read(EMD_EC_REG_PWMD, val);
if (ret)
    dev_err(&priv->wdev->dev,
            "EC read of PWMD (0x7C) failed: %d\n", ret);
```

EC IO requires `CONFIG_ACPI_EC`. The driver has no special handling for EC failure beyond
logging.

### sysfs Show

```c
static ssize_t type_show(struct device *dev, struct device_attribute *attr,
                        char *buf)
{
    struct emd_kbd_priv *priv = dev_get_drvdata(dev);
    return sysfs_emit(buf, "%s\n", emd_kbte_str[priv->kbte]);
}
static DEVICE_ATTR_RO(type);
```

Use `sysfs_emit`, not `snprintf`. Always include the trailing newline.

### sysfs Store

```c
static ssize_t mode_store(struct device *dev, struct device_attribute *attr,
                         const char *buf, size_t count)
{
    struct emd_kbd_priv *priv = dev_get_drvdata(dev);
    u8 mode;
    int ret;

    ret = emd_kbd_mode_lookup(buf, &mode);
    if (ret)
        return ret;
    /* ... apply, return count on success ... */
    return ret ?: count;
}
```

Validate, apply, return `count` on success or a negative error. Don't leak partial state.

### Multicolor LED Registration

```c
mc->subled_info = led->subleds;
mc->led_cdev.name = emd_kbd_led_names[zone];
mc->led_cdev.brightness_set_blocking = emd_kbd_mc_brightness_set;
mc->led_cdev.max_brightness = 1;
mc->led_cdev.color = LED_COLOR_ID_RGB;
mc->led_cdev.brightness = 1;          /* NOT 0 — see header comment */
/* intensity defaults to 0, not white */
mc->num_colors = 3;
```

**Why `brightness = 1`?** The kernel's `multi_intensity_store` calls
`led_set_brightness(cdev, cdev->brightness)` after caching the new subled intensities. If
the cached brightness were `0`, every subsequent write would silently reduce to "off" because
the handler would be called with `brightness=0`. With `brightness=1`, brightness is left
untouched by colour writes and the cached intensity reaches the EC.

---

## Testing

### Build

```bash
# Build against running kernel
make

# Strict warnings
make KCFLAGS="-Wall -Wextra"

# Module info
modinfo emdoor-wmi.ko
```

### Manual Test (hardware required)

```bash
# Interactive zone identification
sudo ./test_zones.sh

# Power mode round-trip
echo performance | sudo tee \
    /sys/bus/wmi/devices/4BCA6480-4D03-4674-84CB-26B4C8F5CFC2-10/platform-profile/platform-profile-0/profile
cat /sys/bus/wmi/devices/4BCA6480-4D03-4674-84CB-26B4C8F5CFC2-10/platform-profile/platform-profile-0/profile

# Keyboard mode
echo always | sudo tee \
    /sys/bus/wmi/devices/8600ACCE-FB9B-443E-86F4-3C867398AAE5-12/mode
echo 255 0 0 | sudo tee \
    /sys/class/leds/emdoor:multicolor:zone1/multi_intensity
```

### Probe Failure Modes

| Symptom | Likely cause |
|---------|--------------|
| No `emdoor-power` or `emdoor-kbd` devices | DMI whitelist did not match (load with `force_load=1` to confirm) |
| `EmdAcpi power mode bound (...) WMI path` | Healthy firmware, fast path |
| `EmdAcpi power mode bound (...) EC IO fallback` | `WMBF` is broken; driver routed around it via `PWMD` |
| `unsupported keyboard type 0xNN` | `KBTE` is not 2; this driver only supports the 4-zone layout |
| `WMDA DTID=2 returned RTS0=N` | First-byte check failed; firmware returned an unexpected value (do not retry, the firmware is misbehaving) |

### What Cannot Be Tested In-CI

This driver's test surface is hardware-dependent. CI cannot run the module. Hardware owners
must verify:

- Module binds on target hardware
- `platform_profile` set/get round-trip
- All 6 LED zones respond to `multi_intensity`
- `mode` round-trip across all values
- `rmmod`/`insmod` preserves hardware state

---

## Commit Format

For kernel-targeted work, follow Linux kernel commit conventions:

```
emdoor-wmi: <subsystem>: <one-line description>

<What changed and why. Reference the AML/EC layout if relevant.>

Tested-by: <hardware owner and platform>
Signed-off-by: <author>
```

`Signed-off-by` is mandatory for kernel work (DCO). Use `git commit -s`.

### Pre-Commit Checklist

- [ ] `make` builds clean (no new warnings)
- [ ] `modinfo emdoor-wmi.ko` shows correct metadata
- [ ] Tested on target hardware (if you have it)
- [ ] No dead code or commented-out blocks (the `test_zones.sh` script is the only intentional one; comment-out blocks in `emdoor-wmi.c` are technical debt - close them out before committing)
- [ ] Commit message references the AML/EC field if applicable
- [ ] `Signed-off-by` line

---

## Documentation

### Files

| File | Purpose |
|------|---------|
| `README.md` | User-facing: hardware support, sysfs attributes, build, load, diagnostics |
| `emdoor-wmi.c` header comment | Author/protocol overview, AML bug explanation |
| `AGENTS.md` | This file - developer reference |
| `.clio/instructions.md` | Methodology (Unbroken Method) |

### Updating Docs

When changing code, update:

| Change | Doc Updates |
|--------|-------------|
| New sysfs attribute | `README.md` (sysfs table) |
| New keyboard mode | `README.md` (mode table + flat list) |
| New hardware support | `README.md` (supported hardware table) + `emd_dmi_table` |
| New module parameter | `README.md` (module parameters table) |
| New failure mode | `README.md` (diagnostics table) |

**Rule:** Full rewrite, never changelog patches. Re-read the affected section and rewrite it.

---

## Anti-Patterns (What NOT To Do)

| Anti-Pattern | Why It's Wrong | What To Do |
|--------------|----------------|------------|
| Add new color/mode without firmware documentation | Wire format is fixed by firmware | Match the existing enums; document FA00/FA06/FA07 |
| Add `force_load=1` to a persistent config | Whitelist exists for a reason | Use DMI whitelist for new vendors |
| Touch hardware in `remove` | Cannot recover state; firmware holds the truth | Set `removing = true`, log leave-behind |
| Re-issue the BST2 baseline between zones | Clobbers every other zone; user sees flicker | Use FA06=0xFE for per-zone writes |
| Use `printk` directly without `pr_fmt` | No module prefix in logs | Use `pr_fmt` + `dev_*` macros |
| Add `TODO` / `FIXME` comments | Finish the work | Resolve before committing |
| Patch the BIOS | Out of scope | Route around the AML bug |
| Use the FACS all-zones selector (0x0F) | Re-arms LLBR channel, bleeds into side bars | Per-zone writes keep channels isolated |
| Probe WMI on every call instead of latching the quirk | Slow + race-prone | Latch `quirk_broken_wmbf` on first failure |
| Set `mc->led_cdev.brightness = 0` at init | Every subsequent write becomes a no-op | Use `brightness = 1`; comment why |

---

## Quick Reference

```bash
# Build & install
make && sudo make install

# Load
sudo modprobe emdoor-wmi          # after install
sudo insmod emdoor-wmi.ko         # in-tree build

# Watch the kernel log
sudo dmesg -w | grep -E 'emdoor|EmdAcpi'

# Power mode
PROFILE=/sys/bus/wmi/devices/4BCA6480-4D03-4674-84CB-26B4C8F5CFC2-10/platform-profile/platform-profile-0
cat $PROFILE/profile
echo performance | sudo tee $PROFILE/profile

# Keyboard
KBD=/sys/bus/wmi/devices/8600ACCE-FB9B-443E-86F4-3C867398AAE5-12
cat $KBD/modes
echo always | sudo tee $KBD/mode
echo 255 0 0 | sudo tee /sys/class/leds/emdoor:multicolor:zone1/multi_intensity

# Test zones
sudo ./test_zones.sh
```

---

*For project methodology and workflow, see .clio/instructions.md*

// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EmdAcpi WMI driver for Nimo, Xuanpai, and MetaMech laptops
 *
 * Supports the EmdAcpi power-mode, battery charge ratio, and six-zone
 * RGB keyboard WMI devices on the EmdAcpi firmware family. The driver
 * is restricted to three known products; see `emd_dmi_table` below.
 * Hardware owners with new products in this firmware family can submit
 * their DMI strings via a PR to extend the whitelist.
 *
 * 14 of the 15 EmdAcpi WMxx control methods declare a 63-bit
 * BufferField at offset 0x20 of an 8-byte buffer, which is structurally
 * out of bounds. Linux ACPICA rejects the declaration with
 * AE_AML_BUFFER_LIMIT before any case body runs so we route around
 * the broken methods:
 *
 *   - WMBF (BF_POWER_MODE, platform_profile): the actual mode is held
 *     in the EC's PWMD field at OperationRegion offset 0x7C. The fan
 *     curve re-tuning that E004() does after `PWMD = ECPM` is skipped;
 *     platform_profile doesn't care about fan curves.
 *   - WMDA (DA_APP_KB_LED backend): parses cleanly with a
 *     0x50-byte local buffer. Use wmidev_evaluate_method() directly.
 *   - WMDD (BatteryChargeRatio): parses cleanly with 8-byte buffer.
 *
 * Keyboard RGB protocol (BST2 via WMDA case 3):
 *
 * The keyboard has 6 logical zones:
 *   Zones 0-3: Keyboard zones (FACS bits 0-3 of FA00)
 *   Zones 4-5: Left/Right side bars (FLAB bits 4-5 of FA00)
 *
 * Modes (FA06):
 *   0x00 (always)    - Static color per zone
 *   0x01-0x08        - Dynamic effects (twinkle, wave, breath, etc.)
 *   0xFE (off)       - All LEDs off
 *   0xFF (reset)     - Hardware reset
 *
 * Static mode (0x00): Each zone addressed individually with its own FA00
 * selector and RGB values. Effect byte = 0xFE (skip) tells EC to use
 * per-zone RGB without dynamic effect processing.
 *
 * Dynamic modes (0x01-0x08): Single FA00=0 with effect byte selects the
 * animation; zone colors come from the first zone's multicolor LED
 * brightness components.
 *
 * Side bars (zones 4,5): FLAB=0x10 (left) / 0x20 (right) / 0x30 (both).
 * These use the LLBR channel via ECMB=0x40. Writing RGB=0 with FLAB
 * is the only reliable way to turn off the side bars - FA04 brightness
 * alone does not persist across subsequent BST2 calls.
 *
 * Copyright (C) 2026 fewtarius
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

/* Driver version for tracking installed module */
#define EMD_DRIVER_VERSION "20260807.1"

#include <linux/acpi.h>
#include <linux/errno.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/led-class-multicolor.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/platform_profile.h>
#include <linux/power_supply.h>
#include <linux/types.h>
#include <linux/wmi.h>

MODULE_VERSION(EMD_DRIVER_VERSION);
MODULE_DESCRIPTION("EmdAcpi power, battery charge ratio, power limit override, and six-zone RGB driver (Nimo, Xuanpai, MetaMech)");
MODULE_AUTHOR("fewtarius");
MODULE_LICENSE("GPL");

/* ------------------------------------------------------------------------- */
/* DMI whitelist                                                              */
/* ------------------------------------------------------------------------- */

static const struct dmi_system_id emd_dmi_table[] = {
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "Nimo Direct INC."),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "N161L"),
		},
		.driver_data = (void *)"nimo-axis-n161",
	},
	{}
};
MODULE_DEVICE_TABLE(dmi, emd_dmi_table);

static bool force_load;
module_param(force_load, bool, 0444);
MODULE_PARM_DESC(force_load,
		 "Skip DMI whitelist (for diagnostics on unrecognised hardware)");

static int emd_dmi_check(void)
{
	if (force_load)
		return 0;
	if (dmi_first_match(emd_dmi_table))
		return 0;
	pr_info("hardware not on DMI whitelist; load with force_load=1 to override\n");
	return -ENODEV;
}

/* Fan control constants */
#define EMD_FAN_AUTO                  1
#define EMD_FAN_MAX                   2

/*
 * Fan control via EC registers (from DSDT ECF2 OperationRegion):
 *   XXTT (0x03): Fan control command/data port. Write 0x11 to arm
 *                fan control before writing TLID.
 *   TLID (0x32): Fan mode parameter (1=auto, 2=max).
 *   FN1H (0x76): Fan 1 RPM high byte.
 *   FN1L (0x77): Fan 1 RPM low byte.
 *
 * The WMAF WMI method is broken on Linux ACPICA (CreateField at
 * bit offset 0x20 with 0x3F width extends past the 8-byte buffer,
 * causing AE_AML_BUFFER_LIMIT), so we use direct EC I/O instead.
 */
#define EMD_EC_REG_XXTT              0x03
#define EMD_EC_REG_TLID              0x32
#define EMD_EC_REG_FN1H              0x76
#define EMD_EC_REG_FN1L              0x77

static int emd_fan_get_speed(struct wmi_device *wdev, u16 *rpm)
{
	u8 fn1l, fn1h;
	int ret;

	ret = ec_read(EMD_EC_REG_FN1L, &fn1l);
	if (ret) {
		dev_err(&wdev->dev, "EC read of FN1L (0x%02x) failed: %d\n",
			EMD_EC_REG_FN1L, ret);
		return ret;
	}

	ret = ec_read(EMD_EC_REG_FN1H, &fn1h);
	if (ret) {
		dev_err(&wdev->dev, "EC read of FN1H (0x%02x) failed: %d\n",
			EMD_EC_REG_FN1H, ret);
		return ret;
	}

	*rpm = (fn1h << 8) | fn1l;
	return 0;
}

static int emd_fan_set_mode(struct wmi_device *wdev, u8 mode)
{
	u8 xxtt;
	int ret;

	if (mode != EMD_FAN_AUTO && mode != EMD_FAN_MAX)
		return -EINVAL;

	ret = ec_write(EMD_EC_REG_XXTT, 0x11);
	if (ret) {
		dev_err(&wdev->dev, "EC write of XXTT (0x%02x) failed: %d\n",
			EMD_EC_REG_XXTT, ret);
		return ret;
	}

	ret = ec_read(EMD_EC_REG_XXTT, &xxtt);
	if (ret) {
		dev_err(&wdev->dev, "EC read of XXTT (0x%02x) failed: %d\n",
			EMD_EC_REG_XXTT, ret);
		return ret;
	}
	if (xxtt != 0x11) {
		dev_err(&wdev->dev, "XXTT verification failed: expected 0x11, got 0x%02x\n",
			xxtt);
		return -EIO;
	}

	ret = ec_write(EMD_EC_REG_TLID, mode);
	if (ret) {
		dev_err(&wdev->dev, "EC write of TLID (0x%02x) failed: %d\n",
			EMD_EC_REG_TLID, ret);
		return ret;
	}

	ret = ec_read(EMD_EC_REG_TLID, &xxtt);
	if (ret) {
		dev_err(&wdev->dev, "EC read of TLID (0x%02x) failed: %d\n",
			EMD_EC_REG_TLID, ret);
		return ret;
	}
	if (xxtt != mode) {
		dev_err(&wdev->dev, "TLID verification failed: expected %d, got 0x%02x\n",
			mode, xxtt);
		return -EIO;
	}

	return 0;
}

/* Fan control sysfs attributes */
static ssize_t fan_speed_show(struct device *dev,
			      struct device_attribute *attr,
			      char *buf)
{
	struct wmi_device *wdev = to_wmi_device(dev);
	u16 rpm;
	int ret;

	ret = emd_fan_get_speed(wdev, &rpm);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", rpm);
}

static ssize_t fan_mode_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	u8 mode;
	int ret;

	ret = ec_read(EMD_EC_REG_TLID, &mode);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", mode);
}

static ssize_t fan_mode_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct wmi_device *wdev = to_wmi_device(dev);
	unsigned long mode;
	int ret;

	if (kstrtoul(buf, 0, &mode) || (mode != 1 && mode != 2))
		return -EINVAL;

	ret = emd_fan_set_mode(wdev, (u8)mode);
	if (ret)
		return ret;

	return count;
}

static DEVICE_ATTR_RO(fan_speed);
static DEVICE_ATTR_RW(fan_mode);

#define EMD_GUID_BF_POWER_MODE        "4BCA6480-4D03-4674-84CB-26B4C8F5CFC2"
#define EMD_GUID_DA_APP_KB_LED        "8600ACCE-FB9B-443E-86F4-3C867398AAE5"
#define EMD_GUID_BATTERY_CHARGE       "6B40A935-7FEF-42B6-B08D-6C79B57D6C35"
#define EMD_GUID_FAN_CONTROL          "D06385DE-BC53-4B9B-85DC-674936D9549F"

/* ------------------------------------------------------------------------- */
/* Shared helpers                                                            */
/* ------------------------------------------------------------------------- */

static int emd_wmi_call(struct wmi_device *wdev, u8 sub,
			const void *in, size_t in_len,
			void *out, size_t out_size)
{
	struct acpi_buffer result = { ACPI_ALLOCATE_BUFFER, NULL };
	struct acpi_buffer in_buf = { in_len, (void *)in };
	acpi_status status;
	union acpi_object *obj;
	int ret = 0;

	if (in_len && !in)
		return -EINVAL;

	status = wmidev_evaluate_method(wdev, 0, sub, &in_buf, &result);
	if (ACPI_FAILURE(status)) {
		if (status == AE_AML_BUFFER_LIMIT)
			ret = -EBUSY;
		else
			ret = -EIO;
		dev_dbg(&wdev->dev, "WMI method 0x%02x: %s\n", sub,
			acpi_format_exception(status));
		kfree(result.pointer);
		return ret;
	}

	if (out && out_size) {
		obj = result.pointer;
		if (!obj || obj->type != ACPI_TYPE_BUFFER ||
		    obj->buffer.length < out_size) {
			ret = -EIO;
			goto free;
		}
		memcpy(out, obj->buffer.pointer, out_size);
	}

free:
	kfree(result.pointer);
	return ret;
}

/* Standard 8-byte return layout used by most EmdAcpi methods */
struct emd_return {
	u16 rt_code;
	u16 value;
	u8  reserved[4];
} __packed;

#define EMD_RT_OK	0

/* ------------------------------------------------------------------------- */
/* Power mode (BF -> WMBF, with EC IO fallback for the broken BIOS)         */
/* ------------------------------------------------------------------------- */

/*
 * EC register map for power mode:
 *   PWMD (0x7C): Current power mode (1=quiet, 2=balance, 3=perf)
 *   ECWR (0x78): EC write register - writing triggers E4 (power mode change)
 * Battery Charge Ratio registers (shared EC OperationRegion):
 *   MICP (0xBB): Min charge percent
 *   MXCP (0xBC): Max charge percent
 */

#define EMD_POWER_PROFILE_PERF		3
#define EMD_POWER_PROFILE_BALANCE	2
#define EMD_POWER_PROFILE_QUIET		1
#define EMD_POWER_PROFILE_READ		4

#define EMD_EC_REG_PWMD			0x7C
#define EMD_EC_REG_MICP			0xBB
#define EMD_EC_REG_MXCP			0xBC
#define EMD_EC_REG_ECWR			0x78

struct emd_power_priv {
	struct wmi_device *wdev;
	bool quirk_broken_wmbf;
};

static int emd_ec_pwmd_read(struct emd_power_priv *priv, u8 *val)
{
	int ret;

	ret = ec_read(EMD_EC_REG_PWMD, val);
	if (ret)
		dev_err(&priv->wdev->dev,
			"EC read of PWMD (0x7C) failed: %d\n", ret);
	return ret;
}

static int emd_ec_pwmd_write(struct emd_power_priv *priv, u8 val)
{
	/*
	 * Write power mode to EC PWMD register (0x7C). The EC firmware
	 * monitors this register and applies the corresponding fan curve
	 * and power limits when it changes.
	 */
	int ret;

	ret = ec_write(EMD_EC_REG_PWMD, val);
	if (ret)
		dev_err(&priv->wdev->dev,
			"EC write of PWMD (0x7C) failed: %d\n", ret);
	return ret;
}

static int emd_ec_ecwr_write(struct emd_power_priv *priv, u8 val)
{
	/*
	 * Write to ECWR (0x78) to trigger the E4 interrupt handler in
	 * the EC firmware. This forces the EC to re-evaluate power mode
	 * and re-apply the corresponding PL1/PL2/PL3 limits. The value
	 * written is not used by the EC - any non-zero write triggers E4.
	 */
	int ret;

	ret = ec_write(EMD_EC_REG_ECWR, val);
	if (ret)
		dev_err(&priv->wdev->dev,
			"EC write of ECWR (0x78) failed: %d\n", ret);
	return ret;
}

static int emd_power_set(struct emd_power_priv *priv, int profile)
{
	struct emd_return r;
	int ret;

	if (profile != EMD_POWER_PROFILE_PERF &&
	    profile != EMD_POWER_PROFILE_BALANCE &&
	    profile != EMD_POWER_PROFILE_QUIET)
		return -EINVAL;

	/*
	 * Try WMI path first. If the BIOS WMBF method is broken (AE_AML_BUFFER_LIMIT
	 * from the out-of-bounds BufferField), fall back to EC IO.
	 */
	if (priv->quirk_broken_wmbf)
		return emd_ec_pwmd_write(priv, (u8)profile);

	ret = emd_wmi_call(priv->wdev, (u8)profile, NULL, 0, &r, sizeof(r));
	if (ret == 0 && r.rt_code == EMD_RT_OK)
		return 0;

	priv->quirk_broken_wmbf = true;
	return emd_ec_pwmd_write(priv, (u8)profile);
}

static int emd_power_get(struct emd_power_priv *priv, int *profile)
{
	struct emd_return r;
	u8 val;
	int ret;

	/*
	 * Read current power mode. Try WMI first, fall back to EC PWMD
	 * register on failure.
	 */

	if (priv->quirk_broken_wmbf)
		goto read_ec;

	ret = emd_wmi_call(priv->wdev, EMD_POWER_PROFILE_READ, NULL, 0,
			   &r, sizeof(r));
	if (ret == 0 && r.rt_code == EMD_RT_OK) {
		*profile = r.value;
		return 0;
	}

	priv->quirk_broken_wmbf = true;
read_ec:
	ret = emd_ec_pwmd_read(priv, &val);
	if (ret)
		return ret;
	*profile = val;
	return 0;
}

static int emd_power_profile_get(struct device *dev,
				 enum platform_profile_option *profile)
{
	struct emd_power_priv *priv = dev_get_drvdata(dev);
	int val, ret;

	ret = emd_power_get(priv, &val);
	if (ret)
		return ret;

	switch (val) {
	case EMD_POWER_PROFILE_PERF:
		*profile = PLATFORM_PROFILE_PERFORMANCE;
		break;
	case EMD_POWER_PROFILE_BALANCE:
		*profile = PLATFORM_PROFILE_BALANCED;
		break;
	case EMD_POWER_PROFILE_QUIET:
		*profile = PLATFORM_PROFILE_LOW_POWER;
		break;
	default:
		dev_warn(dev, "unknown power-mode value %d\n", val);
		return -EINVAL;
	}
	return 0;
}

static int emd_power_profile_set(struct device *dev,
				 enum platform_profile_option profile)
{
	struct emd_power_priv *priv = dev_get_drvdata(dev);

	switch (profile) {
	case PLATFORM_PROFILE_LOW_POWER:
		return emd_power_set(priv, EMD_POWER_PROFILE_QUIET);
	case PLATFORM_PROFILE_BALANCED:
		return emd_power_set(priv, EMD_POWER_PROFILE_BALANCE);
	case PLATFORM_PROFILE_PERFORMANCE:
		return emd_power_set(priv, EMD_POWER_PROFILE_PERF);
	default:
		return -EOPNOTSUPP;
	}
}

static int emd_power_profile_probe(void *drvdata, unsigned long *choices)
{
	set_bit(PLATFORM_PROFILE_LOW_POWER, choices);
	set_bit(PLATFORM_PROFILE_BALANCED, choices);
	set_bit(PLATFORM_PROFILE_PERFORMANCE, choices);
	return 0;
}

static const struct platform_profile_ops emd_power_profile_ops = {
	.probe = emd_power_profile_probe,
	.profile_get = emd_power_profile_get,
	.profile_set = emd_power_profile_set,
};

/*
 * ECWR sysfs attribute - write-only trigger for E4 interrupt.
 * Writing any value forces EC to re-apply power limits for current mode.
 * Used for debugging and manual limit refresh.
 */
static ssize_t ecwr_store(struct device *dev,
			  struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct emd_power_priv *priv = dev_get_drvdata(dev);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret)
		return ret;
	if (val > 0xFF)
		return -EINVAL;

	ret = emd_ec_ecwr_write(priv, (u8)val);
	return ret ?: count;
}

static DEVICE_ATTR_WO(ecwr);

static int emd_power_probe(struct wmi_device *wdev, const void *context)
{
	struct emd_power_priv *priv;
	struct device *ppdev;
	int probe_val, ret;

	priv = devm_kzalloc(&wdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->wdev = wdev;
	dev_set_drvdata(&wdev->dev, priv);

	ret = emd_power_get(priv, &probe_val);
	if (ret)
		return ret;

	ppdev = devm_platform_profile_register(&wdev->dev, "epm",
					       priv, &emd_power_profile_ops);
	if (IS_ERR(ppdev))
		return PTR_ERR(ppdev);

	/* Add ECWR control attribute */
	ret = device_create_file(&wdev->dev, &dev_attr_ecwr);
	if (ret)
		return ret;

	/* Add fan control attributes */
	ret = device_create_file(&wdev->dev, &dev_attr_fan_speed);
	if (ret)
		dev_warn(&wdev->dev, "failed to create fan_speed attr: %d\n", ret);
	ret = device_create_file(&wdev->dev, &dev_attr_fan_mode);
	if (ret)
		dev_warn(&wdev->dev, "failed to create fan_mode attr: %d\n", ret);

	dev_info(&wdev->dev,
		 "EmdAcpi power mode bound (current=%d, %s)\n",
		 probe_val,
		 priv->quirk_broken_wmbf ? "EC IO fallback" : "WMI path");
	return 0;
}

static void emd_power_remove(struct wmi_device *wdev)
{
	device_remove_file(&wdev->dev, &dev_attr_ecwr);
	device_remove_file(&wdev->dev, &dev_attr_fan_speed);
	device_remove_file(&wdev->dev, &dev_attr_fan_mode);
}

static const struct wmi_device_id emd_power_id_table[] = {
	{ EMD_GUID_BF_POWER_MODE, 0 },
	{ }
};

static struct wmi_driver emd_power_driver = {
	.driver = {
		.name = "emdoor-power",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.id_table = emd_power_id_table,
	.probe = emd_power_probe,
	.remove = emd_power_remove,
	.no_singleton = true,
};

/* ------------------------------------------------------------------------- */
/* Battery Charge Ratio (WMDD -> EmdAcpi_BatteryChargeRatio)                */
/* ------------------------------------------------------------------------- */

#define EMD_WMDD_CASE_GET		0x01
#define EMD_WMDD_CASE_SET		0x02

/*
 * Standard 8-byte return layout used by EmdAcpi WMDD (BatteryChargeRatio)
 */
struct emd_charge_return {
	u16 rt_code;
	u8  micp;
	u8  mxcp;
	u8  reserved[3];
} __packed;

struct emd_charge_priv {
	struct wmi_device *wdev;
	struct mutex lock;
	bool quirk_broken_wmdd;
};

/*
 * EC register access helpers for charge ratio
 */
static int emd_ec_micp_read(struct emd_charge_priv *priv, u8 *val)
{
	int ret;

	ret = ec_read(EMD_EC_REG_MICP, val);
	if (ret)
		dev_err(&priv->wdev->dev,
			"EC read of MICP (0xBB) failed: %d\n", ret);
	return ret;
}

static int emd_ec_mxcp_read(struct emd_charge_priv *priv, u8 *val)
{
	int ret;

	ret = ec_read(EMD_EC_REG_MXCP, val);
	if (ret)
		dev_err(&priv->wdev->dev,
			"EC read of MXCP (0xBC) failed: %d\n", ret);
	return ret;
}

static int emd_ec_micp_write(struct emd_charge_priv *priv, u8 val)
{
	int ret;

	ret = ec_write(EMD_EC_REG_MICP, val);
	if (ret)
		dev_err(&priv->wdev->dev,
			"EC write of MICP (0xBB) failed: %d\n", ret);
	return ret;
}

static int emd_ec_mxcp_write(struct emd_charge_priv *priv, u8 val)
{
	int ret;

	ret = ec_write(EMD_EC_REG_MXCP, val);
	if (ret)
		dev_err(&priv->wdev->dev,
			"EC write of MXCP (0xBC) failed: %d\n", ret);
	return ret;
}

/*
 * Read charge ratio: try WMI first, fall back to EC on failure.
 * Per DSDT: WMDD case 1 returns MICP and MXCP.
 */
static int emd_charge_get(struct emd_charge_priv *priv, u8 *micp, u8 *mxcp)
{
	struct emd_charge_return r;
	int ret;

	if (priv->quirk_broken_wmdd)
		goto read_ec;

	ret = emd_wmi_call(priv->wdev, EMD_WMDD_CASE_GET, NULL, 0, &r, sizeof(r));
	if (ret == 0 && r.rt_code == EMD_RT_OK) {
		*micp = r.micp;
		*mxcp = r.mxcp;
		return 0;
	}

	priv->quirk_broken_wmdd = true;
read_ec:
	ret = emd_ec_micp_read(priv, micp);
	if (ret)
		return ret;
	ret = emd_ec_mxcp_read(priv, mxcp);
	if (ret)
		return ret;
	return 0;
}

/*
 * Write charge ratio: try WMI first, fall back to EC on failure.
 * Per DSDT: WMDD case 2 writes Arg2 to MXCP and Arg2-1 to MICP.
 * Our charge_ratio attribute takes one value (0-100) and applies:
 *   MXCP = val, MICP = val - 1
 */
static int emd_charge_set(struct emd_charge_priv *priv, u8 val)
{
	struct emd_return r;
	u8 micp, mxcp;
	int ret;

	if (val > 100)
		return -EINVAL;

	mxcp = val;
	micp = (val > 0) ? val - 1 : 0;

	if (priv->quirk_broken_wmdd)
		goto write_ec;

	/* WMDD case 2 takes Arg2 (MXCP); MICP = Arg2-1 per DSDT */
	ret = emd_wmi_call(priv->wdev, EMD_WMDD_CASE_SET, &mxcp, 1, &r, sizeof(r));
	if (ret == 0 && r.rt_code == EMD_RT_OK)
		return 0;

	priv->quirk_broken_wmdd = true;
write_ec:
	ret = emd_ec_mxcp_write(priv, mxcp);
	if (ret)
		return ret;
	ret = emd_ec_micp_write(priv, micp);
	return ret;
}

/*
 * sysfs attributes for charge ratio on the WMI device:
 *   charge_micp  - read current MICP (min charge %)
 *   charge_mxcp  - read current MXCP (max charge %)
 *   charge_ratio - write new limit (sets MXCP=val, MICP=val-1)
 */
static ssize_t charge_micp_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct emd_charge_priv *priv = dev_get_drvdata(dev);
	u8 micp, mxcp;
	int ret;

	ret = mutex_lock_interruptible(&priv->lock);
	if (ret)
		return ret;
	ret = emd_charge_get(priv, &micp, &mxcp);
	mutex_unlock(&priv->lock);

	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", micp);
}
static DEVICE_ATTR_RO(charge_micp);

static ssize_t charge_mxcp_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct emd_charge_priv *priv = dev_get_drvdata(dev);
	u8 micp, mxcp;
	int ret;

	ret = mutex_lock_interruptible(&priv->lock);
	if (ret)
		return ret;
	ret = emd_charge_get(priv, &micp, &mxcp);
	mutex_unlock(&priv->lock);

	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", mxcp);
}
static DEVICE_ATTR_RO(charge_mxcp);

static ssize_t charge_ratio_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct emd_charge_priv *priv = dev_get_drvdata(dev);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret)
		return ret;
	if (val > 100)
		return -EINVAL;

	ret = mutex_lock_interruptible(&priv->lock);
	if (ret)
		return ret;
	ret = emd_charge_set(priv, (u8)val);
	mutex_unlock(&priv->lock);

	return ret ?: count;
}
static DEVICE_ATTR_WO(charge_ratio);

static struct attribute *emd_charge_attrs[] = {
	&dev_attr_charge_micp.attr,
	&dev_attr_charge_mxcp.attr,
	&dev_attr_charge_ratio.attr,
	NULL,
};

static const struct attribute_group emd_charge_attr_group = {
	.attrs = emd_charge_attrs,
};

static int emd_charge_probe(struct wmi_device *wdev, const void *context)
{
	struct emd_charge_priv *priv;
	u8 micp, mxcp;
	int ret;

	priv = devm_kzalloc(&wdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->wdev = wdev;
	mutex_init(&priv->lock);
	dev_set_drvdata(&wdev->dev, priv);

	ret = emd_charge_get(priv, &micp, &mxcp);
	if (ret)
		return ret;

	ret = devm_device_add_group(&wdev->dev, &emd_charge_attr_group);
	if (ret)
		return ret;

	dev_info(&wdev->dev,
		 "EmdAcpi battery charge ratio bound (MICP=%u, MXCP=%u, %s)\n",
		 micp, mxcp,
		 priv->quirk_broken_wmdd ? "EC IO fallback" : "WMI path");
	return 0;
}

static void emd_charge_remove(struct wmi_device *wdev)
{
	/* devm_device_add_group auto-removes on detach */
}

static const struct wmi_device_id emd_charge_id_table[] = {
	{ EMD_GUID_BATTERY_CHARGE, 0 },
	{ }
};

static struct wmi_driver emd_charge_driver = {
	.driver = {
		.name = "emdoor-charge",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.id_table = emd_charge_id_table,
	.probe = emd_charge_probe,
	.remove = emd_charge_remove,
	.no_singleton = true,
};

/* ------------------------------------------------------------------------- */
/* Power Limit Override (CMS mailbox direct access)                         */
/* ------------------------------------------------------------------------- */

/*
 * Power Limit Override (CMS mailbox direct access)
 *
 * The EC exposes a Command/Data mailbox at I/O ports 0x72/0x73.
 * Command 0x0C (ALIB) writes a 32-bit value to a specific register:
 *   outb(0x0C, 0x72) -> outb(reg, 0x73) -> outl(value, 0x73)
 * Register map:
 *   0x05 = PL1 (sustained power limit)
 *   0x06 = PL2 (short turbo limit)
 *   0x07 = PL3 (peak power limit)
 *   0x2E = PL1 duplicate (some firmware mirrors PL1 here)
 *   0x32 = SYS_PL (system total power limit)
 *   0x13 = GPU_PL (GPU power limit)
 * All values in milliwatts. Write-only sysfs interface.
 */
#define EMD_CMS_CMD_PORT		0x72
#define EMD_CMS_DATA_PORT		0x73
#define EMD_CMS_ALIB_CMD		0x0C

#define EMD_PL_REG_PL1		0x05
#define EMD_PL_REG_PL2		0x06
#define EMD_PL_REG_PL3		0x07
#define EMD_PL_REG_PL1_DUP	0x2E
#define EMD_PL_REG_SYS		0x32
#define EMD_PL_REG_GPU		0x13

struct emd_pl_priv {
	struct mutex lock;
};

/*
 * EC CMS mailbox write helper.
 * Sequence: cmd -> delay -> reg -> delay -> value -> delay
 * Delays are critical - the EC needs time to process each byte.
 */
static int emd_cms_write(u8 cmd, u8 reg, u32 value)
{
	outb(cmd, EMD_CMS_CMD_PORT);
	udelay(1);
	outb(reg, EMD_CMS_DATA_PORT);
	udelay(1);
	outl(value, EMD_CMS_DATA_PORT);
	udelay(10);
	return 0;
}

static ssize_t pl_limit_show(struct kobject *kobj,
			     struct kobj_attribute *attr, char *buf)
{
	/*
	 * Power limit sysfs entries are write-only. Reads return this
	 * static string indicating units (milliwatts).
	 */
	return sysfs_emit(buf, "write-only (mW)\n");
}

#define EMD_PL_ATTR(name, reg)						\
	static ssize_t emd_pl_##name##_store(struct kobject *kobj,	\
					     struct kobj_attribute *attr,	\
					     const char *buf, size_t count)	\
	{								\
		struct emd_pl_priv *priv = dev_get_drvdata(kobj_to_dev(kobj));	\
		unsigned long val;					\
		int ret;						\
									\
		if (!priv)						\
			return -ENODEV;					\
		ret = kstrtoul(buf, 0, &val);				\
		if (ret)						\
			return ret;					\
		if (val > 0xFFFFFFFF)					\
			return -EINVAL;					\
		ret = mutex_lock_interruptible(&priv->lock);		\
		if (ret)						\
			return ret;					\
		ret = emd_cms_write(EMD_CMS_ALIB_CMD, reg, (u32)val);	\
		mutex_unlock(&priv->lock);				\
		return ret ?: count;					\
	}								\
	static struct kobj_attribute emd_pl_##name =			\
		__ATTR(name, 0644, pl_limit_show, emd_pl_##name##_store)

EMD_PL_ATTR(pl1, EMD_PL_REG_PL1);
EMD_PL_ATTR(pl2, EMD_PL_REG_PL2);
EMD_PL_ATTR(pl3, EMD_PL_REG_PL3);
EMD_PL_ATTR(pl1_dup, EMD_PL_REG_PL1_DUP);
EMD_PL_ATTR(sys_pl, EMD_PL_REG_SYS);
EMD_PL_ATTR(gpu_pl, EMD_PL_REG_GPU);

static struct attribute *emd_pl_attrs[] = {
	&emd_pl_pl1.attr,
	&emd_pl_pl2.attr,
	&emd_pl_pl3.attr,
	&emd_pl_pl1_dup.attr,
	&emd_pl_sys_pl.attr,
	&emd_pl_gpu_pl.attr,
	NULL,
};

static const struct attribute_group emd_pl_attr_group = {
	.attrs = emd_pl_attrs,
	.name = "power_limits",
};

static int emd_pl_probe(struct platform_device *pdev)
{
	struct emd_pl_priv *priv;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	mutex_init(&priv->lock);
	platform_set_drvdata(pdev, priv);

	ret = sysfs_create_group(&pdev->dev.kobj, &emd_pl_attr_group);
	if (ret)
		return ret;

	dev_info(&pdev->dev, "EmdAcpi power limit override interface ready\n");
	return 0;
}

static void emd_pl_remove(struct platform_device *pdev)
{
	sysfs_remove_group(&pdev->dev.kobj, &emd_pl_attr_group);
}

static struct platform_driver emd_pl_driver = {
	.driver = {
		.name = "emdoor-power-limits",
	},
	.probe = emd_pl_probe,
	.remove = emd_pl_remove,
};

/* ------------------------------------------------------------------------- */
/* Keyboard RGB (DA -> WMDA, used directly)                                  */
/* ------------------------------------------------------------------------- */

/*
 * Keyboard RGB (DA -> WMDA, used directly)
 *
 * The DA_APP_KB_LED device (GUID 8600ACCE-FB9B-443E-86F4-3C867398AAE5)
 * implements the BST2 method via WMDA case 3. This is the only WMI
 * method that parses correctly on Linux ACPICA.
 *
 * The keyboard has 6 logical zones:
 *   Zone 0-3: Keyboard matrix zones (selected via FA00 FACS bits 0-3)
 *   Zone 4: Left side bar (selected via FA00 FLAB bit 4 = 0x10)
 *   Zone 5: Right side bar (selected via FA00 FLAB bit 5 = 0x20)
 *
 * Each zone is exposed as a multicolor LED class device with RGB
 * sub-leds. The led-class-multicolor core handles color blending
 * and calls our brightness_set_blocking with the computed brightness.
 */
#define EMD_WMDA_CASE_SET		0x03
#define EMD_WMDA_CASE_GET_STATUS	0x02
#define EMD_WMDA_DTID_KBD_TYPE		0x02

#define EMD_WMDA_FA06_ALWAYS		0x00
#define EMD_WMDA_FA06_TWINKLE		0x01
#define EMD_WMDA_FA06_WAVE		0x02
#define EMD_WMDA_FA06_BREATH		0x03
#define EMD_WMDA_FA06_COLORCYCLE	0x04
#define EMD_WMDA_FA06_REACTIVE		0x05
#define EMD_WMDA_FA06_RIPPLE		0x06
#define EMD_WMDA_FA06_SPIRAL		0x07
#define EMD_WMDA_FA06_RAINBOW		0x08
#define EMD_WMDA_FA06_RESET		0xFF
#define EMD_WMDA_FA06_OFF		0xFE
#define EMD_FA06_MODE_SKIP		0xFE
#define EMD_KBTE_FOUR_ZONE		0x02

#define EMD_NUM_ZONES			6
#define EMD_CONTROL_SKIP		0xFE

#define EMD_FA00_FACS_ZONE1		0x01
#define EMD_FA00_FACS_ZONE2		0x02
#define EMD_FA00_FACS_ZONE3		0x04
#define EMD_FA00_FACS_ZONE4		0x08
#define EMD_FA00_FLAB_BAR0		0x10
#define EMD_FA00_FLAB_BAR1		0x20

struct emd_kbd_priv;
struct emd_kbd_led {
	struct led_classdev_mc mc;
	struct mc_subled subleds[3];
	struct emd_kbd_priv *priv;
	unsigned int zone;
};

struct emd_kbd_priv {
	struct wmi_device *wdev;
	struct mutex lock;	/* guards mode, multi_intensity writes */
	struct emd_kbd_led leds[EMD_NUM_ZONES];
	struct led_classdev mode_cdev;	/* control-surface LED class device */
	bool removing;		/* driver is being unloaded */
	u8 kbte;
	u8 kbmax;
	u8 mode;		/* last mode written to firmware */
};

/*
 * Apply dynamic effect mode.
 * Only zone 0's colors are sent with FA00=0 and the effect byte.
 * The EC firmware animates all zones using the same pattern.
 * Zone 1-5 color components are ignored in dynamic modes.
 */
static int emd_kbd_apply_dynamic(struct emd_kbd_priv *priv);
/*
 * Apply static colors to all 6 zones sequentially.
 * Zone 0 first, then zones 1-5. Returns first error.
 */
static int emd_kbd_apply_static(struct emd_kbd_priv *priv);
/*
 * LED multicolor brightness_set_blocking callback.
 * The led-class-multicolor core calls this when any subled intensity
 * changes. It computes color components from the cached brightness
 * (must be max_brightness=1 so components are non-zero), then sends
 * the zone's color to the EC via BST2.
 */
static int emd_kbd_mc_brightness_set(struct led_classdev *led_cdev,
				     enum led_brightness brightness);

/*
 * FA06 mode values map to EC EDTA (effect data) values:
 *   0x00 -> EDTA=2 (static)
 *   0x01 -> EDTA=5 (twinkle)
 *   0x02 -> EDTA=4 (wave)
 *   0x03 -> EDTA=3 (breath)
 *   0x04 -> EDTA=6 (colorcycle)
 *   0x05 -> EDTA=7 (reactive)
 *   0x06 -> EDTA=8 (ripple)
 *   0x07 -> EDTA=9 (spiral)
 *   0x08 -> EDTA=10 (rainbow)
 *   0xFE -> OFF
 *   0xFF -> RESET
 */
static const char *const emd_kbd_mode_names[] = {
	[EMD_WMDA_FA06_ALWAYS]		= "always",
	[EMD_WMDA_FA06_TWINKLE]		= "twinkle",
	[EMD_WMDA_FA06_WAVE]		= "wave",
	[EMD_WMDA_FA06_BREATH]		= "breath",
	[EMD_WMDA_FA06_COLORCYCLE]	= "colorcycle",
	[EMD_WMDA_FA06_REACTIVE]	= "reactive",
	[EMD_WMDA_FA06_RIPPLE]		= "ripple",
	[EMD_WMDA_FA06_SPIRAL]		= "spiralrainbow",
	[EMD_WMDA_FA06_RAINBOW]		= "rainbowripple",
};

static const char *const emd_kbte_str[] = {
	[EMD_KBTE_FOUR_ZONE]	= "4zone",
};

static const char *const emd_kbd_off_str = "off";
static const char *const emd_kbd_reset_str = "reset";

/*
 * Zone selector bitmasks for FA00:
 *   Bits 0-3 (0x01,0x02,0x04,0x08) = FACS keyboard zones 1-4
 *   Bit 4 (0x10) = FLAB left side bar
 *   Bit 5 (0x20) = FLAB right side bar
 *   0x0F = all 4 keyboard zones simultaneously (16-byte payload)
 */
static const u8 emd_kbd_zone_sel[EMD_NUM_ZONES] = {
	EMD_FA00_FACS_ZONE1, EMD_FA00_FACS_ZONE2,
	EMD_FA00_FACS_ZONE3, EMD_FA00_FACS_ZONE4,
	EMD_FA00_FLAB_BAR0, EMD_FA00_FLAB_BAR1,
};

static const char *const emd_kbd_led_names[EMD_NUM_ZONES] = {
	"emdoor:multicolor:zone1",
	"emdoor:multicolor:zone2",
	"emdoor:multicolor:zone3",
	"emdoor:multicolor:zone4",
	"emdoor:multicolor:bar-left",
	"emdoor:multicolor:bar-right",
};

struct emd_bst_input {
	u8 fa00;
	u8 r;
	u8 g;
	u8 b;
	u8 control[2];
	u8 effect;
	u8 on;
} __packed;

static int emd_kbd_mode_lookup(const char *buf, u8 *out)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(emd_kbd_mode_names); i++) {
		if (sysfs_streq(buf, emd_kbd_mode_names[i])) {
			*out = (u8)i;
			return 0;
		}
	}
	if (sysfs_streq(buf, emd_kbd_off_str)) {
		*out = EMD_WMDA_FA06_OFF;
		return 0;
	}
	if (sysfs_streq(buf, emd_kbd_reset_str)) {
		*out = EMD_WMDA_FA06_RESET;
		return 0;
	}
	return -EINVAL;
}

/*
 * Send a BST2 input structure to the EC via WMDA case 3.
 * Returns 0 on success, negative errno on failure.
 */
static int emd_kbd_send(struct emd_kbd_priv *priv,
			const struct emd_bst_input *in)
{
	struct emd_return r;
	int ret;

	ret = emd_wmi_call(priv->wdev, EMD_WMDA_CASE_SET,
			   in, sizeof(*in), &r, sizeof(r));
	if (ret)
		return ret;
	if (r.rt_code != EMD_RT_OK)
		return -EIO;
	return 0;
}

/*
 * Apply current keyboard mode to hardware.
 * Routes to static or dynamic path based on priv->mode.
 * For OFF/RESET modes, sends a minimal packet to trigger the EC.
 */
static int emd_kbd_apply(struct emd_kbd_priv *priv)
{
	struct emd_bst_input in;

	if (priv->mode == EMD_WMDA_FA06_OFF) {
		memset(&in, 0, sizeof(in));
		return emd_kbd_send(priv, &in);
	}

	if (priv->mode == EMD_WMDA_FA06_RESET) {
		memset(&in, 0, sizeof(in));
		in.effect = 0xFF;
		in.on = 0xFF;
		return emd_kbd_send(priv, &in);
	}

	if (priv->mode == EMD_WMDA_FA06_ALWAYS)
		return emd_kbd_apply_static(priv);

	return emd_kbd_apply_dynamic(priv);
}

/*
 * Apply static color to a single zone.
 * Used for mode=always (0x00). Sends per-zone FA00 with RGB and
 * effect=0xFE (skip dynamic processing).
 * The LED multicolor core calls this via brightness_set_blocking
 * after computing color components from the zone's brightness.
 */
static int emd_kbd_apply_zone(struct emd_kbd_led *led)
{
	struct emd_kbd_priv *priv = led->priv;
	struct led_classdev_mc *mc = &led->mc;
	struct emd_bst_input in;

	led_mc_calc_color_components(mc, mc->led_cdev.brightness);

	memset(&in, 0, sizeof(in));
	in.control[0] = EMD_CONTROL_SKIP;
	in.control[1] = EMD_CONTROL_SKIP;
	in.on = 0xFF;
	in.r = mc->subled_info[0].brightness;
	in.g = mc->subled_info[1].brightness;
	in.b = mc->subled_info[2].brightness;

	if (priv->mode == EMD_WMDA_FA06_OFF ||
	    priv->mode == EMD_WMDA_FA06_RESET)
		return 0;

	if (priv->mode == EMD_WMDA_FA06_ALWAYS) {
		in.effect = EMD_FA06_MODE_SKIP;
		in.fa00 = emd_kbd_zone_sel[led->zone];
		return emd_kbd_send(priv, &in);
	}

	in.effect = priv->mode;
	in.fa00 = 0;
	return emd_kbd_send(priv, &in);
}

/*
 * Apply static colors to all 6 zones sequentially.
 * Zone 0 first, then zones 1-5. Returns first error.
 */
static int emd_kbd_apply_static(struct emd_kbd_priv *priv)
{
	struct emd_kbd_led *led = &priv->leds[0];
	unsigned int i;
	int ret;

	ret = emd_kbd_apply_zone(led);
	if (ret)
		return ret;
	for (i = 1; i < EMD_NUM_ZONES; i++) {
		ret = emd_kbd_apply_zone(&priv->leds[i]);
		if (ret)
			return ret;
	}
	return 0;
}

/*
 * Apply dynamic effect mode.
 * Only zone 0's colors are sent with FA00=0 and the effect byte.
 * The EC firmware animates all zones using the same pattern.
 * Zone 1-5 color components are ignored in dynamic modes.
 */
static int emd_kbd_apply_dynamic(struct emd_kbd_priv *priv)
{
	return emd_kbd_apply_zone(&priv->leds[0]);
}

/*
 * LED multicolor brightness_set_blocking callback.
 * The led-class-multicolor core calls this when any subled intensity
 * changes. It computes color components from the cached brightness
 * (must be max_brightness=1 so components are non-zero), then sends
 * the zone's color to the EC via BST2.
 */
static int emd_kbd_mc_brightness_set(struct led_classdev *led_cdev,
				     enum led_brightness brightness)
{
	struct emd_kbd_led *led =
		container_of(lcdev_to_mccdev(led_cdev),
			     struct emd_kbd_led, mc);
	struct emd_kbd_priv *priv = led->priv;
	int ret;

	if (priv->removing)
		return 0;

	ret = mutex_lock_interruptible(&priv->lock);
	if (ret)
		return ret;
	ret = emd_kbd_apply_zone(led);
	mutex_unlock(&priv->lock);
	return ret;
}

/*
 * Query keyboard type via WMDA case 2 (DTID=2).
 * Response: RTS0=1 (success), byte 3 = KBTE (keyboard type enum),
 * byte 5 = KBMAX (max brightness). We only support KBTE=2 (4-zone).
 */
static int emd_kbd_probe_type(struct emd_kbd_priv *priv)
{
	u8 args[8] = { EMD_WMDA_DTID_KBD_TYPE };
	u8 resp[16] = { };
	int ret;

	ret = emd_wmi_call(priv->wdev, EMD_WMDA_CASE_GET_STATUS,
			   args, sizeof(args), resp, sizeof(resp));
	if (ret)
		return ret;

	if (resp[2] != 1) {
		dev_err(&priv->wdev->dev,
			"WMDA DTID=2 returned RTS0=%u (expected 1)\n", resp[2]);
		return -EIO;
	}
	priv->kbte = resp[3];
	priv->kbmax = resp[5];
	return 0;
}

static ssize_t type_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct emd_kbd_priv *priv = dev_get_drvdata(dev);

	if (priv->kbte >= ARRAY_SIZE(emd_kbte_str) || !emd_kbte_str[priv->kbte])
		return sysfs_emit(buf, "unknown\n");
	return sysfs_emit(buf, "%s\n", emd_kbte_str[priv->kbte]);
}
static DEVICE_ATTR_RO(type);

/*
 * No-op brightness handler for the mode control LED class device.
 * `emdoor:rgb:mode` is a control surface, not a real LED - its
 * brightness is meaningless. The LED class framework rejects any
 * led_classdev that doesn't provide at least one of brightness_set
 * or brightness_set_blocking, so we stub one out.
 */
static int emd_kbd_mode_cdev_set_brightness(struct led_classdev *led_cdev,
					    enum led_brightness brightness)
{
	return 0;
}

static ssize_t mode_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	/*
	 * Mode/modes live on the LED class device `emdoor:rgb:mode`,
	 * whose parent is the WMI device. The WMI device's drvdata is
	 * our emd_kbd_priv; reach it via dev->parent->driver_data.
	 */
	struct emd_kbd_priv *priv = dev_get_drvdata(dev->parent);
	const char *mode = "unknown";

	if (!priv)
		return sysfs_emit(buf, "unknown\n");

	mutex_lock(&priv->lock);
	if (priv->mode == EMD_WMDA_FA06_OFF)
		mode = emd_kbd_off_str;
	else if (priv->mode == EMD_WMDA_FA06_RESET)
		mode = emd_kbd_reset_str;
	else if (priv->mode < ARRAY_SIZE(emd_kbd_mode_names) &&
		 emd_kbd_mode_names[priv->mode])
		mode = emd_kbd_mode_names[priv->mode];
	mutex_unlock(&priv->lock);

	return sysfs_emit(buf, "%s\n", mode);
}

static ssize_t mode_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct emd_kbd_priv *priv = dev_get_drvdata(dev->parent);
	u8 mode, old_mode;
	int ret;

	if (!priv)
		return -ENODEV;

	ret = emd_kbd_mode_lookup(buf, &mode);
	if (ret)
		return ret;

	ret = mutex_lock_interruptible(&priv->lock);
	if (ret)
		return ret;
	old_mode = priv->mode;
	priv->mode = mode;
	ret = emd_kbd_apply(priv);
	if (ret)
		priv->mode = old_mode;
	mutex_unlock(&priv->lock);

	return ret ?: count;
}
static DEVICE_ATTR_RW(mode);

static ssize_t modes_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	ssize_t len = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(emd_kbd_mode_names); i++) {
		if (!emd_kbd_mode_names[i])
			continue;
		len += sysfs_emit_at(buf, len, "%s%s",
				     len ? " " : "",
				     emd_kbd_mode_names[i]);
	}
	len += sysfs_emit_at(buf, len, " %s %s",
			     emd_kbd_off_str, emd_kbd_reset_str);
	len += sysfs_emit_at(buf, len, "\n");
	return len;
}
static DEVICE_ATTR_RO(modes);

/*
 * Mode/modes attributes live on a dedicated `emdoor:rgb:mode` LED class
 * device, not on the WMI device. Exposing them under /sys/class/leds/
 * lets the SteamOS udev rule (70-steam-jupiter-leds.rules) auto-chown
 * any `mode` file to deck:deck, so Decky Loader (running as the deck
 * user) can switch animation modes without root.
 *
 * The LED class's standard groups (brightness, max_brightness) come
 * from the class's dev_groups; we don't override `.groups` here. The
 * mode/modes attributes are attached via device_create_file in probe
 * after the LED class device is registered - they get cleaned up
 * automatically when the device is unregistered.
 */

/*
 * WMI device attributes. `type` lives here; mode/modes moved to LED
 * class device for udev chown. Fan control is separate (power driver).
 */
static struct attribute *emd_kbd_attrs[] = {
	&dev_attr_type.attr,
	NULL,
};

static const struct attribute_group emd_kbd_attr_group = {
	.attrs = emd_kbd_attrs,
};

/*
 * Initialize a single multicolor LED for a zone.
 * Sets up subleds for R/G/B, registers with led-class-multicolor.
 * Initial brightness=1 (max_brightness) so color components flow
 * through correctly - see LED multicolor note in LTM.
 */
static int emd_kbd_init_led(struct emd_kbd_priv *priv, unsigned int zone)
{
	struct emd_kbd_led *led = &priv->leds[zone];
	struct led_classdev_mc *mc = &led->mc;

	led->priv = priv;
	led->zone = zone;

	mc->subled_info = led->subleds;
	mc->led_cdev.name = emd_kbd_led_names[zone];
	mc->led_cdev.brightness_set_blocking = emd_kbd_mc_brightness_set;
	mc->led_cdev.max_brightness = 1;
	mc->led_cdev.color = LED_COLOR_ID_RGB;
	mc->led_cdev.brightness = 1;
	mc->num_colors = 3;
	led->subleds[0] = (struct mc_subled){
		.color_index = LED_COLOR_ID_RED,
		.intensity = 0,
	};
	led->subleds[1] = (struct mc_subled){
		.color_index = LED_COLOR_ID_GREEN,
		.intensity = 0,
	};
	led->subleds[2] = (struct mc_subled){
		.color_index = LED_COLOR_ID_BLUE,
		.intensity = 0,
	};
	return devm_led_classdev_multicolor_register(&priv->wdev->dev, mc);
}

/*
 * Remove callback - sets the removing flag. All LED class devices are
 * devm-managed and will be automatically unregistered after the remove
 * function returns, with the brightness_set_blocking callback honoring
 * the removing flag to avoid further hardware access.
 */
static void emd_kbd_remove(struct wmi_device *wdev)
{
	struct emd_kbd_priv *priv = dev_get_drvdata(&wdev->dev);

	if (!priv)
		return;

	priv->removing = true;
	/* No manual unregister – devres handles everything */
}

static int emd_kbd_probe(struct wmi_device *wdev, const void *context)
{
	struct emd_kbd_priv *priv;
	unsigned int i;
	int ret;

	priv = devm_kzalloc(&wdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->wdev = wdev;
	priv->mode = EMD_WMDA_FA06_ALWAYS;
	mutex_init(&priv->lock);
	dev_set_drvdata(&wdev->dev, priv);

	ret = emd_kbd_probe_type(priv);
	if (ret)
		return ret;
	if (priv->kbte != EMD_KBTE_FOUR_ZONE) {
		dev_err(&wdev->dev, "unsupported keyboard type 0x%02x\n",
			priv->kbte);
		return -ENODEV;
	}

	for (i = 0; i < EMD_NUM_ZONES; i++) {
		ret = emd_kbd_init_led(priv, i);
		if (ret)
			return ret;
	}

	/*
	 * Register the mode control LED class device. This is a virtual
	 * LED class device - no actual LEDs - that carries the mode and
	 * modes attributes. SteamOS's 70-steam-jupiter-leds.rules chowns
	 * its `mode` file to deck:deck, which is what the Decky plugin
	 * needs to switch animation modes without root.
	 *
	 * `.groups` is left NULL so the class's default groups
	 * (brightness, max_brightness) get attached automatically.
	 * The mode/modes attributes are then added via
	 * device_create_file below - cleanup is automatic when the
	 * LED class device is unregistered.
	 */
	priv->mode_cdev.name = "emdoor:rgb:mode";
	priv->mode_cdev.max_brightness = 1;
	priv->mode_cdev.brightness = 0;
	priv->mode_cdev.brightness_set_blocking =
		emd_kbd_mode_cdev_set_brightness;
	ret = devm_led_classdev_register(&wdev->dev, &priv->mode_cdev);
	if (ret)
		return ret;

	ret = device_create_file(priv->mode_cdev.dev, &dev_attr_mode);
	if (ret)
		return ret;

	ret = device_create_file(priv->mode_cdev.dev, &dev_attr_modes);
	if (ret)
		return ret;

	ret = devm_device_add_group(&wdev->dev, &emd_kbd_attr_group);
	if (ret)
		return ret;

	dev_info(&wdev->dev,
		 "EmdAcpi keyboard backlight bound (type=%s max=%u mode=%s)\n",
		 (priv->kbte < ARRAY_SIZE(emd_kbte_str) &&
		  emd_kbte_str[priv->kbte]) ?
			 emd_kbte_str[priv->kbte] : "unknown",
		 priv->kbmax,
		 emd_kbd_mode_names[priv->mode] ?: emd_kbd_off_str);
	return 0;
}

static const struct wmi_device_id emd_kbd_id_table[] = {
	{ EMD_GUID_DA_APP_KB_LED, 0 },
	{ }
};

static struct wmi_driver emd_kbd_driver = {
	.driver = {
		.name = "emdoor-kbd",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.id_table = emd_kbd_id_table,
	.probe = emd_kbd_probe,
	.remove = emd_kbd_remove,
	.no_singleton = true,
};

/* ------------------------------------------------------------------------- */
/* Module entry                                                              */
/* ------------------------------------------------------------------------- */

static struct platform_device *emd_pl_pdev;

/*
 * Module initialization - registers all 4 drivers in order:
 *   1. emdoor-power      - platform_profile via WMBF (case 1-4) with EC
 *                          PWMD (0x7C) fallback; ECWR attribute for E4 trigger
 *   2. emdoor-charge     - battery charge ratio via WMDD (case 1/2) with EC
 *                          MICP (0xBB) / MXCP (0xBC) fallback; charge_ratio
 *                          attribute sets MXCP=val, MICP=val-1
 *   3. emdoor-kbd        - 6-zone RGB keyboard via WMDA case 3 (BST2);
 *                          multicolor LED class devices + mode control surface
 *   4. emdoor-power-limits - CMS mailbox (ports 0x72/0x73) direct EC writes
 *                            for PL1-PL3, SYS_PL, GPU_PL (all in mW)
 * All-or-nothing: failure rolls back previous registrations.
 */
static int __init emd_wmi_init(void)
{
	int ret;

	ret = emd_dmi_check();
	if (ret)
		return ret;

	ret = wmi_driver_register(&emd_power_driver);
	if (ret)
		return ret;

	ret = wmi_driver_register(&emd_charge_driver);
	if (ret) {
		wmi_driver_unregister(&emd_power_driver);
		return ret;
	}

	ret = wmi_driver_register(&emd_kbd_driver);
	if (ret) {
		wmi_driver_unregister(&emd_charge_driver);
		wmi_driver_unregister(&emd_power_driver);
		return ret;
	}

	/* Create platform device for power limit override sysfs */
	emd_pl_pdev = platform_device_register_simple("emdoor-power-limits", -1, NULL, 0);
	if (IS_ERR(emd_pl_pdev)) {
		ret = PTR_ERR(emd_pl_pdev);
		wmi_driver_unregister(&emd_kbd_driver);
		wmi_driver_unregister(&emd_charge_driver);
		wmi_driver_unregister(&emd_power_driver);
		return ret;
	}

	ret = platform_driver_register(&emd_pl_driver);
	if (ret) {
		platform_device_unregister(emd_pl_pdev);
		wmi_driver_unregister(&emd_kbd_driver);
		wmi_driver_unregister(&emd_charge_driver);
		wmi_driver_unregister(&emd_power_driver);
		return ret;
	}

	return 0;
}

/*
 * Module cleanup - reverse order of init.
 */
static void __exit emd_wmi_exit(void)
{
	platform_driver_unregister(&emd_pl_driver);
	platform_device_unregister(emd_pl_pdev);
	wmi_driver_unregister(&emd_kbd_driver);
	wmi_driver_unregister(&emd_charge_driver);
	wmi_driver_unregister(&emd_power_driver);
}

module_init(emd_wmi_init);
module_exit(emd_wmi_exit);

MODULE_AUTHOR("fewtarius");
MODULE_DESCRIPTION("EmdAcpi power, battery charge ratio, power limit override, and six-zone RGB driver (Nimo, Xuanpai, MetaMech)");
MODULE_LICENSE("GPL");

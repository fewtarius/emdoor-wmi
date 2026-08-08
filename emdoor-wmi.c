// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EmdAcpi driver for Nimo, Xuanpai, and MetaMech laptops
 *
 * Three subsystems in one module. Two are EC-backed platform drivers;
 * one is a WMI driver for the keyboard (the only EmdAcpi method that
 * parses cleanly under Linux ACPICA).
 *
 *   - emdoor-power      platform device `emdoor-power`
 *       platform_profile: low-power / balanced / performance (EC PWMD)
 *       hwmon `emdoor_fan`:
 *           fan1_input (RPM from EC FN1H/FN1L)
 *           fan_mode   (custom: 1=AUTO, 2=MAX via EC TLID)
 *           power[1-6]_cap (write-only, microWatts via EC CMS mailbox):
 *             power1=PL1, power2=PL2, power3=PL3,
 *             power4=PL1_DUP, power5=SYS_PL, power6=GPU_PL
 *       On probe, sets fan TLID to EMD_FAN_AUTO so the EC defaults
 *       to AUTO at boot regardless of what the firmware last had.
 *
 *   - emdoor-charge     platform device `emdoor-charge`
 *       power_supply extension on the existing BAT0:
 *           charge_control_start_threshold  (read EC MICP)
 *           charge_control_end_threshold    (read/write EC MXCP)
 *
 *   - emdoor-kbd        WMI driver for 8600ACCE-FB9B-443E-86F4-3C867398AAE5
 *       6 multicolor LED class devices (4 keyboard zones + 2 side bars)
 *       mode control surface on `emdoor:rgb:mode` LED class device
 *
 * Why EC IO only for power and charge?
 *
 * 14 of the 15 EmdAcpi WMxx methods declare a 63-bit BufferField at
 * offset 0x20 of an 8-byte argument buffer, which is structurally
 * out-of-bounds. Linux ACPICA rejects the declaration with
 * AE_AML_BUFFER_LIMIT before any case body runs and emits a kernel
 * log error every probe; the unreferenced field is dead code. We
 * cannot patch the BIOS or relax ACPICA. WMBF (power) and WMDD
 * (charge) are both in this set, so we talk to the EC directly.
 * WMDA uses a 0x50-byte local buffer and parses cleanly, so we keep
 * the WMI driver for the keyboard.
 *
 * Copyright (C) 2026 fewtarius
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <acpi/battery.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/led-class-multicolor.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/platform_profile.h>
#include <linux/power_supply.h>
#include <linux/types.h>
#include <linux/wmi.h>

MODULE_DESCRIPTION("EmdAcpi power, fan, charge, and six-zone RGB driver (Nimo, Xuanpai, MetaMech)");
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

/*
 * Boot-time charge threshold override.
 *   -1 (default): leave whatever the firmware / EC last had alone.
 *    0..100    : on charge-probe, write this value to EC MXCP (with the
 *                firmware-enforced MICP = MXCP - 1). Useful when userspace
 *                (tlp, auto-cpufreq, custom units) can't be relied on to
 *                run early enough, e.g. headless boots.
 */
static int default_charge_end_threshold = -1;
module_param(default_charge_end_threshold, int, 0444);
MODULE_PARM_DESC(default_charge_end_threshold,
		 "Probe-time charge_control_end_threshold (-1 = no override; 0..100 = write MXCP)");

static int emd_dmi_check(void)
{
	if (force_load)
		return 0;
	if (dmi_first_match(emd_dmi_table))
		return 0;
	pr_info("hardware not on DMI whitelist; load with force_load=1 to override\n");
	return -ENODEV;
}

/* ------------------------------------------------------------------------- */
/* EC register map                                                            */
/* ------------------------------------------------------------------------- */

/*
 * All control goes through the embedded controller at I/O ports 0x62/0x66
 * (the standard ACPI EC). The EC's OperationRegion exposes these fields:
 *
 *   PWMD (0x7C)  Power mode (1=quiet, 2=balance, 3=performance)
 *   TLID (0x32)  Fan mode (1=auto, 2=max)
 *   XXTT (0x30)  Fan control arm (write 0x11 before TLID)
 *   FN1H (0x76)  Fan 1 RPM high byte
 *   FN1L (0x77)  Fan 1 RPM low byte
 *   ECWR (0x78)  Power-limit refresh trigger (E004)
 *   MICP (0xBB)  Min charge percent
 *   MXCP (0xBC)  Max charge percent
 */
#define EMD_EC_REG_PWMD			0x7C
#define EMD_EC_REG_TLID			0x32
#define EMD_EC_REG_XXTT			0x30
#define EMD_EC_REG_FN1H			0x76
#define EMD_EC_REG_FN1L			0x77
#define EMD_EC_REG_ECWR			0x78
/*
 * Charge-threshold registers. The DSDT's OperationRegion(ECF2) field
 * declaration (DSDT.dsl around line 8766) names 0xBB as MXCP (max charge
 * percent) and 0xBC as MICP (min charge percent). The firmware's WMDD
 * method mirrors this: it writes Arg2 to MXCP and (Arg2 - 1) to MICP.
 * Match the firmware; do not "fix" the apparent swap.
 */
#define EMD_EC_REG_MXCP			0xBB
#define EMD_EC_REG_MICP			0xBC

/* CMS mailbox for power-limit writes (I/O ports 0x72/0x73). */
#define EMD_CMS_CMD_PORT		0x72
#define EMD_CMS_DATA_PORT		0x73
#define EMD_CMS_ALIB_CMD		0x0C

/*
 * Power limit register map (CMS mailbox ALIB cmd 0x0C):
 *   0x05 = PL1     sustained power limit
 *   0x06 = PL2     short turbo limit
 *   0x07 = PL3     peak power limit
 *   0x2E = PL1_DUP PL1 duplicate (some firmware mirrors PL1 here)
 *   0x32 = SYS_PL  system total power limit
 *   0x13 = GPU_PL  GPU power limit
 *
 * All values in milliwatts on the wire; hwmon exposes them as
 * microWatts to match the kernel's powerN_cap convention.
 */
#define EMD_PL_REG_PL1			0x05
#define EMD_PL_REG_PL2			0x06
#define EMD_PL_REG_PL3			0x07
#define EMD_PL_REG_PL1_DUP		0x2E
#define EMD_PL_REG_SYS_PL		0x32
#define EMD_PL_REG_GPU_PL		0x13

/* Fan mode values (EC TLID register). */
#define EMD_FAN_AUTO			1
#define EMD_FAN_MAX			2

/* Power profile values (EC PWMD register). */
#define EMD_POWER_PROFILE_QUIET		1
#define EMD_POWER_PROFILE_BALANCE	2
#define EMD_POWER_PROFILE_PERF		3

/* ------------------------------------------------------------------------- */
/* EC IO helpers                                                              */
/* ------------------------------------------------------------------------- */

static int emd_ec_read(struct device *dev, u8 reg, u8 *val)
{
	int ret;

	ret = ec_read(reg, val);
	if (ret)
		dev_err(dev, "EC read 0x%02x failed: %d\n", reg, ret);
	return ret;
}

static int emd_ec_write(struct device *dev, u8 reg, u8 val)
{
	int ret;

	ret = ec_write(reg, val);
	if (ret)
		dev_err(dev, "EC write 0x%02x=%02x failed: %d\n", reg, val, ret);
	return ret;
}

static void emd_cms_write(u8 reg, u32 value)
{
	outb(EMD_CMS_ALIB_CMD, EMD_CMS_CMD_PORT);
	udelay(1);
	outb(reg, EMD_CMS_DATA_PORT);
	udelay(1);
	outl(value, EMD_CMS_DATA_PORT);
	udelay(10);
}

/* ------------------------------------------------------------------------- */
/* Fan control (EC IO)                                                        */
/* ------------------------------------------------------------------------- */

static int emd_fan_get_speed(struct device *dev, u16 *rpm)
{
	u8 fn1l, fn1h;
	int ret;

	ret = emd_ec_read(dev, EMD_EC_REG_FN1L, &fn1l);
	if (ret)
		return ret;
	ret = emd_ec_read(dev, EMD_EC_REG_FN1H, &fn1h);
	if (ret)
		return ret;

	*rpm = ((u16)fn1h << 8) | fn1l;
	return 0;
}

static int emd_fan_get_mode(struct device *dev, u8 *mode)
{
	return emd_ec_read(dev, EMD_EC_REG_TLID, mode);
}

static int emd_fan_set_mode(struct device *dev, u8 mode)
{
	u8 xxtt;
	int ret;

	if (mode != EMD_FAN_AUTO && mode != EMD_FAN_MAX)
		return -EINVAL;

	/*
	 * Arm fan control by writing 0x11 to XXTT, verify the EC echoed
	 * it back, then write TLID and verify again. The EC is finicky
	 * about the protocol: a missing arm write silently drops TLID.
	 */
	ret = emd_ec_write(dev, EMD_EC_REG_XXTT, 0x11);
	if (ret)
		return ret;

	ret = emd_ec_read(dev, EMD_EC_REG_XXTT, &xxtt);
	if (ret)
		return ret;
	if (xxtt != 0x11) {
		dev_err(dev, "XXTT arm verification failed: expected 0x11, got 0x%02x\n",
			xxtt);
		return -EIO;
	}

	ret = emd_ec_write(dev, EMD_EC_REG_TLID, mode);
	if (ret)
		return ret;

	ret = emd_ec_read(dev, EMD_EC_REG_TLID, &xxtt);
	if (ret)
		return ret;
	if (xxtt != mode) {
		dev_err(dev, "TLID verification failed: expected %u, got 0x%02x\n",
			mode, xxtt);
		return -EIO;
	}

	return 0;
}

/* ------------------------------------------------------------------------- */
/* Power platform driver                                                      */
/* ------------------------------------------------------------------------- */

struct emd_power_priv {
	struct device *dev;
	struct device *hwmon_dev;
	struct device *profile_dev;
};

/*
 * platform_profile handlers.
 *
 * Reads/writes the EC PWMD register directly. The EC firmware watches
 * this register and re-tunes PL1/PL2/PL3 limits when it changes; we
 * additionally pulse ECWR (0x78) so any manual PL overrides via the
 * CMS mailbox take effect immediately.
 */
static int emd_power_profile_get(struct device *dev,
				 enum platform_profile_option *profile)
{
	u8 val;
	int ret;

	ret = emd_ec_read(dev, EMD_EC_REG_PWMD, &val);
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
		dev_warn(dev, "unknown power-mode value %u\n", val);
		return -EINVAL;
	}
	return 0;
}

static int emd_power_profile_set(struct device *dev,
				 enum platform_profile_option profile)
{
	u8 val;
	int ret;

	switch (profile) {
	case PLATFORM_PROFILE_LOW_POWER:
		val = EMD_POWER_PROFILE_QUIET;
		break;
	case PLATFORM_PROFILE_BALANCED:
		val = EMD_POWER_PROFILE_BALANCE;
		break;
	case PLATFORM_PROFILE_PERFORMANCE:
		val = EMD_POWER_PROFILE_PERF;
		break;
	default:
		return -EOPNOTSUPP;
	}

	ret = emd_ec_write(dev, EMD_EC_REG_PWMD, val);
	if (ret)
		return ret;

	/* Trigger EC E4 interrupt so PL1/PL2/PL3 are re-tuned. */
	return emd_ec_write(dev, EMD_EC_REG_ECWR, 1);
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

/* hwmon callbacks */

static umode_t emd_hwmon_is_visible(const void *drvdata,
				    enum hwmon_sensor_types type,
				    u32 attr, int channel)
{
	switch (type) {
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_input:
			return 0444;
		default:
			return 0;
		}
	case hwmon_power:
		switch (attr) {
		case hwmon_power_cap:
			return 0200;	/* write-only */
		default:
			return 0;
		}
	default:
		return 0;
	}
}

static int emd_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			  u32 attr, int channel, long *val)
{
	if (type == hwmon_fan && attr == hwmon_fan_input) {
		u16 rpm;
		int ret;

		ret = emd_fan_get_speed(dev, &rpm);
		if (ret)
			return ret;
		*val = rpm;
		return 0;
	}
	return -EOPNOTSUPP;
}

static int emd_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
			   u32 attr, int channel, long val)
{
	static const u8 pl_regs[] = {
		[0] = EMD_PL_REG_PL1,
		[1] = EMD_PL_REG_PL2,
		[2] = EMD_PL_REG_PL3,
		[3] = EMD_PL_REG_PL1_DUP,
		[4] = EMD_PL_REG_SYS_PL,
		[5] = EMD_PL_REG_GPU_PL,
	};

	if (type == hwmon_power && attr == hwmon_power_cap) {
		/* hwmon is microWatts; CMS mailbox is milliwatts. */
		if (val < 0 || val > (long)(UINT_MAX / 1000) * 1000)
			return -EINVAL;
		if (channel < 0 || channel >= ARRAY_SIZE(pl_regs))
			return -EINVAL;
		emd_cms_write(pl_regs[channel], (u32)(val / 1000));
		return 0;
	}
	return -EOPNOTSUPP;
}

static const struct hwmon_ops emd_hwmon_ops = {
	.is_visible = emd_hwmon_is_visible,
	.read = emd_hwmon_read,
	.write = emd_hwmon_write,
};

static const u32 emd_hwmon_fan_config[] = {
	HWMON_F_INPUT,
	0,
};

static const struct hwmon_channel_info emd_hwmon_fan_info = {
	.type = hwmon_fan,
	.config = emd_hwmon_fan_config,
};

static const u32 emd_hwmon_power_config[] = {
	HWMON_P_CAP,	/* power1 PL1 */
	HWMON_P_CAP,	/* power2 PL2 */
	HWMON_P_CAP,	/* power3 PL3 */
	HWMON_P_CAP,	/* power4 PL1_DUP */
	HWMON_P_CAP,	/* power5 SYS_PL */
	HWMON_P_CAP,	/* power6 GPU_PL */
	0,
};

static const struct hwmon_channel_info emd_hwmon_power_info = {
	.type = hwmon_power,
	.config = emd_hwmon_power_config,
};

static const struct hwmon_channel_info * const emd_hwmon_info[] = {
	&emd_hwmon_fan_info,
	&emd_hwmon_power_info,
	NULL,
};

static const struct hwmon_chip_info emd_hwmon_chip_info = {
	.ops = &emd_hwmon_ops,
	.info = emd_hwmon_info,
};

/*
 * Custom hwmon attribute: fan_mode (1=AUTO, 2=MAX).
 *
 * Not a standard hwmon attribute - the kernel's hwmon class does not
 * define a "fan mode" attribute, only fan_enable (0/1 toggle) and
 * pwm_enable (0=off, 1=manual, 2=auto, ...). EmdAcpi firmware only
 * exposes two meaningful states, and neither maps cleanly to those
 * standard attributes, so we keep the firmware's encoding verbatim.
 * The attribute lives on the hwmon device so userspace still finds
 * it at /sys/class/hwmon/hwmonN/fan_mode.
 */
static ssize_t fan_mode_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	u8 mode;
	int ret;

	ret = emd_fan_get_mode(dev, &mode);
	if (ret)
		return ret;
	return sysfs_emit(buf, "%u\n", mode);
}

static ssize_t fan_mode_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	unsigned long mode;
	int ret;

	ret = kstrtoul(buf, 0, &mode);
	if (ret)
		return ret;

	ret = emd_fan_set_mode(dev, (u8)mode);
	return ret ?: count;
}
static DEVICE_ATTR_RW(fan_mode);

static struct attribute *emd_hwmon_custom_attrs[] = {
	&dev_attr_fan_mode.attr,
	NULL,
};

static const struct attribute_group emd_hwmon_custom_group = {
	.attrs = emd_hwmon_custom_attrs,
};

static const struct attribute_group *emd_hwmon_extra_groups[] = {
	&emd_hwmon_custom_group,
	NULL,
};

static int emd_power_probe(struct platform_device *pdev)
{
	struct emd_power_priv *priv;
	u8 probe_val;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->dev = &pdev->dev;
	dev_set_drvdata(&pdev->dev, priv);

	/* Read initial power mode for the boot log. */
	ret = emd_ec_read(&pdev->dev, EMD_EC_REG_PWMD, &probe_val);
	if (ret)
		return ret;

	/*
	 * Set fan to AUTO on probe. The EC's fan-control register is
	 * undefined on first power-on and is left at whatever the BIOS
	 * set during boot, which the firmware defaults to MAX. Force
	 * AUTO so the boot state matches the firmware's idle behaviour
	 * and userspace sees a sane value through fan_mode.
	 */
	ret = emd_fan_set_mode(&pdev->dev, EMD_FAN_AUTO);
	if (ret)
		dev_warn(&pdev->dev,
			 "failed to set fan to AUTO on probe: %d\n", ret);

	priv->hwmon_dev = devm_hwmon_device_register_with_info(
				&pdev->dev, "emdoor_fan", priv,
				&emd_hwmon_chip_info,
				emd_hwmon_extra_groups);
	if (IS_ERR(priv->hwmon_dev))
		return PTR_ERR(priv->hwmon_dev);

	priv->profile_dev = devm_platform_profile_register(
				&pdev->dev, "epm", priv,
				&emd_power_profile_ops);
	if (IS_ERR(priv->profile_dev))
		return PTR_ERR(priv->profile_dev);

	dev_info(&pdev->dev,
		 "EmdAcpi power bound (current profile=%u, fan=AUTO)\n",
		 probe_val);
	return 0;
}

static struct platform_driver emd_power_driver = {
	.driver = {
		.name = "emdoor-power",
	},
	.probe = emd_power_probe,
};

static struct platform_device *emd_power_pdev;

/* ------------------------------------------------------------------------- */
/* Charge platform driver (power_supply extension on BAT0)                    */
/* ------------------------------------------------------------------------- */

struct emd_charge_priv {
	struct device *dev;
	struct acpi_battery_hook hook;
	struct power_supply *hooked_psy;
};

static int emd_charge_get_micp(struct emd_charge_priv *priv, u8 *val)
{
	return emd_ec_read(priv->dev, EMD_EC_REG_MICP, val);
}

static int emd_charge_get_mxcp(struct emd_charge_priv *priv, u8 *val)
{
	return emd_ec_read(priv->dev, EMD_EC_REG_MXCP, val);
}

/*
 * Apply the firmware's invariant: MICP = MXCP - 1 (the EC clamps charge
 * between [MICP, MXCP] as a single pair). MXCP is the user-facing upper
 * bound; we only write MXCP here.
 */
static int emd_charge_set_mxcp(struct emd_charge_priv *priv, u8 val)
{
	u8 micp;
	int ret;

	if (val > 100)
		return -EINVAL;
	micp = val > 0 ? val - 1 : 0;

	ret = emd_ec_write(priv->dev, EMD_EC_REG_MXCP, val);
	if (ret)
		return ret;
	ret = emd_ec_write(priv->dev, EMD_EC_REG_MICP, micp);
	return ret;
}

static int emd_charge_ext_get_property(struct power_supply *psy,
				       const struct power_supply_ext *ext,
				       void *data,
				       enum power_supply_property psp,
				       union power_supply_propval *val)
{
	struct emd_charge_priv *priv = data;
	u8 v;
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD:
		ret = emd_charge_get_micp(priv, &v);
		if (ret)
			return ret;
		val->intval = v;
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		ret = emd_charge_get_mxcp(priv, &v);
		if (ret)
			return ret;
		val->intval = v;
		return 0;
	default:
		return -EINVAL;
	}
}

static int emd_charge_ext_set_property(struct power_supply *psy,
				       const struct power_supply_ext *ext,
				       void *data,
				       enum power_supply_property psp,
				       const union power_supply_propval *val)
{
	struct emd_charge_priv *priv = data;
	u8 mxcp;
	int ret;

	if (val->intval < 0 || val->intval > 100)
		return -EINVAL;

	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD:
		/* Re-read MXCP and write the pair (MICP=val, MXCP unchanged). */
		ret = emd_charge_get_mxcp(priv, &mxcp);
		if (ret)
			return ret;
		if (val->intval >= mxcp)
			return -EINVAL;
		return emd_ec_write(priv->dev, EMD_EC_REG_MICP, (u8)val->intval);
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		return emd_charge_set_mxcp(priv, (u8)val->intval);
	default:
		return -EINVAL;
	}
}

static int emd_charge_ext_property_is_writeable(struct power_supply *psy,
						const struct power_supply_ext *ext,
						void *data,
						enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD:
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		return 1;
	default:
		return 0;
	}
}

static const enum power_supply_property emd_charge_ext_properties[] = {
	POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD,
};

static const struct power_supply_ext emd_charge_ext = {
	.name			= "emdoor-charge",
	.properties		= emd_charge_ext_properties,
	.num_properties		= ARRAY_SIZE(emd_charge_ext_properties),
	.get_property		= emd_charge_ext_get_property,
	.set_property		= emd_charge_ext_set_property,
	.property_is_writeable	= emd_charge_ext_property_is_writeable,
};

static int emd_charge_add_battery(struct power_supply *psy,
				  struct acpi_battery_hook *hook)
{
	struct emd_charge_priv *priv = container_of(hook, struct emd_charge_priv, hook);
	int ret;

	if (priv->hooked_psy)
		return 0;

	ret = power_supply_register_extension(psy, &emd_charge_ext, priv->dev, priv);
	if (ret)
		return ret;

	priv->hooked_psy = psy;
	dev_info(priv->dev, "EmdAcpi charge control attached to %s\n",
		 psy->desc->name);
	return 0;
}

static int emd_charge_remove_battery(struct power_supply *psy,
				     struct acpi_battery_hook *hook)
{
	struct emd_charge_priv *priv = container_of(hook, struct emd_charge_priv, hook);

	if (priv->hooked_psy == psy) {
		power_supply_unregister_extension(psy, &emd_charge_ext);
		priv->hooked_psy = NULL;
	}
	return 0;
}

static int emd_charge_probe(struct platform_device *pdev)
{
	struct emd_charge_priv *priv;
	u8 micp, mxcp;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->dev = &pdev->dev;

	/* Probe-time sanity read; failure here means EC IO is dead. */
	ret = emd_charge_get_micp(priv, &micp);
	if (ret)
		return ret;
	ret = emd_charge_get_mxcp(priv, &mxcp);
	if (ret)
		return ret;

	dev_set_drvdata(&pdev->dev, priv);

	priv->hook.name = dev_name(&pdev->dev);
	priv->hook.add_battery = emd_charge_add_battery;
	priv->hook.remove_battery = emd_charge_remove_battery;

	ret = devm_battery_hook_register(&pdev->dev, &priv->hook);
	if (ret)
		return ret;

	if (default_charge_end_threshold >= 0) {
		if (default_charge_end_threshold > 100) {
			dev_warn(&pdev->dev,
				 "default_charge_end_threshold=%d out of range; leaving EC at MXCP=%u\n",
				 default_charge_end_threshold, mxcp);
		} else {
			u8 new_mxcp = (u8)default_charge_end_threshold;
			ret = emd_charge_set_mxcp(priv, new_mxcp);
			if (ret) {
				dev_err(&pdev->dev,
					"failed to apply default_charge_end_threshold=%d: %d\n",
					new_mxcp, ret);
			} else {
				dev_info(&pdev->dev,
					 "EmdAcpi charge control registered (MICP=%u, MXCP=%u -> %u via default_charge_end_threshold)\n",
					 micp, mxcp, new_mxcp);
				mxcp = new_mxcp;
				micp = new_mxcp > 0 ? new_mxcp - 1 : 0;
			}
		}
	} else {
		dev_info(&pdev->dev,
			 "EmdAcpi charge control registered (MICP=%u, MXCP=%u)\n",
			 micp, mxcp);
	}
	return 0;
}

static struct platform_driver emd_charge_driver = {
	.driver = {
		.name = "emdoor-charge",
	},
	.probe = emd_charge_probe,
};

static struct platform_device *emd_charge_pdev;

/* ------------------------------------------------------------------------- */
/* Keyboard RGB WMI driver                                                    */
/* ------------------------------------------------------------------------- */

/*
 * Keyboard RGB (DA -> WMDA, used directly)
 *
 * The DA_APP_KB_LED device (GUID 8600ACCE-FB9B-443E-86F4-3C867398AAE5)
 * implements BST2 via WMDA case 3. This is the only EmdAcpi WMI method
 * that parses correctly on Linux ACPICA - it declares a 0x50-byte local
 * buffer instead of 0x08, so the bogus 63-bit BufferField at offset 0x20
 * sits inside the buffer rather than past the end. The kernel's WMI
 * subsystem evaluates it via wmidev_evaluate_method() directly.
 *
 * The keyboard has 6 logical zones:
 *   Zone 0-3: Keyboard matrix zones (selected via FA00 FACS bits 0-3)
 *   Zone 4:   Left side bar  (selected via FA00 FLAB bit 4 = 0x10)
 *   Zone 5:   Right side bar (selected via FA00 FLAB bit 5 = 0x20)
 *
 * Each zone is exposed as a multicolor LED class device with RGB
 * sub-leds. The led-class-multicolor core handles colour blending and
 * calls our brightness_set_blocking with the computed brightness.
 */
#define EMD_GUID_DA_APP_KB_LED		"8600ACCE-FB9B-443E-86F4-3C867398AAE5"

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

/* Standard 8-byte return layout used by EmdAcpi WMxx methods. */
struct emd_return {
	u16 rt_code;
	u16 value;
	u8  reserved[4];
} __packed;

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
 *   0x0F = all 4 keyboard zones simultaneously (16-byte payload) - unused:
 *         re-arms the LLBR channel and bleeds into the side bars.
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
		ret = status == AE_AML_BUFFER_LIMIT ? -EBUSY : -EIO;
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

static int emd_kbd_send(struct emd_kbd_priv *priv,
			const struct emd_bst_input *in)
{
	struct emd_return r;
	int ret;

	ret = emd_wmi_call(priv->wdev, EMD_WMDA_CASE_SET,
			   in, sizeof(*in), &r, sizeof(r));
	if (ret)
		return ret;
	if (r.rt_code != 0)
		return -EIO;
	return 0;
}

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

static int emd_kbd_apply_static(struct emd_kbd_priv *priv)
{
	unsigned int i;
	int ret;

	for (i = 0; i < EMD_NUM_ZONES; i++) {
		ret = emd_kbd_apply_zone(&priv->leds[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static int emd_kbd_apply_dynamic(struct emd_kbd_priv *priv)
{
	return emd_kbd_apply_zone(&priv->leds[0]);
}

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
 *
 * The first call immediately after WMI bind sometimes returns KBTE=0
 * (the EC hasn't finished initialising yet). Retry up to 3 times with
 * a small delay before giving up.
 */
static int emd_kbd_probe_type(struct emd_kbd_priv *priv)
{
	u8 args[8] = { EMD_WMDA_DTID_KBD_TYPE };
	u8 resp[16] = { };
	int ret, tries;

	for (tries = 0; tries < 3; tries++) {
		ret = emd_wmi_call(priv->wdev, EMD_WMDA_CASE_GET_STATUS,
				   args, sizeof(args), resp, sizeof(resp));
		if (ret)
			return ret;

		if (resp[2] != 1) {
			dev_err(&priv->wdev->dev,
				"WMDA DTID=2 returned RTS0=%u (expected 1)\n",
				resp[2]);
			return -EIO;
		}
		priv->kbte = resp[3];
		priv->kbmax = resp[5];
		if (priv->kbte != 0)
			return 0;

		msleep(50);
	}
	return 0;
}

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
	mc->led_cdev.brightness = 1;	/* see comment below */
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

	/*
	 * brightness must be max_brightness (1) at init: the kernel's
	 * multi_intensity_store calls led_set_brightness(cdev,
	 * cdev->brightness) after caching the new subled intensities;
	 * if the cached brightness were 0, every subsequent write
	 * would silently reduce to "off" because the handler would be
	 * called with brightness=0. With brightness=1, brightness is
	 * left untouched by colour writes and the cached intensity
	 * reaches the EC.
	 */
	return devm_led_classdev_multicolor_register(&priv->wdev->dev, mc);
}

static void emd_kbd_remove(struct wmi_device *wdev)
{
	struct emd_kbd_priv *priv = dev_get_drvdata(&wdev->dev);

	if (!priv)
		return;
	priv->removing = true;
	/* All LED class devices are devm-managed and unregister on detach. */
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

/*
 * Module initialization - registers 3 drivers in dependency order:
 *   1. emdoor-power   - platform_profile + hwmon (fan1_input + fan_mode +
 *                       power[1-6]_cap) on platform device `emdoor-power`
 *   2. emdoor-charge  - power_supply extension on BAT0 with
 *                       charge_control_{start,end}_threshold on platform
 *                       device `emdoor-charge`
 *   3. emdoor-kbd     - WMI driver for the keyboard (6 multicolor LEDs
 *                       + mode control surface)
 *
 * All-or-nothing: failure rolls back previous registrations.
 */
static int __init emd_wmi_init(void)
{
	int ret;

	ret = emd_dmi_check();
	if (ret)
		return ret;

	emd_power_pdev = platform_device_register_simple("emdoor-power",
							-1, NULL, 0);
	if (IS_ERR(emd_power_pdev))
		return PTR_ERR(emd_power_pdev);

	ret = platform_driver_register(&emd_power_driver);
	if (ret) {
		platform_device_unregister(emd_power_pdev);
		return ret;
	}

	emd_charge_pdev = platform_device_register_simple("emdoor-charge",
							-1, NULL, 0);
	if (IS_ERR(emd_charge_pdev)) {
		ret = PTR_ERR(emd_charge_pdev);
		goto err_unregister_power_driver;
	}

	ret = platform_driver_register(&emd_charge_driver);
	if (ret) {
		platform_device_unregister(emd_charge_pdev);
		goto err_unregister_power_driver;
	}

	ret = wmi_driver_register(&emd_kbd_driver);
	if (ret)
		goto err_unregister_charge_driver;

	return 0;

err_unregister_charge_driver:
	platform_driver_unregister(&emd_charge_driver);
	platform_device_unregister(emd_charge_pdev);
err_unregister_power_driver:
	platform_driver_unregister(&emd_power_driver);
	platform_device_unregister(emd_power_pdev);
	return ret;
}

static void __exit emd_wmi_exit(void)
{
	wmi_driver_unregister(&emd_kbd_driver);
	platform_driver_unregister(&emd_charge_driver);
	platform_device_unregister(emd_charge_pdev);
	platform_driver_unregister(&emd_power_driver);
	platform_device_unregister(emd_power_pdev);
}

module_init(emd_wmi_init);
module_exit(emd_wmi_exit);

MODULE_AUTHOR("fewtarius");
MODULE_DESCRIPTION("EmdAcpi power, fan, charge, and six-zone RGB driver (Nimo, Xuanpai, MetaMech)");
MODULE_LICENSE("GPL");

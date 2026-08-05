// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EmdAcpi WMI driver for Nimo, Xuanpai, and MetaMech laptops
 *
 * Supports the EmdAcpi power-mode and six-zone RGB keyboard WMI
 * devices on the EmdAcpi firmware family. The driver is restricted
 * to three known products; see `emd_dmi_table` below. Hardware owners
 * with new products in this firmware family can submit their DMI
 * strings via a PR to extend the whitelist.
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
 *     0x50 buffer. Use wmidev_evaluate_method() directly.
 *
 * Copyright (C) 2026 fewtarius
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/errno.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/kernel.h>
#include <linux/led-class-multicolor.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_profile.h>
#include <linux/types.h>
#include <linux/wmi.h>

/* ------------------------------------------------------------------------- */
/* DMI whitelist                                                              */
/* ------------------------------------------------------------------------- */

/*
 * Supported products. The driver refuses to load on hardware that does
 * not match one of these entries; this prevents the WMI driver from
 * binding on unrelated EmdAcpi devices with different keyboard or EC
 * layouts.
 *
 * To add a product, drop the placeholder comment block and fill in the
 * strings reported by `cat /sys/class/dmi/id/{board_vendor,board_name}`.
 * udev auto-loads the module once an entry matches.
 */
/*
 * Supported products. The driver refuses to load on hardware that
 * does not match one of these entries; this prevents the WMI driver
 * from binding on unrelated EmdAcpi devices with different keyboard
 * or EC layouts.
 *
 * Awaiting DMI strings from hardware owners:
 *   - Xuanpai Xuanji 16 Strix
 *   - MetaMech 16
 *
 * To add a product, append:
 *
 *   {
 *       .matches = {
 *           DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "<vendor>"),
 *           DMI_EXACT_MATCH(DMI_BOARD_NAME,  "<product>"),
 *       },
 *       .driver_data = (void *)"<ident>",
 *   },
 *
 * udev auto-loads the module once an entry matches.
 */
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

#define EMD_GUID_BF_POWER_MODE        "4BCA6480-4D03-4674-84CB-26B4C8F5CFC2"
#define EMD_GUID_DA_APP_KB_LED        "8600ACCE-FB9B-443E-86F4-3C867398AAE5"

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
		/*
		 * Buffer limit errors are expected for the vendor firmware
		 * (the BIOS AML declares fields that extend beyond their
		 * parent buffer). The caller decides whether to fall back
		 * to an alternate path or retry with a different input
		 * size; we just surface the AE status here.
		 */
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

/* ------------------------------------------------------------------------- */
/* Power mode (BF -> WMBF, with EC IO fallback for the broken BIOS)         */
/* ------------------------------------------------------------------------- */

/*
 * Power-mode BIOS layout. E004() in the EC reads ECPM and writes PWMD
 * (EC register 0x7C). The ECPM values are:
 *   1 = Performance
 *   2 = Balanced
 *   3 = Power Saver / Quiet
 *   4 = Read current power mode (WMBF case 4)
 */
#define EMD_POWER_PROFILE_PERF		1
#define EMD_POWER_PROFILE_BALANCE	2
#define EMD_POWER_PROFILE_QUIET		3
#define EMD_POWER_PROFILE_READ		4

/* EC OperationRegion offset for the PWMD field */
#define EMD_EC_REG_PWMD			0x7C

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
	int ret;

	ret = ec_write(EMD_EC_REG_PWMD, val);
	if (ret)
		dev_err(&priv->wdev->dev,
			"EC write of PWMD (0x7C) failed: %d\n", ret);
	return ret;
}

/* Standard 8-byte return layout used by most EmdAcpi methods */
struct emd_return {
	u16 rt_code;
	u16 value;
	u8  reserved[4];
} __packed;

#define EMD_RT_OK	0

static int emd_power_set(struct emd_power_priv *priv, int profile)
{
	struct emd_return r;
	int ret;

	if (profile < EMD_POWER_PROFILE_PERF ||
	    profile > EMD_POWER_PROFILE_QUIET)
		return -EINVAL;
	if (priv->quirk_broken_wmbf)
		return emd_ec_pwmd_write(priv, (u8)profile);

	ret = emd_wmi_call(priv->wdev, (u8)profile, NULL, 0, &r, sizeof(r));
	if (ret == 0 && r.rt_code == EMD_RT_OK)
		return 0;

	/*
	 * WMI path failed. AE_AML_BUFFER_LIMIT is the vendor BIOS bug
	 * (WMBF has a RES3 field that extends beyond its 8-byte buffer).
	 * Use the EC's PWMD register directly. The EC firmware polls
	 * PWMD for the platform profile label, so the OS contract is
	 * satisfied; the only thing we lose is the fan curve re-tuning
	 * that E004() runs after `PWMD = ECPM`.
	 */
	priv->quirk_broken_wmbf = true;
	return emd_ec_pwmd_write(priv, (u8)profile);
}

static int emd_power_get(struct emd_power_priv *priv, int *profile)
{
	struct emd_return r;
	u8 val;
	int ret;

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

	/*
	 * Probe the WMI path. If WMBF is structurally broken (which the
	 * vendor firmware in this family is), the first call fails with
	 * AE_AML_BUFFER_LIMIT. emd_power_get() notices that, sets the
	 * quirk flag, and retries via EC IO so the initial read returns
	 * a usable value. On healthy firmware the WMI path is taken
	 * transparently.
	 */
	ret = emd_power_get(priv, &probe_val);
	if (ret)
		return ret;

	ppdev = devm_platform_profile_register(&wdev->dev, "emdoor-power",
					       priv, &emd_power_profile_ops);
	if (IS_ERR(ppdev))
		return PTR_ERR(ppdev);

	dev_info(&wdev->dev,
		 "EmdAcpi power mode bound (current=%d, %s)\n",
		 probe_val,
		 priv->quirk_broken_wmbf ? "EC IO fallback" : "WMI path");
	return 0;
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
	.no_singleton = true,
};

/* ------------------------------------------------------------------------- */
/* Keyboard RGB (DA -> WMDA, used directly)                                  */
/* ------------------------------------------------------------------------- */

/*
 * WMDA dispatch. Arg1 selects the case, Arg2 is the input buffer.
 * WMDA is the only well-formed EmdAcpi method (0x50-byte local buffer
 * instead of 0x08).
 *
 * For the keyboard we use:
 *   - Case 2 (DTID=2): read keyboard type (KBTE) and max mode
 *   - Case 3: dispatch BST1 / BST2 / BST3 by KBTE
 *
 * BST1 = single-color, BST2 = 4-zone, BST3 = per-key. The driver only
 * drives BST2.
 *
 * KBTE values:
 *   1 = single color   (BST1)
 *   2 = 4-zone RGB     (BST2)
 *   3 = per-key RGB    (BST3)
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
#define EMD_WMDA_FA06_OFF		0xFE	/* blackout all zones */
#define EMD_FA06_MODE_SKIP		0xFE	/* wire: skip mode-select */
#define EMD_KBTE_FOUR_ZONE		0x02

#define EMD_NUM_ZONES			6
#define EMD_CONTROL_SKIP		0xFE

/* FA00 fields (BST2 arg2 byte 0):
 *   bits 0-3 (FACS): keyboard zone selector
 *     0       = no FACS write
 *     1,2,4,8 = zones 1,2,3,4 individually (ECMB=0x20, 4 bytes)
 *     0x0F    = all 4 keyboard zones at once (ECMB=0x20, 16 bytes)
 *   bits 4-5 (FLAB): side bar channel selector
 *     0x00    = no LLBR action
 *     0x10    = left side bar only (ECMB=0x40, 4 bytes, LLB0=0)
 *     0x20    = right side bar only (ECMB=0x40, 4 bytes, LLB0=1)
 *     0x30    = both side bars at once (ECMB=0x40, 8 bytes, RGB dup)
 *
 * Bytes 4-5 are firmware controls that this driver leaves unchanged by
 * writing 0xFE.
 * FA06 (byte 6): effect mode (see enum above)
 * FA07 (byte 7): 0 = off (early-return off path), non-zero = leave on
 *
 * The 6 logical zones:
 *   zone1..zone4 = keyboard regions (FACS bits 0-3)
 *   zone5        = left side bar (FLAB bit 4, LLB0=0)
 *   zone6        = right side bar (FLAB bit 5, LLB0=1)
 */
#define EMD_FA00_FACS_ZONE1		0x01
#define EMD_FA00_FACS_ZONE2		0x02
#define EMD_FA00_FACS_ZONE3		0x04
#define EMD_FA00_FACS_ZONE4		0x08
#define EMD_FA00_FLAB_BAR0		0x10	/* left side bar (LLB0=0) */
#define EMD_FA00_FLAB_BAR1		0x20	/* right side bar (LLB0=1) */

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
	bool removing;		/* driver is being unloaded */
	u8 kbte;
	u8 kbmax;
	u8 mode;		/* last mode written to firmware */
};

static int emd_kbd_apply_dynamic(struct emd_kbd_priv *priv);
static int emd_kbd_apply_static(struct emd_kbd_priv *priv);
static int emd_kbd_mc_brightness_set(struct led_classdev *led_cdev,
				     enum led_brightness brightness);

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

/* FA00 selector for each logical zone: 4 keyboard zones then 2 side bars. */
static const u8 emd_kbd_zone_sel[EMD_NUM_ZONES] = {
	EMD_FA00_FACS_ZONE1, EMD_FA00_FACS_ZONE2,
	EMD_FA00_FACS_ZONE3, EMD_FA00_FACS_ZONE4,
	EMD_FA00_FLAB_BAR0,	EMD_FA00_FLAB_BAR1,
};

/*
 * Physical layout on the Nimo N161L (and any other 4-zone EmdAcpi
 * device reporting KBTE=2):
 *
 *   +------+------+------+------+------+------+
 *   |      |      |      |      |      |      |
 *   | bar- | z1   | z2   | z3   | z4   | bar- |
 *   | left | left | left | right| right| right|
 *   |      | quad | ctr  | ctr  |      |      |
 *   |      |      |      |      |      |      |
 *   +------+------+------+------+------+------+
 *
 * z1 = FACS bit 0   (keyboard left quadrant)
 * z2 = FACS bit 1   (keyboard left-of-center)
 * z3 = FACS bit 2   (keyboard right-of-center)
 * z4 = FACS bit 3   (keyboard right quadrant)
 * bar-left  = FLAB bit 4, LLB0=0  (left chassis bar)
 * bar-right = FLAB bit 5, LLB0=1  (right chassis bar)
 *
 */
/* LED class device names. The chassis has six physical zones (four
 * keyboard regions plus two side bars); each gets its own multicolor
 * LED class device. Names follow the kernel convention
 * <devicename>:multicolor:<function> so userspace can scan /sys/class/leds
 * for the emdoor:multicolor:* prefix.
 */
static const char *const emd_kbd_led_names[EMD_NUM_ZONES] = {
	"emdoor:multicolor:zone1",
	"emdoor:multicolor:zone2",
	"emdoor:multicolor:zone3",
	"emdoor:multicolor:zone4",
	"emdoor:multicolor:bar-left",
	"emdoor:multicolor:bar-right",
};

/* BST2 input is 8 bytes. */
struct emd_bst_input {
	u8 fa00;	/* bits 0-3: keyboard zone, bits 4-5: side bar */
	u8 r;
	u8 g;
	u8 b;
	u8 control[2];	/* firmware controls, 0xFE to preserve */
	u8 effect;	/* FA06: effect mode */
	u8 on;		/* FA07: 0 = turn off, non-zero = leave on */
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

static int emd_kbd_apply(struct emd_kbd_priv *priv)
{
	struct emd_bst_input in;

	/*
	 * BST2 dispatch:
	 *   FA06 == 0xFF     -> factory reset
	 *   FA07 == 0        -> turn off (EDTA=0, early return)
	 *   FA06 != 0xFE     -> run mode-select switch:
	 *     FA06 == 0..8   -> set mode (EDTA = 2,5,4,3,6,7,8,9,10)
	 *     FA06 == 0      -> static, then ECMB=0x18 with [R,G,B]
	 *     FA06 == 3      -> breath, then ECMB=0x18 with [R,G,B]
	 *   FA00 bits 4-5 (FLAB) -> LLBR side bars via ECMB=0x40
	 *     FLAB == 0x10 -> left bar (LLB0=0)
	 *     FLAB == 0x20 -> right bar (LLB0=1)
	 *     FLAB == 0x30 -> both bars (RGB duplicated, 8 bytes)
	 * Static mode sends one BST2 with FA06=0 to install the static
	 * baseline (which also runs ECMB=0x18 with the FA01/02/03 RGB),
	 * then per-zone writes with FA06=0xFE so BST2 skips the
	 * baseline and only updates the targeted FACS/FLAB channel.
	 * Re-running the baseline between zones would clobber every
	 * other zone with the per-zone color, which the user sees as
	 * flicker.
	 */
	if (priv->mode == EMD_WMDA_FA06_OFF) {
		memset(&in, 0, sizeof(in));
		return emd_kbd_send(priv, &in);
	}

	if (priv->mode == EMD_WMDA_FA06_RESET) {
		memset(&in, 0, sizeof(in));
		in.effect = 0xFF;	/* triggers FCFN() inside BST2 */
		in.on = 0xFF;
		return emd_kbd_send(priv, &in);
	}

	if (priv->mode == EMD_WMDA_FA06_ALWAYS)
		return emd_kbd_apply_static(priv);

	return emd_kbd_apply_dynamic(priv);
}

/** emd_kbd_apply_zone - write one zone's cached RGB to firmware.
 *
 * Off and reset modes return early; the firmware state for those is
 * set by mode_store. Static mode sends a single BST2 with FA06=0xFE
 * and the zone's FA00 selector; dynamic mode sends one BST2 with
 * FA06=mode and FA00=0 (the firmware animates from the RGB).
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
		return 0;	/* firmware state set by mode_store */

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
	struct emd_kbd_led *led = &priv->leds[0];
	unsigned int i;
	int ret;

	/* One BST2 with FA06=0 installs the static baseline (which also
	 * runs ECMB=0x18 with the FA01/02/03 RGB). After that, per-zone
	 * writes use FA06=0xFE so BST2 skips the baseline and only
	 * updates the targeted FACS/FLAB channel.
	 */
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

static int emd_kbd_apply_dynamic(struct emd_kbd_priv *priv)
{
	return emd_kbd_apply_zone(&priv->leds[0]);
}

static int emd_kbd_mc_brightness_set(struct led_classdev *led_cdev,
				     enum led_brightness brightness)
{
	struct emd_kbd_led *led =
		container_of(lcdev_to_mccdev(led_cdev),
			     struct emd_kbd_led, mc);
	struct emd_kbd_priv *priv = led->priv;
	int ret;

	/* Don't write to hardware during driver unload - preserve state */
	if (priv->removing)
		return 0;

	ret = mutex_lock_interruptible(&priv->lock);
	if (ret)
		return ret;
	ret = emd_kbd_apply_zone(led);
	mutex_unlock(&priv->lock);
	return ret;
}

static int emd_kbd_probe_type(struct emd_kbd_priv *priv)
{
	u8 args[8] = { EMD_WMDA_DTID_KBD_TYPE };
	u8 resp[16] = { };
	int ret;

	/*
	 * WMDA declares CreateByteField (Arg2, 0x02, DEST) at the top of
	 * its body, so the input buffer must be at least 3 bytes even
	 * though only byte 0 (DTID) is read. We pass 8 to satisfy the
	 * bounds check and to match BST1/BST2/BST3's input layout.
	 */
	ret = emd_wmi_call(priv->wdev, EMD_WMDA_CASE_GET_STATUS,
			   args, sizeof(args), resp, sizeof(resp));
	if (ret)
		return ret;

	/* Response layout: RTS0 is byte 2, KBTE is byte 3, KBMX is byte 5. */
	if (resp[2] != 1) {
		dev_err(&priv->wdev->dev,
			"WMDA DTID=2 returned RTS0=%u (expected 1)\n", resp[2]);
		return -EIO;
	}
	priv->kbte = resp[3];
	priv->kbmax = resp[5];
	return 0;
}

/* ---- sysfs attributes ---- */

static ssize_t type_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct emd_kbd_priv *priv = dev_get_drvdata(dev);

	if (priv->kbte >= ARRAY_SIZE(emd_kbte_str) || !emd_kbte_str[priv->kbte])
		return sysfs_emit(buf, "unknown\n");
	return sysfs_emit(buf, "%s\n", emd_kbte_str[priv->kbte]);
}
static DEVICE_ATTR_RO(type);

static ssize_t mode_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct emd_kbd_priv *priv = dev_get_drvdata(dev);
	const char *mode = "unknown";

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
	struct emd_kbd_priv *priv = dev_get_drvdata(dev);
	u8 mode, old_mode;
	int ret;

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

/*
 * `modes` mirrors `platform_profile_choices`: read-only list of all
 * valid values accepted by `mode`. Userspace can read this to discover
 * the supported set without hardcoding the OEM names. The convention
 * follows Documentation/ABI/testing/sysfs-platform-profile.
 */
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

/* Per-zone color and zone color sysfs attrs are gone; the LED class
 * multicolor interface (multi_intensity, brightness) replaces them.
 */

static struct attribute *emd_kbd_attrs[] = {
	&dev_attr_type.attr,
	&dev_attr_mode.attr,
	&dev_attr_modes.attr,
	NULL,
};

static const struct attribute_group emd_kbd_attr_group = {
	.attrs = emd_kbd_attrs,
};

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
	/*
	 * Intensity defaults to 0, not white. Firmware has no per-zone RGB
	 * readback, and brightness=0 here would make the kernel call our
	 * brightness_set with 0 on every multi_intensity write (it uses
	 * the cached brightness, not max_brightness), turning real writes
	 * into no-ops. Brightness=1 with intensity=0 means "on, dark" -
	 * multi_intensity writes pass through to hardware correctly.
	 */
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
	return led_classdev_multicolor_register(&priv->wdev->dev, mc);
}

static void emd_kbd_remove(struct wmi_device *wdev)
{
	struct emd_kbd_priv *priv = dev_get_drvdata(&wdev->dev);
	unsigned int i;

	if (!priv)
		return;

	priv->removing = true;

	for (i = 0; i < EMD_NUM_ZONES; i++) {
		struct led_classdev_mc *mc = &priv->leds[i].mc;

		led_classdev_unregister(&mc->led_cdev);
	}
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

	ret = devm_device_add_group(&wdev->dev, &emd_kbd_attr_group);
	if (ret)
		return ret;

	dev_info(&wdev->dev,
		 "EmdAcpi keyboard backlight bound (type=%s max=%u)\n",
		 (priv->kbte < ARRAY_SIZE(emd_kbte_str) &&
		  emd_kbte_str[priv->kbte]) ?
			 emd_kbte_str[priv->kbte] : "unknown",
		 priv->kbmax);
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

static int __init emd_wmi_init(void)
{
	int ret;

	ret = emd_dmi_check();
	if (ret)
		return ret;

	ret = wmi_driver_register(&emd_power_driver);
	if (ret)
		return ret;
	ret = wmi_driver_register(&emd_kbd_driver);
	if (ret) {
		wmi_driver_unregister(&emd_power_driver);
		return ret;
	}
	return 0;
}

static void __exit emd_wmi_exit(void)
{
	wmi_driver_unregister(&emd_kbd_driver);
	wmi_driver_unregister(&emd_power_driver);
}

module_init(emd_wmi_init);
module_exit(emd_wmi_exit);

MODULE_AUTHOR("fewtarius");
MODULE_DESCRIPTION("EmdAcpi power and six-zone RGB driver (Nimo, Xuanpai, MetaMech)");
MODULE_LICENSE("GPL");

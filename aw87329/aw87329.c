// SPDX-License-Identifier: GPL-2.0
/*
 * Edited by: grayfox951 <admin@dnr.qzz.io>
 */

#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#define AW87329_DRV_NAME	"aw87329"
#define AW87329_DRV_VERSION	"1.0.0"

/* -------------------------------------------------------------------------
 * Register map - verbatim from tp/audio_pa/aw87329_audio.c:56-66
 * ---------------------------------------------------------------------- */
#define AW87329_REG_CHIPID	0x00
#define AW87329_REG_SYSCTRL	0x01
#define AW87329_REG_MODECTRL	0x02
#define AW87329_REG_CPOVP	0x03
#define AW87329_REG_CPP		0x04
#define AW87329_REG_GAIN	0x05
#define AW87329_REG_AGC3_PO	0x06
#define AW87329_REG_AGC3	0x07
#define AW87329_REG_AGC2_PO	0x08
#define AW87329_REG_AGC2	0x09
#define AW87329_REG_AGC1	0x0a

/* Highest register the vendor driver considers addressable. */
#define AW87329_REG_MAX		0x0f

/* Value written to SYSCTRL to park the amp. aw87329_audio.c:68 */
#define AW87329_CHIP_DISABLE	0x0c

/* Chip ID the vendor driver insists on. aw87329_audio.c:720 */
#define AW87329_CHIP_ID		0x39

/* SYSCTRL bit mask cleared for the duration of a config write.
 * aw87329_audio.c:260, 291, 322 - "&0xF7", i.e. clear bit 3. */
#define AW87329_SYSCTRL_STAGE_MASK	0xf7

/* Number of configuration bytes per mode: registers 0x01..0x0a inclusive.
 * The vendor's 11-byte arrays (aw87329_audio.c:127-135) are indexed by
 * register number, and index 0 is the chip ID, which is never written.
 * So the payload is indexes 1..10. */
#define AW87329_CFG_LEN		10

/* I2C retry policy - aw87329_audio.c:51-54 */
#define AW87329_I2C_RETRIES	5
#define AW87329_I2C_RETRY_DELAY_US	2000

/* Reset pulse timing - aw87329_audio.c:186-206 (msleep(2) twice) */
#define AW87329_RESET_DELAY_US	2000

enum aw87329_mode {
	AW87329_MODE_OFF = 0,
	AW87329_MODE_KSPK,	/* speaker - the one this board uses */
	AW87329_MODE_DRCV,	/* digital receiver / voice */
	AW87329_MODE_ABRCV,	/* analog receiver / voice */
	AW87329_MODE_NR
};

static const char * const aw87329_mode_names[AW87329_MODE_NR] = {
	"off", "kspk", "drcv", "abrcv",
};

/*
 * Factory DSP configuration, copied byte for byte from the vendor driver.
 * Index i corresponds to register (AW87329_REG_SYSCTRL + i).
 *
 *   aw87329_kspk_cfg_default[]  aw87329_audio.c:127
 *   aw87329_drcv_cfg_default[]  aw87329_audio.c:130
 *   aw87329_abrcv_cfg_default[] aw87329_audio.c:133
 *
 * The leading 0x39 in each vendor array is the chip ID at register 0x00 and is
 * dropped here for the reason above.
 */
static const u8 aw87329_kspk_cfg_default[AW87329_CFG_LEN] = {
	0x0e, 0xa3, 0x06, 0x05, 0x10, 0x07, 0x52, 0x06, 0x08, 0x96,
};

static const u8 aw87329_drcv_cfg_default[AW87329_CFG_LEN] = {
	0x0a, 0xab, 0x06, 0x05, 0x00, 0x0f, 0x52, 0x09, 0x08, 0x97,
};

static const u8 aw87329_abrcv_cfg_default[AW87329_CFG_LEN] = {
	0x0a, 0xaf, 0x06, 0x05, 0x00, 0x0f, 0x52, 0x09, 0x08, 0x97,
};

struct aw87329 {
	struct i2c_client	*client;
	struct device		*dev;
	struct regmap		*regmap;
	struct regulator	*vdd;
	struct gpio_desc	*reset_gpiod;

	struct mutex		lock;	/* serialises config writes */
	enum aw87329_mode	mode;
	bool			powered;
	bool			probed;
};

/* -------------------------------------------------------------------------
 * Module parameters
 * ---------------------------------------------------------------------- */

/*
 * GAIN register (0x05) code. Default 0x10 is the value the vendor driver
 * writes in speaker mode - aw87329_kspk_cfg_default[] index 4,
 * aw87329_audio.c:128. The voice modes use 0x00 there, so this default is
 * correct only for the speaker path; set it to 0x00 to match drcv/abrcv.
 */
static unsigned int gain = 0x10;
module_param(gain, uint, 0644);
MODULE_PARM_DESC(gain, "GAIN register (0x05) code, 0x00-0xff. Vendor kspk default 0x10");

/*
 * Mode this amplifier runs in. Used by the exported spk_enable() and by the
 * "mode" sysfs attribute. kspk is correct for a speaker PA.
 *
 * dsp_cfg (above) overrides the compiled-in payload of exactly this mode.
 */
static unsigned int default_mode = AW87329_MODE_KSPK;
module_param(default_mode, uint, 0644);
MODULE_PARM_DESC(default_mode, "Default mode: 0=off 1=kspk(speaker) 2=drcv 3=abrcv");

/*
 * The DSP config array. Default is the vendor kspk payload above. Write
 * "0x0e 0xa3 0x06 ..." (10 bytes, registers 0x01..0x0a) to replace it at
 * runtime, e.g.
 *     echo "0x0e 0xa3 0x06 0x05 0x10 0x07 0x52 0x06 0x08 0x96" \
 *         > /sys/module/aw87329/parameters/dsp_cfg
 *
 * Re-arm the amplifier afterwards (enable toggle) for the change to be pushed.
 */
static u8 dsp_cfg[AW87329_CFG_LEN] = {
	0x0e, 0xa3, 0x06, 0x05, 0x10, 0x07, 0x52, 0x06, 0x08, 0x96,
};

/*
 * Accepts "0x0e 0xa3 ..." and "0x0e,0xa3,...". Values are always hex, with
 * an optional 0x/0X prefix, which is what a register byte looks like. Exactly
 * AW87329_CFG_LEN bytes are required; anything shorter, longer or malformed is
 * rejected outright, because a silently half-applied PA configuration is
 * worse than a clean error.
 *
 * Note this parses hex itself rather than using kstrtouint(): base-0 parsing
 * would read a bare "12" as decimal 12 and stop at the 'a' in "12ab", which
 * is not what a register dump means.
 */
static int aw87329_dsp_cfg_set(const char *val, const struct kernel_param *kp)
{
	u8 *cfg = kp->arg;
	u8 tmp[AW87329_CFG_LEN];
	char tok[24];
	const char *s = val;
	unsigned int i, j, byte;
	size_t n, prefix;

	if (!val)
		return -EINVAL;

	memset(tmp, 0, sizeof(tmp));

	for (i = 0; i < AW87329_CFG_LEN; i++) {
		while (isspace((unsigned char)*s) || *s == ',')
			s++;
		if (!*s)
			return -EINVAL;

		/* Copy the token, then validate it. Validating before any
		 * numeric conversion is what stops "12abc" or "0xzz" from
		 * being silently accepted. */
		n = 0;
		while (s[n] && !isspace((unsigned char)s[n]) && s[n] != ',') {
			if (n >= sizeof(tok) - 1)
				return -EINVAL;
			tok[n] = s[n];
			n++;
		}
		s += n;
		tok[n] = '\0';

		/* Optional "0x"/"0X" prefix, then one or two hex digits. */
		prefix = (n >= 2 && tok[0] == '0' &&
			  (tok[1] == 'x' || tok[1] == 'X')) ? 2 : 0;
		if (n - prefix < 1 || n - prefix > 2)
			return -EINVAL;

		byte = 0;
		for (j = prefix; j < n; j++) {
			if (!isxdigit((unsigned char)tok[j]))
				return -EINVAL;

			byte <<= 4;
			if (tok[j] >= '0' && tok[j] <= '9')
				byte |= (unsigned int)(tok[j] - '0');
			else if (tok[j] >= 'a' && tok[j] <= 'f')
				byte |= (unsigned int)(tok[j] - 'a' + 10);
			else
				byte |= (unsigned int)(tok[j] - 'A' + 10);
		}

		tmp[i] = (u8)byte;
	}

	while (isspace((unsigned char)*s) || *s == ',')
		s++;
	if (*s)
		return -EINVAL;

	memcpy(cfg, tmp, sizeof(tmp));
	return 0;
}

static int aw87329_dsp_cfg_get(char *buffer, const struct kernel_param *kp)
{
	const u8 *cfg = kp->arg;
	unsigned int i;
	int off = 0;

	for (i = 0; i < AW87329_CFG_LEN; i++)
		off += sysfs_emit_at(buffer, off, "%s0x%02x",
				     i ? " " : "", cfg[i]);

	return off;
}

static const struct kernel_param_ops aw87329_dsp_cfg_ops = {
	.set	= aw87329_dsp_cfg_set,
	.get	= aw87329_dsp_cfg_get,
};

module_param_cb(dsp_cfg, &aw87329_dsp_cfg_ops, dsp_cfg, 0644);
MODULE_PARM_DESC(dsp_cfg, "DSP config for regs 0x01-0x0a, 10 hex bytes");

/*
 * Force the reset line active-high.
 *
 * The stock DT says GPIO_ACTIVE_LOW and the vendor driver ignored that flag
 * (it used the legacy integer GPIO API, aw87329_audio.c:683). With the
 * descriptor API the flag is honoured, which reproduces the vendor's physical
 * levels exactly, so this stays 0 unless a board variant wires it the other
 * way and probe fails the chip ID read.
 */
static bool reset_active_high;
module_param(reset_active_high, bool, 0644);
MODULE_PARM_DESC(reset_active_high, "Invert reset polarity; 0 = honour DT GPIO_ACTIVE_LOW");

/* -------------------------------------------------------------------------
 * Low level I2C, with the vendor retry policy
 * ---------------------------------------------------------------------- */

static int aw87329_read_reg(struct aw87329 *aw, unsigned int reg, u8 *val)
{
	unsigned int cnt;
	unsigned int raw;
	int ret = -EIO;

	for (cnt = 0; cnt < AW87329_I2C_RETRIES; cnt++) {
		/* regmap_read() takes an unsigned int; narrow to the 8-bit
		 * register width the part actually has. */
		ret = regmap_read(aw->regmap, reg, &raw);
		if (!ret) {
			*val = (u8)raw;
			return 0;
		}

		dev_dbg(aw->dev, "read reg 0x%02x try %u failed: %d\n",
			reg, cnt, ret);
		usleep_range(AW87329_I2C_RETRY_DELAY_US,
			     AW87329_I2C_RETRY_DELAY_US + 500);
	}

	return ret;
}

/*
 * Write one configuration register. No retry loop: the vendor driver retried
 * writes, but replaying a partially-applied config sequence is worse than
 * reporting the error, so the sequence is abandoned on the first failure and
 * the caller parks the chip.
 */
static int aw87329_write_reg(struct aw87329 *aw, unsigned int reg, u8 val)
{
	return regmap_write(aw->regmap, reg, val);
}

/* -------------------------------------------------------------------------
 * Hardware enable / reset
 * ---------------------------------------------------------------------- */

/*
 * Bring the amp out of reset, or park it.
 *
 * The legacy driver wrote raw pin levels: 0 then 1 to come up, 0 to go down
 * (aw87329_audio.c:186-206). The DT flags the line GPIO_ACTIVE_LOW, so under
 * the descriptor API physical LOW is logical 1. A raw 0 is therefore logical
 * 1 (assert) and a raw 1 is logical 0 (deassert), and:
 *
 *	hw_reset()  : assert (1) -> deassert (0)   == vendor raw 0 -> 1
 *	hw_off()    : assert (1)                   == vendor raw 0
 *
 * The active-low flag and the vendor's raw values agree, which is a useful
 * cross-check that this mapping is right.
 */
static void aw87329_reset_set(struct aw87329 *aw, bool assert_reset)
{
	/* Raw pin levels, because the descriptor carries the DT polarity flag
	 * and the reset_active_high override has to bypass it. */
	int raw_assert = reset_active_high ? 1 : 0;

	if (!aw->reset_gpiod)
		return;

	gpiod_set_raw_value_cansleep(aw->reset_gpiod,
				     assert_reset ? raw_assert : !raw_assert);
}

static void aw87329_hw_reset(struct aw87329 *aw)
{
	if (!aw->reset_gpiod)
		return;

	aw87329_reset_set(aw, true);
	usleep_range(AW87329_RESET_DELAY_US, AW87329_RESET_DELAY_US + 500);
	aw87329_reset_set(aw, false);
	usleep_range(AW87329_RESET_DELAY_US, AW87329_RESET_DELAY_US + 500);
}

static void aw87329_hw_off(struct aw87329 *aw)
{
	if (!aw->reset_gpiod)
		return;

	aw87329_reset_set(aw, true);
	usleep_range(AW87329_RESET_DELAY_US, AW87329_RESET_DELAY_US + 500);
}

/* -------------------------------------------------------------------------
 * Chip ID
 * ---------------------------------------------------------------------- */

static int aw87329_read_chipid(struct aw87329 *aw, u8 *id)
{
	unsigned int cnt;
	int ret;

	for (cnt = 0; cnt < AW87329_I2C_RETRIES; cnt++) {
		ret = aw87329_read_reg(aw, AW87329_REG_CHIPID, id);
		if (ret)
			continue;

		if (*id == AW87329_CHIP_ID) {
			dev_info(aw->dev, "chip id 0x%02x\n", *id);
			return 0;
		}

		dev_info(aw->dev, "chip id 0x%02x unexpected, retry %u\n",
			 *id, cnt);
		usleep_range(AW87329_I2C_RETRY_DELAY_US,
			     AW87329_I2C_RETRY_DELAY_US + 500);
	}

	return -ENODEV;
}

/* -------------------------------------------------------------------------
 * Configuration push
 * ---------------------------------------------------------------------- */

static const u8 *aw87329_cfg_for_mode(enum aw87329_mode m)
{
	switch (m) {
	case AW87329_MODE_KSPK:
		return aw87329_kspk_cfg_default;
	case AW87329_MODE_DRCV:
		return aw87329_drcv_cfg_default;
	case AW87329_MODE_ABRCV:
		return aw87329_abrcv_cfg_default;
	default:
		return NULL;
	}
}

/*
 * Push a configuration, reproducing the vendor write sequence exactly
 * (aw87329_audio.c:260-270 for kspk, 291-301 for drcv, 322-332 for abrcv):
 *
 *	SYSCTRL &= ~0x08	stage the chip so the new config is not latched
 *	MODECTRL, CPOVP, CPP, GAIN, AGC3_PO, AGC3, AGC2_PO, AGC2, AGC1
 *	SYSCTRL  = full value	commit
 *
 * SYSCTRL is deliberately written twice. Do not "optimise" it away.
 */
static int aw87329_push_config(struct aw87329 *aw, const u8 *cfg)
{
	unsigned int i;
	u8 v;
	int ret;

	/* Register 0x01, bit 3 cleared. */
	ret = aw87329_write_reg(aw, AW87329_REG_SYSCTRL,
				cfg[0] & AW87329_SYSCTRL_STAGE_MASK);
	if (ret)
		return ret;

	/* Registers 0x02..0x0a. GAIN (0x05) is overridable at runtime. */
	for (i = 1; i < AW87329_CFG_LEN; i++) {
		unsigned int reg = AW87329_REG_SYSCTRL + i;

		v = (reg == AW87329_REG_GAIN) ? (u8)gain : cfg[i];

		ret = aw87329_write_reg(aw, reg, v);
		if (ret)
			return ret;
	}

	/* Register 0x01, full value: commit. */
	return aw87329_write_reg(aw, AW87329_REG_SYSCTRL, cfg[0]);
}

static int aw87329_set_mode_locked(struct aw87329 *aw, enum aw87329_mode m)
{
	const u8 *cfg;
	int ret;

	if (m == AW87329_MODE_OFF) {
		ret = aw87329_write_reg(aw, AW87329_REG_SYSCTRL,
					 AW87329_CHIP_DISABLE);
		if (!ret)
			aw->mode = AW87329_MODE_OFF;
		return ret;
	}

	cfg = aw87329_cfg_for_mode(m);
	if (!cfg)
		return -EINVAL;

	/* Live tuning override: dsp_cfg replaces the compiled-in payload of the
	 * mode named by default_mode. The other modes keep their factory
	 * payloads, so a bad override cannot corrupt a path it does not own. */
	if (m == (enum aw87329_mode)default_mode)
		cfg = dsp_cfg;

	ret = aw87329_push_config(aw, cfg);
	if (ret) {
		dev_err(aw->dev, "config write failed: %d\n", ret);
		return ret;
	}

	aw->mode = m;
	return 0;
}

/* -------------------------------------------------------------------------
 * Exported control surface
 * ---------------------------------------------------------------------- */

static struct aw87329 *aw87329_get(struct device *dev)
{
	struct aw87329 *aw;

	aw = dev_get_drvdata(dev);
	if (!aw || !aw->probed)
		return NULL;
	return aw;
}

/**
 * aw87329_spk_enable - power up the amplifier and push its configuration
 * @dev: the AW87329 i2c client device
 *
 * Call from the codec's SPK PA power-up (SND_SOC_DAPM_POST_PMU) so the
 * amplifier comes up together with the codec's speaker path. The mode pushed
 * is the default_mode module parameter, kspk by default.
 *
 * Return: 0 on success, negative errno otherwise.
 */
int aw87329_spk_enable(struct device *dev)
{
	struct aw87329 *aw = aw87329_get(dev);
	unsigned int want = default_mode;
	int ret;

	if (!aw)
		return -ENODEV;

	/* Guard the module parameter; a bad value must not reach the chip. */
	if (want == AW87329_MODE_OFF || want >= AW87329_MODE_NR)
		want = AW87329_MODE_KSPK;

	mutex_lock(&aw->lock);

	if (!aw->powered) {
		aw87329_hw_reset(aw);
		if (aw->vdd) {
			ret = regulator_enable(aw->vdd);
			if (ret) {
				dev_err(aw->dev, "regulator enable: %d\n", ret);
				goto out;
			}
		}
		aw->powered = true;
	}

	ret = aw87329_set_mode_locked(aw, want);

out:
	mutex_unlock(&aw->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(aw87329_spk_enable);

/**
 * aw87329_spk_disable - park the amplifier
 * @dev: the AW87329 i2c client device
 *
 * Call from the codec's SPK PA power-down (SND_SOC_DAPM_POST_PMD).
 *
 * Return: 0 on success, negative errno otherwise.
 */
int aw87329_spk_disable(struct device *dev)
{
	struct aw87329 *aw = aw87329_get(dev);
	int ret;

	if (!aw)
		return -ENODEV;

	mutex_lock(&aw->lock);

	/* aw87329_audio.c:350 - only if the chip is still out of reset. */
	if (aw->powered) {
		ret = aw87329_write_reg(aw, AW87329_REG_SYSCTRL,
					AW87329_CHIP_DISABLE);
		if (ret)
			dev_warn(aw->dev, "disable write: %d\n", ret);
	}

	aw87329_hw_off(aw);
	aw->powered = false;
	aw->mode = AW87329_MODE_OFF;

	if (aw->vdd)
		regulator_disable(aw->vdd);

	mutex_unlock(&aw->lock);
	return 0;
}
EXPORT_SYMBOL_GPL(aw87329_spk_disable);

/* -------------------------------------------------------------------------
 * sysfs
 * ---------------------------------------------------------------------- */

static ssize_t chipid_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct aw87329 *aw = dev_get_drvdata(dev);
	u8 id;
	int ret;

	ret = aw87329_read_reg(aw, AW87329_REG_CHIPID, &id);
	if (ret)
		return ret;

	return sysfs_emit(buf, "0x%02x\n", id);
}
static DEVICE_ATTR_ADMIN_RO(chipid);

static ssize_t gain_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	return sysfs_emit(buf, "0x%02x\n", gain);
}

static ssize_t gain_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct aw87329 *aw = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret)
		return ret;
	if (val > 0xff)
		return -ERANGE;

	mutex_lock(&aw->lock);
	gain = val;
	/* Re-push if live, so the change takes effect without a re-toggle. */
	if (aw->powered && aw->mode != AW87329_MODE_OFF)
		ret = aw87329_set_mode_locked(aw, aw->mode);
	mutex_unlock(&aw->lock);

	return ret ? ret : count;
}
static DEVICE_ATTR_ADMIN_RW(gain);

static ssize_t enable_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct aw87329 *aw = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", aw->powered ? 1 : 0);
}

static ssize_t enable_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;

	ret = on ? aw87329_spk_enable(dev) : aw87329_spk_disable(dev);
	if (ret)
		return ret;

	return count;
}
static DEVICE_ATTR_ADMIN_RW(enable);

static ssize_t mode_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct aw87329 *aw = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d %s\n", aw->mode, aw87329_mode_names[aw->mode]);
}

static ssize_t mode_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct aw87329 *aw = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret)
		return ret;
	if (val >= AW87329_MODE_NR)
		return -ERANGE;

	mutex_lock(&aw->lock);
	default_mode = val;
	if (aw->powered)
		ret = aw87329_set_mode_locked(aw, val);
	else
		ret = 0;
	mutex_unlock(&aw->lock);

	return ret ? ret : count;
}
static DEVICE_ATTR_ADMIN_RW(mode);

/* Read-only register dump, for bring-up. The vendor had a writable one
 * (aw87329_audio.c:567); a writable path onto a PA config register is a
 * foot-gun, so this port omits it. */
static ssize_t registers_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct aw87329 *aw = dev_get_drvdata(dev);
	unsigned int reg;
	int off = 0;
	u8 val;
	int ret;

	for (reg = AW87329_REG_CHIPID; reg <= AW87329_REG_AGC1; reg++) {
		ret = aw87329_read_reg(aw, reg, &val);
		if (ret)
			val = 0xff;

		off += sysfs_emit_at(buf, off, "%02x:%02x\n", reg, val);
	}

	return off;
}
static DEVICE_ATTR_ADMIN_RO(registers);

/*
 * All attributes are DEVICE_ATTR_ADMIN_* (0600/0400) so an unprivileged
 * process cannot reconfigure a speaker amplifier.
 */
static struct attribute *aw87329_attrs[] = {
	&dev_attr_chipid.attr,
	&dev_attr_gain.attr,
	&dev_attr_enable.attr,
	&dev_attr_mode.attr,
	&dev_attr_registers.attr,
	NULL
};

static const struct attribute_group aw87329_attr_group = {
	.attrs	= aw87329_attrs,
	.name	= "aw87329",
};

/* -------------------------------------------------------------------------
 * regmap
 * ---------------------------------------------------------------------- */

static const struct regmap_config aw87329_regmap_config = {
	.max_register	= AW87329_REG_MAX,
	/*
	 * No register cache, deliberately. The vendor sequence writes SYSCTRL
	 * twice with two different values (staged, then committed) and relies on
	 * both reaching the part in order. A regcache would coalesce or reorder
	 * them and the commit would be lost.
	 */
	.cache		= false,
};

/* -------------------------------------------------------------------------
 * Probe / remove
 * ---------------------------------------------------------------------- */

static int aw87329_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct aw87329 *aw;
	int ret;

	aw = devm_kzalloc(dev, sizeof(*aw), GFP_KERNEL);
	if (!aw)
		return -ENOMEM;

	aw->client	= client;
	aw->dev		= dev;
	aw->mode	= AW87329_MODE_OFF;
	mutex_init(&aw->lock);
	dev_set_drvdata(dev, aw);

	/* The vendor checked I2C_FUNC_I2C. regmap-i2c issues plain I2C
	 * transfers, so that is the correct capability to require. */
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return dev_err_probe(dev, -ENODEV,
				     "i2c functionality not supported\n");

	aw->regmap = devm_regmap_init_i2c(client, &aw87329_regmap_config);
	if (IS_ERR(aw->regmap)) {
		ret = PTR_ERR(aw->regmap);
		return dev_err_probe(dev, ret, "regmap init failed\n");
	}

	/* Optional supply. The stock DT has no regulator for this node, so this
	 * returns NULL and probing continues. */
	aw->vdd = devm_regulator_get_optional(dev, "vdd");
	if (IS_ERR(aw->vdd)) {
		ret = PTR_ERR(aw->vdd);
		if (ret == -EPROBE_DEFER)
			return dev_err_probe(dev, ret, "vdd not ready\n");

		/* Any other error: treat the rail as absent rather than failing. */
		dev_warn(dev, "vdd supply unusable (%d), continuing\n", ret);
		aw->vdd = NULL;
	}

	/*
	 * Reset line. Requested with con_id "reset", which resolves the
	 * "reset-gpios" property. The stock DT uses the nonstandard singular
	 * "reset-gpio"; gpiolib-of falls back from "<con_id>-gpios" to
	 * "<con_id>-gpio", so both spellings bind. See dts-fragment.dtsi.
	 *
	 * Requested GPIOD_OUT_HIGH, i.e. logically asserted. With the DT's
	 * GPIO_ACTIVE_LOW that drives the pin physically low, which is the
	 * state the vendor driver asked for (GPIOF_OUT_INIT_LOW, whose legacy
	 * integer API also meant physical low). Holding the part in reset
	 * through probe keeps the speaker quiet until the ID check passes.
	 */
	aw->reset_gpiod = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(aw->reset_gpiod)) {
		ret = PTR_ERR(aw->reset_gpiod);
		aw->reset_gpiod = NULL;
		dev_warn(dev, "no reset gpio (%d), relying on always-on power\n",
			 ret);
	} else if (!aw->reset_gpiod) {
		dev_warn(dev, "no reset-gpios property, relying on always-on power\n");
	}

	aw87329_hw_reset(aw);

	{
		u8 id;

		ret = aw87329_read_chipid(aw, &id);
		if (ret) {
			dev_err(dev, "no AW87329 on i2c bus %d addr 0x%02x\n",
				i2c_adapter_id(client->adapter), client->addr);
			return ret;
		}
	}

	aw->probed = true;

	ret = devm_device_add_group(dev, &aw87329_attr_group);
	if (ret) {
		aw->probed = false;
		return dev_err_probe(dev, ret, "sysfs group failed\n");
	}

	/* Park the amp, as the vendor probe did (aw87329_audio.c:803). */
	aw87329_hw_off(aw);

	dev_info(dev, "aw87329 probed, version %s\n", AW87329_DRV_VERSION);

	return 0;
}

static void aw87329_remove(struct i2c_client *client)
{
	struct aw87329 *aw = dev_get_drvdata(&client->dev);

	/* Leave the speaker quiet before the i2c channel goes away. */
	aw87329_spk_disable(&client->dev);
	aw->probed = false;

	/* devm handles regmap, gpio, regulator, the sysfs group and aw itself. */
}

static const struct i2c_device_id aw87329_i2c_id[] = {
	{ "aw87329_pa", 0 },
	{ "aw87319_pa", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, aw87329_i2c_id);

static const struct of_device_id aw87329_of_match[] = {
	{ .compatible = "awinic,aw87329_pa" },
	/* Same family; accepted so a mislabelled DT still probes. */
	{ .compatible = "awinic,aw87319_pa" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw87329_of_match);

static struct i2c_driver aw87329_i2c_driver = {
	.driver = {
		.name		= AW87329_DRV_NAME,
		.of_match_table	= aw87329_of_match,
	},
	.probe		= aw87329_probe,
	.remove		= aw87329_remove,
	.id_table	= aw87329_i2c_id,
};

module_i2c_driver(aw87329_i2c_driver);

MODULE_AUTHOR("Nick Li <liweilei@awinic.com.cn>");
MODULE_DESCRIPTION("Awinic AW87329/AW87319 analog-input speaker PA driver");
MODULE_LICENSE("GPL-2.0");
MODULE_VERSION(AW87329_DRV_VERSION);

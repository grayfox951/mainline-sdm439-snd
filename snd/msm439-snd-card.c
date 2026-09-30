// SPDX-License-Identifier: GPL-2.0-only
/*
 * Speaker-amplifier supervisor for Xiaomi Redmi 7A (pine) / SDM439
 * ==============================================================
 * Kernel: msm89x7-mainline/linux @ msm89x7/7.1.3
 *
 * ------------------------------------------------------------------------
 * THIS IS NOT AN ASOC MACHINE DRIVER.  READ THIS FIRST.
 * ------------------------------------------------------------------------
 * The sound card machine driver is sound/soc/qcom/apq8016_sbc.c, used as-is
 * with compatible = "qcom,apq8016-sbc-sndcard".  No variant of it is
 * needed and none is written here.  Reasons, each verified against the fork
 * at msm89x7/7.1.3:
 *
 *  1. Its of_device_id already has the right entry and the driver dispatches
 *     on it with device_get_match_data():
 *          { .compatible = "qcom,apq8016-sbc-sndcard",
 *            .data = apq8016_sbc_add_ops },
 *          { .compatible = "qcom,msm8916-qdsp6-sndcard",
 *            .data = msm8916_qdsp6_add_ops },
 *     apq8016_sbc_add_ops() is only
 *          for_each_card_prelinks(card, i, link)
 *                  link->init = apq8016_sbc_dai_init;
 *     The QDSP6 half - msm8916_qdsp6_add_ops(), which sets
 *     card->components = "qdsp6" and pins every link to 48 kHz / 2ch /
 *     S16_LE for the QDSP6 AFE DAIs - is never selected.  Nothing AFE-
 *     related is reached at runtime, so no q6apm/q6afe reference matters.
 *
 *  2. It covers both DAI links this board needs.  apq8016_dai_init() switches
 *     on cpu_dai->id and handles MI2S_PRIMARY (writes
 *     SPKR_CTL_PRI_WS_SLAVE_SEL_11 into spkr-iomux) and MI2S_TERTIARY
 *     (writes MIC_CTRL_TER_WS_SLAVE_SEL | MIC_CTRL_TLMM_SCLK_EN into
 *     mic-iomux).  Those are the only two LPASS DAI ids pine uses.  The
 *     MI2S_QUINARY case exists but lpass-apq8016.c registers no DAI for it,
 *     so that branch is dead on this SoC anyway.
 *
 *  3. The iommux regions it needs are already at the pine addresses.
 *     apq8016_sbc_platform_probe() maps mic-iomux and spkr-iomux by name
 *     with devm_platform_ioremap_resource_byname() and fails probe if either
 *     is missing; msm8937.dtsi:2340 already supplies both.
 *
 *  4. It creates the jack itself, so the DT must NOT add jack widgets:
 *     snd_soc_card_jack_new_pins(card, "Headset Jack",
 *                     SND_JACK_HEADSET | SND_JACK_HEADPHONE |
 *                     SND_JACK_BTN_0 .. SND_JACK_BTN_4, ...)
 *     The card widgets it registers are "Headphone Jack", "Mic Jack",
 *     "Handset Mic", "Headset Mic", "Secondary Mic", "Digital Mic1" and
 *     "Digital Mic2".
 *
 *  5. No dai-format and no set_fmt are needed.  qcom_snd_parse_of() never
 *     parses one, and neither lpass-cpu nor msm8916-wcd-analog nor
 *     msm8916-wcd-digital declares master_capable / slave_capable or a
 *     set_fmt op.  lpass hard-codes WSSRC_INTERNAL in
 *     lpass_cpu_daiops_hw_params() and programs LPAIF_I2SCTL_* itself, so
 *     LPASS is always the bit-clock and word-select source.  Every working
 *     msm8916 board in this kernel runs with dai_fmt == 0.
 *
 *  6. iommux handling is therefore identical to the reference board: the
 *     driver writes those four registers directly, so the reg list must be
 *     left exactly as msm8937.dtsi:2340 defines it.
 *
 * ------------------------------------------------------------------------
 * WHY THIS FILE EXISTS THEN
 * ------------------------------------------------------------------------
 * One gap has no in-tree home: the speaker amplifier.
 *
 * The AW87329 is an ANALOG-INPUT class-K PA on blsp1_i2c2 at 0x58, ported as
 * its own out-of-tree module (aw87329.c).  It carries no I2S, so it is
 * correctly NOT a codec and NOT a DAI, and it is correctly not modelled as
 * one here.  But it does have to be switched in step with the WCD codec's
 * speaker path, and the only in-tree hook that fires at exactly the right
 * moment is pm8916_wcd_analog_enable_spk_pa() in
 * sound/soc/codecs/msm8916-wcd-analog.c - a file an out-of-tree module
 * cannot patch.
 *
 * This module supplies the missing half of that coupling without touching
 * the codec, and without duplicating anything:
 *
 *   - it exports msm439_spk_amp_enable() / msm439_spk_amp_disable() so that
 *     a future in-tree patch to pm8916_wcd_analog_enable_spk_pa() can call
 *     them from SND_SOC_DAPM_POST_PMU / SND_SOC_DAPM_POST_PMD and get the
 *     ordering right for free;
 *
 *   - until that patch exists it exposes the same two operations through
 *     sysfs, so the amplifier can be sequenced by hand around
 *     aplay / arecord (see snd/README.md).
 *
 * The AW87329 register protocol lives only in aw87329.c.  This file locates
 * the amplifier and switches it on or off.
 *
 * ------------------------------------------------------------------------
 * HOW THE AMPLIFIER IS REACHED
 * ------------------------------------------------------------------------
 * The amplifier is found through a DT phandle, not by scanning a bus:
 *
 *   &msm439_snd_card { compatible = "qcom,msm439-snd-card";
 *                      amp-phandle = <&aw87329>; };
 *
 * resolved with of_property_read_phandle() + of_find_device_by_phandle().
 * The node itself belongs to aw87329.c and is never claimed here.
 *
 * Switching it on or off calls aw87329_spk_enable() / aw87329_spk_disable(),
 * looked up with __symbol_get().  That creates no link-time dependency, so
 * this module builds, loads and unloads with or without aw87329.ko present.
 * If the symbols are absent the amp is simply left alone and the sysfs
 * attribute returns -ENODEV with a message pointing at the amplifier's own
 * sysfs switch, which is what aw87329/README.md documents.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/device.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

/* compatible of the out-of-tree AW87329 node, from aw87329/dts-fragment.dtsi */
#define AMP_AMP_COMPAT		"awinic,aw87329_pa"

/* symbols exported by aw87329.c, resolved with __symbol_get() */
#define AMP_AMP_ENABLE_SYM	"aw87329_spk_enable"
#define AMP_AMP_DISABLE_SYM	"aw87329_spk_disable"

/* signature of the two exported aw87329 entry points */
typedef int (*amp_op_t)(struct device *dev);

struct msm439_snd_card {
	struct device	*dev;		/* our own platform device */
	struct mutex	lock;
	struct device	*amp;		/* borrowed, not owned */
	amp_op_t	enable;
	amp_op_t	disable;
	bool		have_syms;
};

/* the single instance; the codec hook has no other way to reach us */
static struct msm439_snd_card *card_priv;

/*
 * Resolve the amplifier device and the driver entry points.
 *
 * Called from probe and again from the sysfs store handler, because the
 * amplifier may probe after this module does.  Idempotent and lock-free: the
 * caller holds ->lock.
 */
static int amp_resolve(struct msm439_snd_card *priv, struct device_node *np)
{
	struct device_node *amp_np;
	struct device *amp_dev;
	amp_op_t enable, disable;

	if (!priv->have_syms) {
		enable  = (amp_op_t)__symbol_get(AMP_AMP_ENABLE_SYM);
		disable = (amp_op_t)__symbol_get(AMP_AMP_DISABLE_SYM);

		if (!!enable != !!disable) {
			/* only one of them exported: refuse both */
			dev_warn(priv->dev,
				 "only one of %s/%s exported, refusing both\n",
				 AMP_AMP_ENABLE_SYM, AMP_AMP_DISABLE_SYM);
			enable = disable = NULL;
		}

		if (enable) {
			priv->enable  = enable;
			priv->disable = disable;
			priv->have_syms = true;
		}
	}

	if (priv->amp || !np)
		return priv->amp ? 0 : -ENODEV;

	if (!priv->have_syms)
		return -EPROBE_DEFER;

	if (of_property_read_phandle(np, "amp-phandle", &amp_np))
		return -EINVAL;

	amp_dev = of_find_device_by_phandle(amp_np);
	if (!amp_dev)
		return -EPROBE_DEFER;	/* amplifier has not probed yet */

	/* the lookup hands back a borrowed reference; pin it explicitly so
	 * the device cannot go away underneath us between probe and remove.
	 * get_device() does not pin aw87329.ko, so unloading that module is
	 * still allowed - it just leaves a dead amplifier behind. */
	get_device(amp_dev);
	priv->amp = amp_dev;
	return 0;
}

/* caller must hold ->lock */
static int amp_apply(struct msm439_snd_card *priv, int enable)
{
	amp_op_t op;
	int ret;

	if (!priv->amp) {
		dev_warn_ratelimited(priv->dev,
			"no %s device present; speaker amplifier left unchanged\n",
			AMP_AMP_COMPAT);
		return -ENODEV;
	}

	op = enable ? priv->enable : priv->disable;
	if (!op)
		return -ENODEV;

	ret = op(&priv->amp->dev);

	dev_dbg(priv->dev, "amplifier %s -> %d\n", enable ? "on" : "off", ret);
	return ret;
}

/* ------------------------------------------------------------------------ *
 * Exported API, for an in-tree hook in
 * sound/soc/codecs/msm8916-wcd-analog.c:pm8916_wcd_analog_enable_spk_pa()
 *
 *   case SND_SOC_DAPM_POST_PMU:   ...msm439_spk_amp_enable(dev);   break;
 *   case SND_SOC_DAPM_POST_PMD:   ...msm439_spk_amp_disable(dev);  break;
 *
 * @dev is the PM8953 WCD analog codec device.  It is accepted so the call
 * site can pass its own device naturally, and is not used for the lookup:
 * the amplifier is resolved once, at probe.
 * ------------------------------------------------------------------------ */

/**
 * msm439_spk_amp_enable() - switch the AW87329 speaker amplifier on
 * @dev: the PM8953 WCD analog codec device, or %NULL
 *
 * Return: 0 on success, negative errno otherwise.  -ENODEV means the
 * amplifier is not present, which is not an error on a board variant that
 * drives its speaker some other way.
 */
int msm439_spk_amp_enable(struct device *dev)
{
	struct msm439_snd_card *priv = card_priv;
	int ret;

	if (!priv)
		return -ENODEV;

	mutex_lock(&priv->lock);
	ret = amp_apply(priv, 1);
	mutex_unlock(&priv->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(msm439_spk_amp_enable);

/**
 * msm439_spk_amp_disable() - switch the AW87329 speaker amplifier off
 * @dev: the PM8953 WCD analog codec device, or %NULL
 *
 * Return: 0 on success, negative errno otherwise.
 */
int msm439_spk_amp_disable(struct device *dev)
{
	struct msm439_snd_card *priv = card_priv;
	int ret;

	if (!priv)
		return -ENODEV;

	mutex_lock(&priv->lock);
	ret = amp_apply(priv, 0);
	mutex_unlock(&priv->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(msm439_spk_amp_disable);

/* ------------------------------------------------------------------------ *
 * sysfs: /sys/devices/platform/...msm439-snd-card.../spk_amp_enable
 *
 * Write "1" to enable, "0" to disable, exactly as a userspace hook would.
 * Deliberately not named "enable": a card-level switch called "enable" is
 * too easy to mistake for the amplifier's own attribute.
 * ------------------------------------------------------------------------ */
static ssize_t spk_amp_enable_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct msm439_snd_card *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", priv->amp ? 1 : 0);
}
static DEVICE_ATTR_RO(spk_amp_enable);

static ssize_t spk_amp_enable_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct msm439_snd_card *priv = dev_get_drvdata(dev);
	int value;
	int ret;

	if (kstrtoint(buf, 0, &value))
		return -EINVAL;

	/* the amplifier may have probed since the last attempt */
	mutex_lock(&priv->lock);
	amp_resolve(priv, dev->of_node);
	ret = value ? amp_apply(priv, 1) : amp_apply(priv, 0);
	mutex_unlock(&priv->lock);

	if (ret)
		return ret;

	return count;
}
static DEVICE_ATTR_WO(spk_amp_enable);

static struct attribute *msm439_snd_card_attrs[] = {
	&dev_attr_spk_amp_enable.attr,
	NULL,
};
ATTRIBUTE_GROUPS(msm439_snd_card);

/*
 * A missing amplifier is not a probe failure: the WCD speaker path must still
 * come up on a board variant with no external PA.  So this only ever fails
 * for real reasons (allocation, sysfs).
 */
static int msm439_snd_card_probe(struct platform_device *pdev)
{
	struct msm439_snd_card *priv;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	mutex_init(&priv->lock);
	priv->dev = &pdev->dev;
	dev_set_drvdata(&pdev->dev, priv);
	platform_set_drvdata(pdev, priv);

	switch (amp_resolve(priv, pdev->dev.of_node)) {
	case 0:
		dev_info(&pdev->dev, "%s found on %s, driving via exported symbols\n",
			 AMP_AMP_COMPAT, dev_name(&priv->amp->dev));
		break;
	case -EPROBE_DEFER:
		dev_info(&pdev->dev, "%s or its driver not ready yet\n",
			 AMP_AMP_COMPAT);
		break;
	default:
		dev_info(&pdev->dev,
			 "no %s node, speaker amplifier control unavailable\n",
			 AMP_AMP_COMPAT);
		break;
	}

	ret = device_create_group(&pdev->dev, &msm439_snd_card_attr_group);
	if (ret)
		return ret;

	card_priv = priv;

	return 0;
}

static int msm439_snd_card_remove(struct platform_device *pdev)
{
	struct msm439_snd_card *priv = platform_get_drvdata(pdev);

	mutex_lock(&priv->lock);

	/* never leave the speaker amplifier latched on */
	amp_apply(priv, 0);

	card_priv = NULL;

	if (priv->amp) {
		put_device(priv->amp);	/* balanced with get_device() */
		priv->amp = NULL;
	}
	priv->have_syms = false;
	priv->enable = NULL;
	priv->disable = NULL;

	mutex_unlock(&priv->lock);

	return 0;
}

static const struct of_device_id msm439_snd_card_of_match[] = {
	{ .compatible = "qcom,msm439-snd-card" },
	{ }
};
MODULE_DEVICE_TABLE(of, msm439_snd_card_of_match);

static struct platform_driver msm439_snd_card_driver = {
	.probe	= msm439_snd_card_probe,
	.remove	= msm439_snd_card_remove,
	.driver = {
		.name		= "msm439-snd-card",
		.of_match_table = of_match_ptr(msm439_snd_card_of_match),
	},
};
module_platform_driver(msm439_snd_card_driver);

MODULE_DESCRIPTION("Speaker amplifier supervisor for Xiaomi Redmi 7A (pine)");
MODULE_LICENSE("GPL");
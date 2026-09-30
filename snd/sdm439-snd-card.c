// SPDX-License-Identifier: GPL-2.0
/*
 * Edited by: grayfox951 <admin@dnr.qzz.io>
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

struct sdm439_snd_card {
	struct device	*dev;		/* our own platform device */
	struct mutex	lock;
	struct device	*amp;		/* borrowed, not owned */
	amp_op_t	enable;
	amp_op_t	disable;
	bool		have_syms;
};

/* the single instance; the codec hook has no other way to reach us */
static struct sdm439_snd_card *card_priv;

/*
 * Resolve the amplifier device and the driver entry points.
 *
 * Called from probe and again from the sysfs store handler, because the
 * amplifier may probe after this module does.  Idempotent and lock-free: the
 * caller holds ->lock.
 */
static int amp_resolve(struct sdm439_snd_card *priv, struct device_node *np)
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
static int amp_apply(struct sdm439_snd_card *priv, int enable)
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
 *   case SND_SOC_DAPM_POST_PMU:   ...sdm439_spk_amp_enable(dev);   break;
 *   case SND_SOC_DAPM_POST_PMD:   ...sdm439_spk_amp_disable(dev);  break;
 *
 * @dev is the PM8953 WCD analog codec device.  It is accepted so the call
 * site can pass its own device naturally, and is not used for the lookup:
 * the amplifier is resolved once, at probe.
 * ------------------------------------------------------------------------ */

/**
 * sdm439_spk_amp_enable() - switch the AW87329 speaker amplifier on
 * @dev: the PM8953 WCD analog codec device, or %NULL
 *
 * Return: 0 on success, negative errno otherwise.  -ENODEV means the
 * amplifier is not present, which is not an error on a board variant that
 * drives its speaker some other way.
 */
int sdm439_spk_amp_enable(struct device *dev)
{
	struct sdm439_snd_card *priv = card_priv;
	int ret;

	if (!priv)
		return -ENODEV;

	mutex_lock(&priv->lock);
	ret = amp_apply(priv, 1);
	mutex_unlock(&priv->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(sdm439_spk_amp_enable);

/**
 * sdm439_spk_amp_disable() - switch the AW87329 speaker amplifier off
 * @dev: the PM8953 WCD analog codec device, or %NULL
 *
 * Return: 0 on success, negative errno otherwise.
 */
int sdm439_spk_amp_disable(struct device *dev)
{
	struct sdm439_snd_card *priv = card_priv;
	int ret;

	if (!priv)
		return -ENODEV;

	mutex_lock(&priv->lock);
	ret = amp_apply(priv, 0);
	mutex_unlock(&priv->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(sdm439_spk_amp_disable);

/* ------------------------------------------------------------------------ *
 * sysfs: /sys/devices/platform/...sdm439-snd-card.../spk_amp_enable
 *
 * Write "1" to enable, "0" to disable, exactly as a userspace hook would.
 * Deliberately not named "enable": a card-level switch called "enable" is
 * too easy to mistake for the amplifier's own attribute.
 * ------------------------------------------------------------------------ */
static ssize_t spk_amp_enable_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct sdm439_snd_card *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", priv->amp ? 1 : 0);
}
static DEVICE_ATTR_RO(spk_amp_enable);

static ssize_t spk_amp_enable_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct sdm439_snd_card *priv = dev_get_drvdata(dev);
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

static struct attribute *sdm439_snd_card_attrs[] = {
	&dev_attr_spk_amp_enable.attr,
	NULL,
};
ATTRIBUTE_GROUPS(sdm439_snd_card);

/*
 * A missing amplifier is not a probe failure: the WCD speaker path must still
 * come up on a board variant with no external PA.  So this only ever fails
 * for real reasons (allocation, sysfs).
 */
static int sdm439_snd_card_probe(struct platform_device *pdev)
{
	struct sdm439_snd_card *priv;
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

	ret = device_create_group(&pdev->dev, &sdm439_snd_card_attr_group);
	if (ret)
		return ret;

	card_priv = priv;

	return 0;
}

static int sdm439_snd_card_remove(struct platform_device *pdev)
{
	struct sdm439_snd_card *priv = platform_get_drvdata(pdev);

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

static const struct of_device_id sdm439_snd_card_of_match[] = {
	{ .compatible = "qcom,sdm439-snd-card" },
	{ }
};
MODULE_DEVICE_TABLE(of, sdm439_snd_card_of_match);

static struct platform_driver sdm439_snd_card_driver = {
	.probe	= sdm439_snd_card_probe,
	.remove	= sdm439_snd_card_remove,
	.driver = {
		.name		= "sdm439-snd-card",
		.of_match_table = of_match_ptr(sdm439_snd_card_of_match),
	},
};
module_platform_driver(sdm439_snd_card_driver);

MODULE_DESCRIPTION("Speaker amplifier supervisor for Xiaomi Redmi 7A (pine)");
MODULE_LICENSE("GPL");
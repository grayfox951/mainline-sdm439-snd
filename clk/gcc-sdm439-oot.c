// SPDX-License-Identifier: GPL-2.0
/*
 * Edited by: grayfox951 <admin@dnr.qzz.io>
 */

#include <dt-bindings/clock/qcom,gcc-msm8916.h>
#include <linux/bitops.h>
#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/device/devres.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define AON_AUDIO_MAX_HW_CLKS	6	/* distinct CBCRs */
#define AON_AUDIO_MAX_DT_CLKS	7	/* DT clock IDs */
#define AON_AUDIO_REF_HZ_DEFAULT	133330000UL

/* indices into the hardware gate array */
enum aon_hw_idx {
	AON_HW_AHBFABRIC_IXFABRIC,
	AON_HW_LPAIF_PRI_I2S,
	AON_HW_LPAIF_SEC_I2S,
	AON_HW_LPAIF_AUX_I2S,
	AON_HW_PCNOC_MPORT,
	AON_HW_PCNOC_SWAY,
};

/*
 * One CBCR gate.
 *
 * .reg/.mask are byte offsets from the GCC base, copied verbatim from
 * drivers/clk/qcom/gcc-msm8916.c:
 *     gcc_ultaudio_pcnoc_mport_clk        halt_reg = enable_reg = 0x1c000
 *     gcc_ultaudio_pcnoc_sway_clk         halt_reg = enable_reg = 0x1c004
 *     gcc_ultaudio_ahbfabric_ixfabric_clk halt_reg = enable_reg = 0x1c028
 *     gcc_ultaudio_lpaif_pri_i2s_clk      halt_reg = enable_reg = 0x1c068
 *     gcc_ultaudio_lpaif_sec_i2s_clk      halt_reg = enable_reg = 0x1c080
 *     gcc_ultaudio_lpaif_aux_i2s_clk      halt_reg = enable_reg = 0x1c098
 * All six use enable_mask = BIT(0).
 */
struct aon_audio_hw {
	u32	reg;
	u32	mask;
	const char *name;
};

static const struct aon_audio_hw gcc_aon_audio_hw[AON_HW_PCNOC_SWAY + 1] = {
	[AON_HW_AHBFABRIC_IXFABRIC]	= { 0x1c028, BIT(0), "gcc_ultaudio_ahbfabric_ixfabric_clk" },
	[AON_HW_LPAIF_PRI_I2S]		= { 0x1c068, BIT(0), "gcc_ultaudio_lpaif_pri_i2s_clk" },
	[AON_HW_LPAIF_SEC_I2S]		= { 0x1c080, BIT(0), "gcc_ultaudio_lpaif_sec_i2s_clk" },
	[AON_HW_LPAIF_AUX_I2S]		= { 0x1c098, BIT(0), "gcc_ultaudio_lpaif_aux_i2s_clk" },
	[AON_HW_PCNOC_MPORT]		= { 0x1c000, BIT(0), "gcc_ultaudio_pcnoc_mport_clk" },
	[AON_HW_PCNOC_SWAY]		= { 0x1c004, BIT(0), "gcc_ultaudio_pcnoc_sway_clk" },
};

/*
 * DT clock ID -> hardware gate. POSITIONAL, and in exactly the order the
 * lpass node lists its "clocks" phandles. Do not reorder without editing
 * dts-fragment.dtsi to match.
 *
 * The IDs are the values from include/dt-bindings/clock/qcom,gcc-msm8916.h.
 * They are safe to use verbatim HERE ONLY, because this provider owns its
 * own DT node and therefore its own ID space. They must NOT be pasted into
 * qcom,gcc-sdm439.h at these values: qcom,gcc-msm8917.h already assigns
 * 150/151/154/156/157/158 to JPEG0_CLK_SRC, MCLK0_CLK_SRC, MDP_CLK_SRC,
 * PDM2_CLK_SRC, SDCC1_APPS_CLK_SRC and SDCC1_ICE_CORE_CLK_SRC respectively.
 * The in-tree copy of this provider therefore renumbers from 183 upwards -
 * see fk/include/dt-bindings/clock/qcom,gcc-sdm439.h.
 *
 * AON_HW_LPAIF_PRI_I2S appears twice on purpose: mainline msm8916.dtsi points
 * both "mi2s-bit-clk0" and "mi2s-bit-clk1" at
 * GCC_ULTAUDIO_LPAIF_PRI_I2S_CLK. Returning the same struct clk_hw for both
 * IDs means the clk core refcounts one clock, not two, which is what
 * upstream does.
 */
struct aon_audio_id {
	u32	id;
	u8	hw;
};

static const struct aon_audio_id gcc_aon_audio_ids[AON_AUDIO_MAX_DT_CLKS] = {
	{ GCC_ULTAUDIO_AHBFABRIC_IXFABRIC_CLK,	AON_HW_AHBFABRIC_IXFABRIC },
	{ GCC_ULTAUDIO_LPAIF_PRI_I2S_CLK,		AON_HW_LPAIF_PRI_I2S },
	{ GCC_ULTAUDIO_LPAIF_PRI_I2S_CLK,		AON_HW_LPAIF_PRI_I2S },
	{ GCC_ULTAUDIO_LPAIF_SEC_I2S_CLK,		AON_HW_LPAIF_SEC_I2S },
	{ GCC_ULTAUDIO_LPAIF_AUX_I2S_CLK,		AON_HW_LPAIF_AUX_I2S },
	{ GCC_ULTAUDIO_PCNOC_MPORT_CLK,		AON_HW_PCNOC_MPORT },
	{ GCC_ULTAUDIO_PCNOC_SWAY_CLK,		AON_HW_PCNOC_SWAY },
};

struct aon_audio {
	struct regmap		*map;
	unsigned long		ref_hz;
	struct clk_hw		ref;
	const struct clk_hw	*ref_parent[1];
	struct clk_hw		*hw[AON_HW_PCNOC_SWAY + 1];
	struct clk_hw		*id_hw[AON_AUDIO_MAX_DT_CLKS];

	/*
	 * struct clk_init_data lives here rather than on the probe() stack, so
	 * that nothing registered points at a stack slot once probe() returns.
	 * (The clk core copies what it needs out of init during registration,
	 * but there is no reason to depend on that.)
	 */
	struct clk_init_data	ref_init;
	struct clk_init_data	gate_init[AON_HW_PCNOC_SWAY + 1];
};

struct aon_audio_gate {
	struct clk_hw		hw;
	struct aon_audio	*aon;
	u32			reg;
	u32			mask;
};

static inline struct aon_audio *to_aon_audio(struct clk_hw *hw)
{
	return container_of(hw, struct aon_audio, ref);
}

static inline struct aon_audio_gate *to_aon_audio_gate(struct clk_hw *hw)
{
	return container_of(hw, struct aon_audio_gate, hw);
}

/*
 * Reference parent.
 *
 * It exists for two reasons, both about clk_set_rate() returning -EINVAL
 * unless somebody in the chain implements .set_rate:
 *
 *  1. clk_core_set_rate_nolock() takes the "!core->parent" shortcut first. A
 *     parentless clock whose hw->ops has no .set_rate returns -EINVAL from
 *     clk_set_rate(). A DT "fixed-clock" stub is exactly such a clock, so a
 *     fixed-clock stub makes clk_set_rate() fail - and both LPASS drivers
 *     treat that as fatal:
 *         apq8016.c:194   clk_set_rate(ahbix_clk, LPASS_AHBIX_CLOCK_FREQUENCY)
 *                         -> goto err_ahbix_clk -> probe fails
 *         lpass-cpu.c:288 clk_set_rate(mi2s_bit_clk[id], rate * bitwidth * 2)
 *                         -> return ret -> DAI hw_params fails
 *     We are the fixed-rate reference, and we DO provide .set_rate, so that
 *     shortcut answers 0 instead of -EINVAL.
 *
 *  2. With a parent present, clk_core_set_rate_nolock() recurses into
 *     clk_core_set_rate_nolock(ref, rate), which hits the "!ref->parent"
 *     shortcut again. Because ref has a .set_rate op that returns 0, that
 *     returns 0, and the leaf is then handed clk_hw_set_rate(), which also
 *     returns 0. So clk_set_rate() on any of the seven clocks returns 0
 *     regardless of how the core orders those checks.
 *
 * The rate we report is a board constant (133.33 MHz, the msm8916
 * GCC_XO_GPLL0_GPLL1_SLEEP frequency), overridable from DT with
 * qcom,aon-audio-hz. It is never written to hardware.
 */
static unsigned long aon_audio_ref_recalc_rate(struct clk_hw *hw,
					      unsigned long rate)
{
	return to_aon_audio(hw)->ref_hz;
}

static int aon_audio_ref_set_rate(struct clk_hw *hw, unsigned long rate,
				  unsigned long parent_rate)
{
	return 0;
}

static const struct clk_ops aon_audio_ref_ops = {
	.recalc_rate	= aon_audio_ref_recalc_rate,
	.set_rate	= aon_audio_ref_set_rate,
};

static int aon_audio_enable(struct clk_hw *hw)
{
	struct aon_audio_gate *g = to_aon_audio_gate(hw);
	unsigned int val;
	int ret;

	ret = regmap_update_bits(g->aon->map, g->reg, g->mask, g->mask);
	if (ret)
		return ret;

	/*
	 * Read back. If this CBCR is not the register we think it is (wrong
	 * base, wrong offset, bus fault) we must fail loudly here rather than
	 * let the LPASS drivers program a controller whose bit clock never
	 * runs.
	 */
	ret = regmap_read(g->aon->map, g->reg, &val);
	if (ret)
		return ret;

	return (val & g->mask) ? 0 : -EIO;
}

static void aon_audio_disable(struct clk_hw *hw)
{
	struct aon_audio_gate *g = to_aon_audio_gate(hw);

	regmap_update_bits(g->aon->map, g->reg, g->mask, 0);
}

static int aon_audio_is_enabled(struct clk_hw *hw)
{
	struct aon_audio_gate *g = to_aon_audio_gate(hw);
	unsigned int val;

	if (regmap_read(g->aon->map, g->reg, &val))
		return 0;

	return !!(val & g->mask);
}

static unsigned long aon_audio_recalc_rate(struct clk_hw *hw,
					   unsigned long parent_rate)
{
	return parent_rate;
}

static int aon_audio_set_rate(struct clk_hw *hw, unsigned long rate,
			      unsigned long parent_rate)
{
	/*
	 * The bit rate itself lives in the LPAIF CMD_RCGR, whose parents are
	 * not reconstructible in this tree (no GCC_GPLL1, no GCC_XO_GPLL0_BIMC
	 * in gcc-msm8917.c). Report success; recalc_rate() is what reports the
	 * rate, and it reports the reference rate, not a programmed one.
	 */
	return 0;
}

static const struct clk_ops aon_audio_gate_ops = {
	.enable		= aon_audio_enable,
	.disable	= aon_audio_disable,
	.is_enabled	= aon_audio_is_enabled,
	.recalc_rate	= aon_audio_recalc_rate,
	.set_rate	= aon_audio_set_rate,
};

static struct clk_hw *gcc_sdm439_aon_audio_hw_get(struct of_phandle_args *args,
						  void *data)
{
	struct aon_audio *aon = data;
	unsigned int i;

	if (!args || args->args_count < 1)
		return ERR_PTR(-EINVAL);

	for (i = 0; i < AON_AUDIO_MAX_DT_CLKS; i++) {
		if (gcc_aon_audio_ids[i].id != args->args[0])
			continue;
		if (!aon->id_hw[i])
			return ERR_PTR(-EINVAL);
		return aon->id_hw[i];
	}

	return ERR_PTR(-EINVAL);
}

static const struct regmap_config gcc_sdm439_aon_audio_regmap_config = {
	.name			= "gcc-sdm439-aon-audio",
	.reg_stride		= 4,
	.max_register		= 0x1c098,
	.val_format_endian	= REGMAP_ENDIAN_DEFAULT,
};

static int gcc_sdm439_aon_audio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct aon_audio *aon;
	struct aon_audio_gate *g;
	struct device_node *gcc_np;
	void __iomem *regs;
	u32 hz;
	unsigned int i;
	int ret;

	if (!dev->of_node)
		return -EINVAL;

	/*
	 * The GCC node, by phandle. Our own DT node deliberately has no "reg",
	 * so we claim no address space of our own.
	 */
	gcc_np = of_parse_phandle(dev->of_node, "qcom,gcc", 0);
	if (!gcc_np) {
		dev_err(dev, "no 'qcom,gcc' phandle\n");
		return -EINVAL;
	}

	aon = devm_kzalloc(dev, sizeof(*aon), GFP_KERNEL);
	if (!aon) {
		ret = -ENOMEM;
		goto err_put_node;
	}

	aon->ref_hz = AON_AUDIO_REF_HZ_DEFAULT;
	hz = 0;
	if (!of_property_read_u32(dev->of_node, "qcom,aon-audio-hz", &hz) && hz)
		aon->ref_hz = hz;

	/*
	 * NON-EXCLUSIVE mapping of the GCC window.
	 *
	 * devm_of_iomap() -> of_iomap() + devm_ioremap(). devm_ioremap() is a
	 * plain ioremap() with a devres free hook; it does NOT call
	 * request_mem_region(). devm_platform_ioremap_resource() and
	 * devm_ioremap_resource() would return -EBUSY here, because
	 * drivers/clk/qcom/gcc-msm8917.c has already claimed 0x01800000.
	 * devm_platform_ioremap_resource_bindmap(), which would have solved
	 * that, does not exist in this kernel - grep over fk/include/ returns
	 * zero hits for "ioremap_resource_bindmap".
	 */
	regs = devm_of_iomap(dev, gcc_np, 0, NULL);
	if (IS_ERR(regs)) {
		ret = PTR_ERR(regs);
		goto err_put_node;
	}

	aon->map = devm_regmap_init_mmio(dev, regs,
					 &gcc_sdm439_aon_audio_regmap_config);
	if (IS_ERR(aon->map)) {
		ret = PTR_ERR(aon->map);
		goto err_put_node;
	}

	/*
	 * Reference clock, parent of all six gates.
	 *
	 * It is deliberately PARENTLESS. It must not be its own parent: the clk
	 * core resolves core->parent = init->parent_hws[0], so pointing that at
	 * &aon->ref would make clk_core_prepare()/clk_core_enable() recurse into
	 * themselves.
	 */
	aon->ref_parent[0]	= &aon->ref;
	aon->ref_init.name	= "gcc_sdm439_aon_audio_ref";
	aon->ref_init.ops	= &aon_audio_ref_ops;
	aon->ref_init.parent_hws	= NULL;
	aon->ref_init.num_parents	= 0;
	aon->ref.init		= &aon->ref_init;

	ret = devm_clk_hw_register(dev, &aon->ref);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register reference clock\n");

	/* The six CBCR gates. */
	for (i = 0; i < ARRAY_SIZE(gcc_aon_audio_hw); i++) {
		g = devm_kzalloc(dev, sizeof(*g), GFP_KERNEL);
		if (!g)
			return -ENOMEM;

		g->aon	= aon;
		g->reg	= gcc_aon_audio_hw[i].reg;
		g->mask	= gcc_aon_audio_hw[i].mask;

		aon->gate_init[i].name		= gcc_aon_audio_hw[i].name;
		aon->gate_init[i].parent_hws	= aon->ref_parent;
		aon->gate_init[i].num_parents	= 1;
		aon->gate_init[i].flags		= CLK_SET_RATE_PARENT;
		aon->gate_init[i].ops		= &aon_audio_gate_ops;
		g->hw.init = &aon->gate_init[i];

		ret = devm_clk_hw_register(dev, &g->hw);
		if (ret)
			return dev_err_probe(dev, ret, "failed to register %s\n",
					     gcc_aon_audio_hw[i].name);

		aon->hw[i] = &g->hw;
	}

	/* Map DT clock IDs onto hardware gates. */
	for (i = 0; i < AON_AUDIO_MAX_DT_CLKS; i++)
		aon->id_hw[i] = aon->hw[gcc_aon_audio_ids[i].hw];

	/*
	 * Prove the register window is live before advertising anything. Every
	 * one of these must read back non-zero; 0x181c004 is known to return
	 * 0x20008001 on this board.
	 */
	for (i = 0; i < ARRAY_SIZE(gcc_aon_audio_hw); i++) {
		unsigned int val = 0;

		ret = regmap_read(aon->map, gcc_aon_audio_hw[i].reg, &val);
		if (ret)
			return dev_err_probe(dev, ret, "%s: read 0x%x failed\n",
					     gcc_aon_audio_hw[i].name,
					     gcc_aon_audio_hw[i].reg);
		dev_info(dev, "aon-audio %-36s @0x%05x = 0x%08x%s\n",
			 gcc_aon_audio_hw[i].name,
			 gcc_aon_audio_hw[i].reg, val,
			 (val & gcc_aon_audio_hw[i].mask) ? " [on]" : " [off]");
		if (!val)
			return -ENODEV;
	}

	/*
	 * Registered last, and only once everything above has succeeded, so
	 * that lpass-cpu / apq8016 never see a provider that half works.
	 * really_probe() calls driver_deferred_probe_trigger() when this
	 * returns 0, which replays every driver parked on -EPROBE_DEFER - that
	 * is what unblocks lpass-cpu.
	 */
	ret = devm_of_clk_add_hw_provider(dev, gcc_sdm439_aon_audio_hw_get, aon);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add clock provider\n");

	of_node_put(gcc_np);
	return 0;

err_put_node:
	of_node_put(gcc_np);
	return ret;
}

static const struct of_device_id gcc_sdm439_aon_audio_match[] = {
	{ .compatible = "qcom,gcc-sdm439-aon-audio-clocks" },
	{ }
};
MODULE_DEVICE_TABLE(of, gcc_sdm439_aon_audio_match);

static struct platform_driver gcc_sdm439_aon_audio_driver = {
	.probe	= gcc_sdm439_aon_audio_probe,
	.driver	= {
		.name		= "gcc-sdm439-aon-audio-clocks",
		.of_match_table = of_match_ptr(gcc_sdm439_aon_audio_match),
	},
};
module_platform_driver(gcc_sdm439_aon_audio_driver);

MODULE_DESCRIPTION("Qualcomm SDM439 AON audio / LPASS clock provider");
MODULE_LICENSE("GPL");
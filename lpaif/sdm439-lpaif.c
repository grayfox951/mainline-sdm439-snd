// SPDX-License-Identifier: GPL-2.0
/*
 * Edited by: grayfox951 <admin@dnr.qzz.io>
 */

/*
 * sdm439-lpaif.c -- out-of-tree CPU-side PCM DAI for the SDM439 (Redmi 7A /
 * "pine") Low Power Audio Interface (LPAIF), built against
 * msm89x7-mainline/linux @ msm89x7/7.1.3, arm64.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS EXISTS AT ALL
 * ---------------------------------------------------------------------------
 * The in-tree pair that would normally do this job is
 *
 *	sound/soc/qcom/lpass-cpu.c	(the generic LPAIF CPU DAI)
 *	sound/soc/qcom/lpass-apq8016.c	(the apq8016/msm8916 variant + DAIs)
 *	sound/soc/qcom/lpass-platform.c	(the ALSA<->LPAIF DMA bridge)
 *
 * All three were read for this work at ref msm89x7/7.1.3 of
 * msm89x7-mainline/linux, together with sound/soc/qcom/apq8016_sbc.c,
 * sound/soc/qcom/common.c, sound/soc/qcom/lpass.h and
 * sound/soc/qcom/lpass-lpaif-reg.h. What this file does is a re-expression of
 * the same register programming in one self-contained translation unit. It is
 * not a copy: the reason it exists is that the LPAIF base address on SDM439
 * appears in no device tree in any tree, so in-tree code cannot be fixed
 * without rebuilding the kernel, whereas a DT property can be edited and a
 * new DTB flashed in seconds.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS DRIVER DOES NOT DO
 * ---------------------------------------------------------------------------
 *  - No AFE. sound/soc/msm is 404 in this fork AND in upstream v7.1, and
 *    q6apm does not exist either. Nothing here references either.
 *  - No qcom,adsp.
 *  - No set_fmt(), no set_sysclk(), no master_capable/slave_capable. See
 *    MASTER/SLAVE below.
 *  - No child "pcm" platform node of its own. See COMPONENT LAYOUT below.
 *
 * ---------------------------------------------------------------------------
 * COMPONENT LAYOUT: two components, one device
 * ---------------------------------------------------------------------------
 * The machine driver this card uses is sound/soc/qcom/apq8016_sbc.c, whose
 * probe calls qcom_snd_parse_of() from sound/soc/qcom/common.c. For a link with
 * a "cpu" child and no "platform" child that function does:
 *
 *	ret = snd_soc_of_get_dlc(cpu, &args, link->cpus, 0);
 *	link->id = args.args[0];
 *	...
 *	link->platforms->of_node = link->cpus->of_node;   /-* the &lpass node *-/
 *
 * so link->platforms and link->cpus resolve to the SAME DT node. The ASoC core
 * then looks for a platform component on the device bound to &lpass. In-tree
 * lpass registers both components on the same &pdev->dev, in this order:
 *
 *	1. devm_snd_soc_register_component(dev, &lpass_cpu_comp_driver,
 *					   variant->dai_driver, variant->num_dai)
 *	2. asoc_qcom_lpass_platform_register(pdev), which ends in
 *	   devm_snd_soc_register_component(&pdev->dev,
 *					   &lpass_component_driver, NULL, 0)
 *
 * (lpass-cpu.c:1263 and lpass-platform.c:1390 at 7.1.3.) That arrangement is
 * reproduced here byte-for-byte in structure, because it is the arrangement
 * that is known to work on the msm8916/apq8016-sbc reference in this very
 * kernel.
 *
 * Option (a) of the brief -- "create a child 'pcm' platform node the way
 * q6apm/q6afedai do" -- is not merely unhelpful here, it BREAKS the card. In
 * the same function:
 *
 *	if (codec) {
 *		ret = snd_soc_of_get_dai_link_codecs(dev, codec, link);
 *		if (platform) {
 *			link->no_pcm = 1;             /-* DPCM backend *-/
 *			link->ignore_pmdown_time = 1;
 *		}
 *	}
 *	if (platform || !codec) {
 *		link->ignore_suspend = 1;
 *		link->nonatomic = 1;
 *	}
 *
 * A "platform" child next to a "codec" child turns the link into a DPCM
 * backend, and pine declares no frontend, so playback would have nowhere to
 * go. Option (b) is therefore the only correct one, and it is what keeps the
 * DT node the same shape as the msm8916 one.
 *
 * ---------------------------------------------------------------------------
 * MASTER/SLAVE
 * ---------------------------------------------------------------------------
 * There is deliberately no .set_fmt() and no master/slave capability flag in
 * this file, so this DAI can never claim to be I2S master:
 *
 *  - qcom_snd_parse_of() never parses "dai-format", so link->dai_fmt == 0.
 *  - Neither in-tree lpass-cpu.c, nor msm8916-wcd-digital.c, nor
 *    msm8916-wcd-analog.c implements set_fmt (grep: zero hits in all three),
 *    so on the working apq8016-sbc reference nothing programs a master either.
 *    See snd/README.md section 1.5.
 *  - The brief states the codec side is the I2S master, because it divides its
 *    own MCLK to make dmic0_clk. Implementing set_fmt as master would
 *    contradict that directly, so it is not implemented.
 *
 * WHAT IS STILL PROGRAMMED, and the honest caveat:
 * LPAIF_I2SCTL.WSSRC is set to INTERNAL, which is what in-tree
 * lpass_cpu_daiops_hw_params() does and is consistent with apq8016_sbc.c
 * programming SPKR_CTL_PRI_WS_SLAVE_SEL_11 into spkr-iomux for MI2S_PRIMARY
 * and MIC_CTRL_TER_WS_SLAVE_SEL | MIC_CTRL_TLMM_SCLK_EN into mic-iomux for
 * MI2S_TERTIARY. On apq8016/msm8916 that combination means LPASS is the WS and
 * bit-clock source, which is the opposite of "codec is master". Which reading
 * is correct on SDM439 is not established by any file available locally, so it
 * is a module parameter:
 *
 *	insmod sdm439_lpaif.ko wssrc_external=1
 *
 * ---------------------------------------------------------------------------
 * CLOCKS
 * ---------------------------------------------------------------------------
 * Seven names, taken with devm_clk_bulk_get(), which matches by the .id string,
 * so the DT order does not matter (dts-fragment.dtsi still uses this order):
 *
 *	ahbix-clk	GCC CBCR +0x1c028
 *	mi2s-bit-clk0	GCC CBCR +0x1c068   LPAIF PRI I2S
 *	mi2s-bit-clk1	GCC CBCR +0x1c068   deliberately the same gate
 *	mi2s-bit-clk2	GCC CBCR +0x1c080   LPAIF SEC I2S
 *	mi2s-bit-clk3	GCC CBCR +0x1c098   LPAIF AUX I2S
 *	pcnoc-mport-clk	GCC CBCR +0x1c000
 *	pcnoc-sway-clk	GCC CBCR +0x1c004
 *
 * These are the seven exported by clk/gcc-sdm439-oot.c. Indices 0, 5 and 6 are
 * enabled once at probe, exactly as apq8016_lpass_init() enables
 * pcnoc-mport-clk, pcnoc-sway-clk and ahbix-clk. Indices 1..4, the bit clocks,
 * are per stream (.startup()/.shutdown()).
 *
 * If clk-sdm439-oot.ko is not loaded, devm_clk_bulk_get() returns
 * -EPROBE_DEFER and that value is returned unchanged, so probe is retried
 * automatically once the provider registers (really_probe() calls
 * driver_deferred_probe_trigger()). This driver never fails hard on a missing
 * clock provider.
 *
 * ---------------------------------------------------------------------------
 * SAFETY
 * ---------------------------------------------------------------------------
 *  - Every register access goes through lpaif_w32() / lpaif_r32() /
 *    lpaif_rmw(), each of which range-checks the offset against the size the
 *    DT declared and refuses anything that would leave the mapping.
 *  - Every offset in this file is relative to the DT window base. The driver
 *    never computes an absolute physical address, so a wrong "reg" value can
 *    only aim a write at the wrong block; it cannot make this driver reach
 *    outside its own mapping.
 *  - Probe performs ZERO register writes. The base is announced with dev_info
 *    so it can be checked in dmesg before anything else is attempted.
 *  - The module parameter write_en=0 disables every write, including the LPAIF
 *    IRQ mask. Bring the driver up that way first; see README.md.
 *  - iommus_required=0 relaxes the "must have an iommus property" defer, which
 *    the default leaves strict. See the note at that call site: mainline's own
 *    msm8916.dtsi has no iommus on its lpass node.
 *
 * ---------------------------------------------------------------------------
 * STATUS
 * ---------------------------------------------------------------------------
 * NOT COMPILED. No kernel build tree is configured in this checkout and none
 * was created. Everything below was established by reading source at
 * msm89x7/7.1.3 and by inspection. README.md lists exactly what was verified
 * and what was not.
 */

// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_dma.h>
#include <linux/printk.h>
#include <linux/ratelimit.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/uio.h>

#include <sound/asoc.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

/*
 * DAI ids.
 *
 * From include/dt-bindings/sound/qcom,lpass.h, read in this checkout at
 * fk/include/dt-bindings/sound/qcom,lpass.h:
 *	#define MI2S_PRIMARY	0
 *	#define MI2S_SECONDARY	1
 *	#define MI2S_TERTIARY	2
 *	#define MI2S_QUATERNARY	3
 *	#define MI2S_QUINARY	4
 *
 * Spelled out rather than #included so the module carries no dt-bindings
 * dependency. They are the two ids apq8016_sbc.c's switch handles that
 * sd/pine-sound.dtsi actually uses: MI2S_PRIMARY for playback, MI2S_TERTIARY
 * for capture. apq8016_sbc.c also handles MI2S_SECONDARY, MI2S_QUATERNARY and
 * MI2S_QUINARY; this driver does not register those DAI ids, and
 * sdm439_lpaif_of_xlate_dai_name() returns -EINVAL for them, so a DT that asks
 * for one fails loudly at card registration instead of mis-binding.
 */
#define SDM439_LPAIF_MI2S_PRIMARY	0u
#define SDM439_LPAIF_MI2S_TERTIARY	2u

/*
 * ---------------------------------------------------------------------------
 * LPAIF register map, apq8016 variant.
 *
 * Copied from sound/soc/qcom/lpass-apq8016.c, "apq8016_data", at ref
 * msm89x7/7.1.3:
 *
 *	.i2sctrl_reg_base    = 0x1000,  .i2sctrl_reg_stride = 0x1000,
 *	.i2s_ports           = 4,
 *	.irq_reg_base        = 0x6000,  .irq_reg_stride     = 0x1000,
 *	.irq_ports           = 3,
 *	.rdma_reg_base       = 0x8400,  .rdma_reg_stride    = 0x1000,
 *	.rdma_channels       = 2,
 *	.dmactl_audif_start  = 1,
 *	.wrdma_reg_base      = 0xb000,  .wrdma_reg_stride   = 0x1000,
 *	.wrdma_channel_start = 5,
 *	.wrdma_channels      = 2,
 *
 * These are a property of the LPAIF IP as instantiated on the apq8016/msm8916
 * family, not of the SoC address map, so they are the best available guess for
 * SDM439. They are NOT confirmed against any SDM439 documentation, for the
 * same reason the base address is not: no SDM439 description of this block is
 * available locally. Everything derived from them is an offset from the DT
 * window base; nothing here is an absolute address.
 *
 * Derived layout, offsets from the "lpass-lpaif" window base:
 *
 *	I2SCTL	port 0..3	0x1000 0x2000 0x3000 0x4000
 *	IRQ	port 0..2	0x6000 0x7000 0x8000
 *			      +0x00 en  +0x04 stat  +0x0c clear
 *	RDMA	chan 0..1	0x8400 0x9400
 *	WRDMA	chan 5..6	0xb000 0xc000
 *
 * Each DMA channel register block:
 *	+0x00 ctl  +0x04 base  +0x08 buff  +0x0c curr  +0x10 per  +0x14 percnt
 *
 * Highest offset this driver ever touches:
 *	0xb000 + 0x1000*(2-1) + 0x14 = 0xd014
 * so the DT window must be at least 0xd018 bytes. msm8916.dtsi and
 * dts-fragment.dtsi both use 0x10000, so there is ample slack.
 */
#define SDM439_LPAIF_MIN_WIN_SIZE	0xd018u

#define SDM439_LPAIF_I2S_BASE		0x1000u
#define SDM439_LPAIF_I2S_STRIDE		0x1000u

#define SDM439_LPAIF_IRQ_BASE		0x6000u
#define SDM439_LPAIF_IRQ_STRIDE		0x1000u
#define SDM439_LPAIF_IRQ_PORT_HOST	0u

#define SDM439_LPAIF_IRQ_REG(port, off) \
	(SDM439_LPAIF_IRQ_BASE + SDM439_LPAIF_IRQ_STRIDE * (u32)(port) + (u32)(off))
#define SDM439_LPAIF_IRQEN_OFF(port)	SDM439_LPAIF_IRQ_REG(port, 0x00u)
#define SDM439_LPAIF_IRQSTAT_OFF(port)	SDM439_LPAIF_IRQ_REG(port, 0x04u)
#define SDM439_LPAIF_IRQCLEAR_OFF(port)	SDM439_LPAIF_IRQ_REG(port, 0x0cu)

/* LPAIF_IRQ_BITSTRIDE / _PER / _XRUN / _ERR / _ALL from lpass-lpaif-reg.h. */
#define SDM439_LPAIF_IRQ_BITSTRIDE	3
#define SDM439_LPAIF_IRQ_PER(ch)		(1u << (SDM439_LPAIF_IRQ_BITSTRIDE * (ch)))
#define SDM439_LPAIF_IRQ_XRUN(ch)	(2u << (SDM439_LPAIF_IRQ_BITSTRIDE * (ch)))
#define SDM439_LPAIF_IRQ_ERR(ch)		(4u << (SDM439_LPAIF_IRQ_BITSTRIDE * (ch)))
#define SDM439_LPAIF_IRQ_ALL(ch)		(7u << (SDM439_LPAIF_IRQ_BITSTRIDE * (ch)))

#define SDM439_LPAIF_RDMA_BASE		0x8400u
#define SDM439_LPAIF_RDMA_STRIDE		0x1000u
#define SDM439_LPAIF_RDMA_CHANNELS	2u

#define SDM439_LPAIF_WRDMA_BASE		0xb000u
#define SDM439_LPAIF_WRDMA_STRIDE	0x1000u
#define SDM439_LPAIF_WRDMA_CH_START	5u
#define SDM439_LPAIF_WRDMA_CHANNELS	2u

#define SDM439_LPAIF_DMA_CTL(port)	((u32)(port) + 0x00u)
#define SDM439_LPAIF_DMA_BASE(port)	((u32)(port) + 0x04u)
#define SDM439_LPAIF_DMA_BUFF(port)	((u32)(port) + 0x08u)
#define SDM439_LPAIF_DMA_CURR(port)	((u32)(port) + 0x0cu)
#define SDM439_LPAIF_DMA_PER(port)	((u32)(port) + 0x10u)

/*
 * Direction -> DMA block.
 *
 *	playback  -> RDMA   (the engine READS from system memory)
 *	capture   -> WRDMA  (the engine WRITES into system memory)
 *
 * Same sense as the in-tree header's
 *
 *	#define __LPAIF_DMA_REG(v, chan, dir, reg)  \
 *		(dir ==  SNDRV_PCM_STREAM_PLAYBACK) ? \
 *			LPAIF_RDMA##reg##_REG(v, chan) : \
 *			LPAIF_WRDMA##reg##_REG(v, chan)
 *
 * (sound/soc/qcom/lpass-lpaif-reg.h). It reads as though it were inverted; it
 * is not, and it agrees with apq8016_lpass_alloc_dma_channel(), which searches
 * [0, rdma_channels) for playback and [wrdma_channel_start,
 * wrdma_channel_start + wrdma_channels) for capture. Do not "fix" this.
 */
static u32 sdm439_lpaif_dma_block(u32 dma_ch)
{
	if (dma_ch >= SDM439_LPAIF_WRDMA_CH_START)
		return SDM439_LPAIF_WRDMA_BASE +
		       SDM439_LPAIF_WRDMA_STRIDE *
		       (dma_ch - SDM439_LPAIF_WRDMA_CH_START);

	return SDM439_LPAIF_RDMA_BASE +
	       SDM439_LPAIF_RDMA_STRIDE * dma_ch;
}

/* LPAIF_I2SCTL_REG offsets and fields, from lpass-lpaif-reg.h. */
#define SDM439_LPAIF_I2SCTL_OFF(port) \
	(SDM439_LPAIF_I2S_BASE + SDM439_LPAIF_I2S_STRIDE * (u32)(port))

#define SDM439_LPAIF_I2SCTL_LOOPBACK_MASK	BIT(15)
#define SDM439_LPAIF_I2SCTL_SPKE_MASK		BIT(14)
#define SDM439_LPAIF_I2SCTL_SPKMODE_MASK		GENMASK(13, 10)
#define SDM439_LPAIF_I2SCTL_SPKMODE_SHIFT	10
#define SDM439_LPAIF_I2SCTL_SPKMODE_NONE	(0u << SDM439_LPAIF_I2SCTL_SPKMODE_SHIFT)
#define SDM439_LPAIF_I2SCTL_SPKMODE_SD0	(1u << SDM439_LPAIF_I2SCTL_SPKMODE_SHIFT)
#define SDM439_LPAIF_I2SCTL_SPKMODE_SD1	(2u << SDM439_LPAIF_I2SCTL_SPKMODE_SHIFT)
#define SDM439_LPAIF_I2SCTL_SPKMODE_SD2	(3u << SDM439_LPAIF_I2SCTL_SPKMODE_SHIFT)
#define SDM439_LPAIF_I2SCTL_SPKMODE_SD3	(4u << SDM439_LPAIF_I2SCTL_SPKMODE_SHIFT)
#define SDM439_LPAIF_I2SCTL_SPKMONO_MASK	BIT(9)
#define SDM439_LPAIF_I2SCTL_SPKMONO_MONO	BIT(9)
#define SDM439_LPAIF_I2SCTL_MICEN_MASK		BIT(8)
#define SDM439_LPAIF_I2SCTL_MICMODE_MASK	GENMASK(7, 4)
#define SDM439_LPAIF_I2SCTL_MICMODE_SHIFT	4
#define SDM439_LPAIF_I2SCTL_MICMODE_NONE	(0u << SDM439_LPAIF_I2SCTL_MICMODE_SHIFT)
#define SDM439_LPAIF_I2SCTL_MICMODE_SD0	(1u << SDM439_LPAIF_I2SCTL_MICMODE_SHIFT)
#define SDM439_LPAIF_I2SCTL_MICMODE_SD1	(2u << SDM439_LPAIF_I2SCTL_MICMODE_SHIFT)
#define SDM439_LPAIF_I2SCTL_MICMODE_SD2	(3u << SDM439_LPAIF_I2SCTL_MICMODE_SHIFT)
#define SDM439_LPAIF_I2SCTL_MICMODE_SD3	(4u << SDM439_LPAIF_I2SCTL_MICMODE_SHIFT)
#define SDM439_LPAIF_I2SCTL_MICMONO_MASK	BIT(3)
#define SDM439_LPAIF_I2SCTL_MICMONO_MONO	BIT(3)
#define SDM439_LPAIF_I2SCTL_WSSRC_MASK		BIT(2)
#define SDM439_LPAIF_I2SCTL_WSSRC_INTERNAL	(0u)
#define SDM439_LPAIF_I2SCTL_WSSRC_EXTERNAL	BIT(2)
#define SDM439_LPAIF_I2SCTL_BW_MASK		GENMASK(1, 0)
#define SDM439_LPAIF_I2SCTL_BW_16		(0u)
#define SDM439_LPAIF_I2SCTL_BW_24		(1u)
#define SDM439_LPAIF_I2SCTL_BW_32		(2u)

/* LPAIF_DMACTL fields, from lpass-lpaif-reg.h. */
#define SDM439_LPAIF_DMACTL_DYNCLK_MASK	BIT(12)
#define SDM439_LPAIF_DMACTL_BURSTEN_MASK	BIT(11)
#define SDM439_LPAIF_DMACTL_WPSCNT_MASK	GENMASK(10, 8)
#define SDM439_LPAIF_DMACTL_WPSCNT_ONE	(0u << 8)
#define SDM439_LPAIF_DMACTL_WPSCNT_TWO	(2u << 8)
#define SDM439_LPAIF_DMACTL_AUDINTF_MASK	GENMASK(7, 4)
#define SDM439_LPAIF_DMACTL_AUDINTF_SHIFT	4
#define SDM439_LPAIF_DMACTL_FIFOWM_MASK	GENMASK(3, 1)
#define SDM439_LPAIF_DMACTL_FIFOWM_8	(7u << 1)
#define SDM439_LPAIF_DMACTL_ENABLE_MASK	BIT(0)

/*
 * dmactl_audif_start. lpass-apq8016.c sets .dmactl_audif_start = 1 for apq8016
 * and lpass-platform.c computes
 *
 *	dma_port = pcm_data->i2s_port + v->dmactl_audif_start;
 *	regmap_fields_write(dmactl->intf, id, LPAIF_DMACTL_AUDINTF(dma_port));
 *
 * so AUDINTF = mi2s port + 1. Reproduced.
 */
#define SDM439_LPAIF_DMACTL_AUDIF_START	1u

/* 131072000, from lpass.h's LPASS_AHBIX_CLOCK_FREQUENCY. */
#define SDM439_LPAIF_AHBIX_HZ		131072000UL

/* lpass-platform.c: 24 * 2 * 1024 bytes in 2 periods. */
#define SDM439_LPAIF_BUF_BYTES		(24 * 2 * 1024)
#define SDM439_LPAIF_PERIODS		2

/* The DT name of the one MMIO region. Unchanged from msm8916.dtsi. */
#define SDM439_LPAIF_REG_NAME		"lpass-lpaif"

/*
 * The DT name of the interrupt. Unchanged from msm8916.dtsi, and the same
 * string lpass-platform.c passes to platform_get_irq_byname() and to
 * devm_request_irq().
 */
#define SDM439_LPAIF_IRQ_NAME		"lpass-irq-lpaif"

/*
 * DMA channel slots. RDMA occupies 0..1, WRDMA occupies 5..6. Sized at 8 to
 * match lpass.h's LPASS_MAX_DMA_CHANNELS, so a slot index is a real LPAIF
 * channel number and can be used directly as the shift in LPAIF_IRQ_ALL(ch).
 */
#define SDM439_LPAIF_MAX_DMA_CH		8

/*
 * Rates. 48000 is primary and is the only one expected to sound correct: the
 * LPAIF bit clock cannot actually be reprogrammed on this SoC, because
 * clk/gcc-sdm439-oot.c implements .set_rate as a no-op returning 0 (the RCG2
 * parents GCC_GPLL1 and GCC_XO_GPLL0_BIMC do not exist in
 * drivers/clk/qcom/gcc-msm8917.c). 48000/2ch/S16_LE is what the stock ROM
 * drives the analog path at.
 *
 * The other four are declared because the brief requires them and because they
 * do not by themselves break anything. Restrict to 48 kHz with the "rates"
 * module parameter: rates=64, because SNDRV_PCM_RATE_48000 is bit 6.
 *
 * For reference, the OTHER machine-driver compatible in apq8016_sbc.c,
 * "qcom,msm8916-qdsp6-sndcard", installs
 * msm8916_qdsp6_be_hw_params_fixup(), which force-pins every link to exactly
 * 48000 / 2 channels / S16_LE. That string selects the QDSP6 AFE DAIs, which do
 * not exist in this kernel, so the card uses "qcom,apq8016-sbc-sndcard" and
 * that fixup is NOT installed. See sd/pine-sound.dtsi section 7.
 */
#define SDM439_LPAIF_RATES_DEFAULT \
	(SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000 | SNDRV_PCM_RATE_32000 | \
	 SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000)

/*
 * The seven clock names, in the order clk/dts-fragment.dtsi lists them.
 * devm_clk_bulk_get() matches by the .id string so the DT order is free, but
 * keeping the two in step makes the DT readable.
 */
static const char * const sdm439_lpaif_clk_names[] = {
	"ahbix-clk",
	"mi2s-bit-clk0",
	"mi2s-bit-clk1",
	"mi2s-bit-clk2",
	"mi2s-bit-clk3",
	"pcnoc-mport-clk",
	"pcnoc-sway-clk",
};

#define SDM439_LPAIF_NUM_CLKS \
	((int)(sizeof(sdm439_lpaif_clk_names) / \
	       sizeof(sdm439_lpaif_clk_names[0])))

/*
 * Indices into the above array that are AHB/fabric ports rather than MI2S bit
 * clocks. Enabled once and left on for the lifetime of the probe, as
 * apq8016_lpass_init() does. The four bit clocks (indices 1..4) are per stream.
 */
static const unsigned int sdm439_lpaif_fabric_clk_idx[] = {
	0,	/* ahbix-clk       */
	5,	/* pcnoc-mport-clk */
	6,	/* pcnoc-sway-clk  */
};

#define SDM439_LPAIF_NUM_FABRIC_CLKS \
	((int)(sizeof(sdm439_lpaif_fabric_clk_idx) / \
	       sizeof(sdm439_lpaif_fabric_clk_idx[0])))

/*
 * mi2s-bit-clkN, where N is the DAI's index in the upstream array. For apq8016
 * that index equals the MI2S id, so:
 *	MI2S_PRIMARY  (0) -> mi2s-bit-clk0
 *	MI2S_TERTIARY (2) -> mi2s-bit-clk2
 * clk/dts-fragment.dtsi points mi2s-bit-clk0 and mi2s-bit-clk1 at the same
 * PRI_I2S gate on purpose, as msm8916.dtsi does.
 */
#define SDM439_LPAIF_BIT_CLK_IDX(dai_id)	(1 + (int)(dai_id))

struct sdm439_lpaif;

struct sdm439_lpaif_pcm {
	struct sdm439_lpaif	*lpaif;
	unsigned int		i2s_port;
	int			dma_ch;
};

struct sdm439_lpaif {
	struct device		*dev;
	struct resource		res;
	void __iomem		*base;
	resource_size_t		win_start;
	size_t			win_size;

	struct clk_bulk_data	clks[SDM439_LPAIF_NUM_CLKS];
	struct clk		*ahbix_clk;

	int			irq;

	spinlock_t		lock;		/* dma_ch_map and substream[] */
	unsigned long		dma_ch_map;
	struct snd_pcm_substream *substream[SDM439_LPAIF_MAX_DMA_CH];

	bool			write_en;
	bool			wssrc_external;
	unsigned int		sd_lines;
	u32			skipped_writes;
};

/* ------------------------------------------------------------------------ */
/* Module parameters. Every one of them exists so a candidate can be tested */
/* without rebuilding anything. See README.md.                               */
/* ------------------------------------------------------------------------ */

static bool write_en = true;
module_param(write_en, bool, 0444);
MODULE_PARM_DESC(write_en,
		 "allow register writes; 0 makes probe and every PCM path write-free");

static unsigned int rates = SDM439_LPAIF_RATES_DEFAULT;
module_param(rates, uint, 0444);
MODULE_PARM_DESC(rates,
		 "SNDRV_PCM_RATE_* bitmask offered to ALSA (48000 alone is 64)");

static bool wssrc_external;
module_param(wssrc_external, bool, 0444);
MODULE_PARM_DESC(wssrc_external,
		 "0: LPAIF I2SCTL.WSSRC = INTERNAL, as in-tree lpass-cpu does; 1: EXTERNAL");

static unsigned int sd_lines;
module_param(sd_lines, uint, 0444);
MODULE_PARM_DESC(sd_lines,
		 "MI2S SD line muxed onto the port, 0..3 (0 = SD0, the reference)");

static bool iommus_required = true;
module_param(iommus_required, bool, 0444);
MODULE_PARM_DESC(iommus_required,
		 "1: defer without an 'iommus' property (default); 0: warn and carry on");

/* ------------------------------------------------------------------------ */
/* Bounds-checked register access.                                          */
/* ------------------------------------------------------------------------ */

/*
 * lpaif_w32() is the only way this driver changes hardware. It
 *   1. refuses any offset that would leave the DT-declared window, and
 *   2. does nothing at all when write_en == 0.
 * Both refusals are counted, so a driver that has silently done nothing is
 * visible in dmesg at remove time rather than looking like a working one.
 */
static bool lpaif_off_ok(struct sdm439_lpaif *lpaif, unsigned int off)
{
	if (off + sizeof(u32) > lpaif->win_size) {
		dev_warn_ratelimited(lpaif->dev,
				     "refusing access to +0x%x: window is only 0x%zx bytes\n",
				     off, lpaif->win_size);
		return false;
	}

	return true;
}

static bool lpaif_w32(struct sdm439_lpaif *lpaif, unsigned int off, u32 val)
{
	if (!lpaif_off_ok(lpaif, off))
		return false;

	if (!lpaif->write_en) {
		lpaif->skipped_writes++;
		return false;
	}

	writel(val, lpaif->base + off);
	return true;
}

static u32 lpaif_r32(struct sdm439_lpaif *lpaif, unsigned int off)
{
	if (!lpaif_off_ok(lpaif, off))
		return 0;

	return readl(lpaif->base + off);
}

/*
 * Read-modify-write. Equivalent to the regmap_fields_write() calls in-tree
 * lpass uses, because a regmap field write is also RMW on the live register.
 * Doing it this way keeps the driver independent of regmap field plumbing and
 * of any regmap cache, which matters because no known-good reset state for this
 * block exists on this board and a cache would only ever hide that.
 */
static bool lpaif_rmw(struct sdm439_lpaif *lpaif, unsigned int off,
		      u32 mask, u32 val)
{
	u32 old, new;

	if (!lpaif_off_ok(lpaif, off))
		return false;

	if (!lpaif->write_en) {
		lpaif->skipped_writes++;
		return false;
	}

	old = readl(lpaif->base + off);
	new = (old & ~mask) | (val & mask);
	writel(new, lpaif->base + off);

	return true;
}

/* ------------------------------------------------------------------------ */
/* Small accessors.                                                          */
/* ------------------------------------------------------------------------ */

static struct sdm439_lpaif *lpaif_of(struct snd_soc_dai *dai)
{
	/*
	 * snd_soc_component_get_drvdata() is component->driver_data, which
	 * devm_snd_soc_register_component() fills from dev_get_drvdata(dev),
	 * i.e. from the platform_set_drvdata() call in probe. That is the same
	 * route in-tree lpass-platform.c uses (snd_soc_component_get_drvdata()
	 * on the components it registers from &pdev->dev), so it does not rely
	 * on any per-DAI drvdata mechanism.
	 */
	return snd_soc_component_get_drvdata(dai->component);
}

static struct sdm439_lpaif_pcm *lpaif_pcm_data(struct snd_pcm_substream *ss)
{
	/*
	 * runtime->private_data, not substream->dma_buffer.pcm_data: the latter
	 * belongs to the DMA buffer and overwriting it breaks
	 * snd_pcm_set_fixed_buffer_all()'s bookkeeping. lpass-platform.c stores
	 * exactly this struct in runtime->private_data and so does this.
	 */
	return (struct sdm439_lpaif_pcm *)ss->runtime->private_data;
}

static struct clk *lpaif_bit_clk(struct sdm439_lpaif *lpaif,
				 struct snd_soc_dai *dai)
{
	unsigned int id = dai->driver->id;
	int idx = SDM439_LPAIF_BIT_CLK_IDX(id);

	if (idx < 1 || idx >= SDM439_LPAIF_NUM_CLKS)
		return NULL;

	return lpaif->clks[idx].clk;
}

/*
 * SPKEN / MICEN. These are the idempotent helpers shared by the CPU DAI's
 * .start()/.stop() and by the platform component's .start()/.stop(); the
 * START/STOP arms of .trigger() on each side call the same pair.
 *
 * ASoC's soc_pcm path calls BOTH snd_soc_trigger_start() on the CPU DAI and
 * snd_soc_component_trigger() on the platform component. In-tree lpass relies
 * on that split (lpass-cpu.c does SPKEN/MICEN plus the bit clock,
 * lpass-platform.c does DMACTL plus the IRQ) and it is reproduced identically
 * here. Because every helper is an idempotent register update, being called
 * from more than one place is harmless.
 */
static void lpaif_i2s_set_dir(struct sdm439_lpaif *lpaif, unsigned int port,
			      int stream, bool on)
{
	u32 off = SDM439_LPAIF_I2SCTL_OFF(port);

	if (stream == SNDRV_PCM_STREAM_PLAYBACK)
		lpaif_rmw(lpaif, off, SDM439_LPAIF_I2SCTL_SPKE_MASK,
			  on ? SDM439_LPAIF_I2SCTL_SPKE_MASK : 0u);
	else
		lpaif_rmw(lpaif, off, SDM439_LPAIF_I2SCTL_MICEN_MASK,
			  on ? SDM439_LPAIF_I2SCTL_MICEN_MASK : 0u);
}

/* ------------------------------------------------------------------------ */
/* DMA channel allocation.                                                   */
/*                                                                            */
/* Same policy as apq8016_lpass_alloc_dma_channel(): playback takes the next  */
/* free RDMA channel, capture the next free WRDMA channel.                    */
/* ------------------------------------------------------------------------ */

static int lpaif_alloc_dma_ch(struct sdm439_lpaif *lpaif, int stream)
{
	unsigned long flags;
	int ch;

	spin_lock_irqsave(&lpaif->lock, flags);

	if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
		ch = find_first_zero_bit(&lpaif->dma_ch_map,
					 SDM439_LPAIF_RDMA_CHANNELS);
		if (ch >= 0 && ch < (int)SDM439_LPAIF_RDMA_CHANNELS)
			set_bit(ch, &lpaif->dma_ch_map);
		else
			ch = -EBUSY;
	} else {
		unsigned long end = SDM439_LPAIF_WRDMA_CH_START +
				    SDM439_LPAIF_WRDMA_CHANNELS;

		ch = find_next_zero_bit(&lpaif->dma_ch_map, end,
					SDM439_LPAIF_WRDMA_CH_START);
		if (ch >= 0 && ch < (int)end)
			set_bit(ch, &lpaif->dma_ch_map);
		else
			ch = -EBUSY;
	}

	if (ch >= 0)
		lpaif->substream[ch] = NULL;

	spin_unlock_irqrestore(&lpaif->lock, flags);

	return ch;
}

static void lpaif_free_dma_ch(struct sdm439_lpaif *lpaif, int ch)
{
	unsigned long flags;

	if (!lpaif || ch < 0 || ch >= SDM439_LPAIF_MAX_DMA_CH)
		return;

	spin_lock_irqsave(&lpaif->lock, flags);
	clear_bit(ch, &lpaif->dma_ch_map);
	lpaif->substream[ch] = NULL;
	spin_unlock_irqrestore(&lpaif->lock, flags);
}

/* ------------------------------------------------------------------------ */
/* CPU DAI ops.                                                              */
/* ------------------------------------------------------------------------ */

static int lpaif_daiops_probe(struct snd_soc_dai *dai)
{
	struct sdm439_lpaif *lpaif = lpaif_of(dai);

	/* "ensure audio hardware is disabled", exactly like lpass-cpu.c. */
	lpaif_w32(lpaif, SDM439_LPAIF_I2SCTL_OFF(dai->driver->id), 0);

	return 0;
}

static int lpaif_daiops_startup(struct snd_pcm_substream *ss,
				struct snd_soc_dai *dai)
{
	struct clk *bit = lpaif_bit_clk(lpaif_of(dai), dai);

	if (!bit)
		return 0;

	return clk_prepare_enable(bit);
}

static void lpaif_daiops_shutdown(struct snd_pcm_substream *ss,
				  struct snd_soc_dai *dai)
{
	struct clk *bit = lpaif_bit_clk(lpaif_of(dai), dai);

	if (bit)
		clk_disable_unprepare(bit);
}

/*
 * Program LPAIF_I2SCTL_REG for the port this DAI owns.
 *
 * The brief describes the field set as "WS_SRC_INTERNAL, no loopback, SPKMODE
 * none, mic/speaker enable as appropriate". Three of those four are honoured
 * literally. SPKMODE_NONE is not: it is 0, it selects no SD line at all, and it
 * would make the port emit nothing. The working reference writes
 * LPAIF_I2SCTL_SPKMODE_SD0 / LPAIF_I2SCTL_MICMODE_SD0 for 1 and 2 channels
 * (lpass_cpu_daiops_hw_params), and that is what is done here, with the line
 * selectable at runtime through sd_lines=.
 *
 * The deliberate difference from in-tree is that a clk_set_rate() failure is
 * warned about rather than propagated. lpass-cpu.c treats it as fatal, which
 * would make hw_params fail here: clk/gcc-sdm439-oot.c's .set_rate returns 0 so
 * it currently succeeds, but a change there must not be able to take audio
 * down. The codec is the clock source on this board anyway.
 */
static int lpaif_daiops_hw_params(struct snd_pcm_substream *ss,
				  struct snd_pcm_hw_params *params,
				  struct snd_soc_dai *dai)
{
	struct sdm439_lpaif *lpaif = lpaif_of(dai);
	unsigned int port = dai->driver->id;
	unsigned int channels = params_channels(params);
	unsigned int rate = params_rate(params);
	snd_pcm_format_t format = params_format(params);
	struct clk *bit = lpaif_bit_clk(lpaif, dai);
	unsigned int sd;
	u32 v;
	int bitwidth, ret;

	if (lpaif->sd_lines > 3) {
		dev_err(dai->dev, "sd_lines=%u out of range\n", lpaif->sd_lines);
		return -EINVAL;
	}

	if (channels != 1 && channels != 2) {
		/*
		 * Only 1 and 2 channels are accepted, because that is all the
		 * board's I2S pinmux brings out (sd/pine-sound.dtsi section 5,
		 * gpio69..74) and because lpass_cpu_daiops_hw_params() maps
		 * anything above 2 onto a quad/6ch/8ch SD grouping this pinmux
		 * does not reach.
		 */
		dev_err(dai->dev,
			"only 1 or 2 channels are wired on this board, got %u\n",
			channels);
		return -EINVAL;
	}

	bitwidth = snd_pcm_format_width(format);
	if (bitwidth < 0)
		return bitwidth;

	switch (bitwidth) {
	case 16:
		v = SDM439_LPAIF_I2SCTL_BW_16;
		break;
	case 24:
		v = SDM439_LPAIF_I2SCTL_BW_24;
		break;
	case 32:
		v = SDM439_LPAIF_I2SCTL_BW_32;
		break;
	default:
		dev_err(dai->dev, "unsupported bit width %d\n", bitwidth);
		return -EINVAL;
	}

	v |= lpaif->wssrc_external ? SDM439_LPAIF_I2SCTL_WSSRC_EXTERNAL :
				     SDM439_LPAIF_I2SCTL_WSSRC_INTERNAL;
	/* LOOPBACK_DISABLE is 0, so it needs no explicit bit here. */

	sd = 1u + lpaif->sd_lines;	/* 1 = SD0 ... 4 = SD3 */

	if (ss->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		v |= (sd << SDM439_LPAIF_I2SCTL_SPKMODE_SHIFT) &
		     SDM439_LPAIF_I2SCTL_SPKMODE_MASK;
		if (channels == 1)
			v |= SDM439_LPAIF_I2SCTL_SPKMONO_MONO;
	} else {
		v |= (sd << SDM439_LPAIF_I2SCTL_MICMODE_SHIFT) &
		     SDM439_LPAIF_I2SCTL_MICMODE_MASK;
		if (channels == 1)
			v |= SDM439_LPAIF_I2SCTL_MICMONO_MONO;
	}

	if (!lpaif_w32(lpaif, SDM439_LPAIF_I2SCTL_OFF(port), v))
		dev_info(dai->dev,
			 "I2SCTL(port %u) would be 0x%08x; not written\n",
			 port, v);
	else
		dev_dbg(dai->dev, "I2SCTL(port %u) = 0x%08x\n", port, v);

	if (bit) {
		unsigned long want = (unsigned long)rate * bitwidth * 2;

		ret = clk_set_rate(bit, want);
		if (ret)
			dev_warn(dai->dev,
				 "clk_set_rate(mi2s-bit-clk, %lu) failed: %d; continuing\n",
				 want, ret);
		else
			dev_dbg(dai->dev, "mi2s-bit-clk requested at %lu Hz\n",
				want);
	}

	return 0;
}

static int lpaif_daiops_hw_free(struct snd_pcm_substream *ss,
				struct snd_soc_dai *dai)
{
	struct sdm439_lpaif *lpaif = lpaif_of(dai);

	lpaif_w32(lpaif, SDM439_LPAIF_I2SCTL_OFF(dai->driver->id), 0);

	return 0;
}

/*
 * The plain-ALSA CPU DAI entry points. The ASoC soc_pcm path drives .trigger()
 * instead; both call the same idempotent register updates, so providing both is
 * safe, and it means the DAI is also usable outside a machine driver.
 */
static int lpaif_daiops_start(struct snd_pcm_substream *ss,
			      struct snd_soc_dai *dai)
{
	lpaif_i2s_set_dir(lpaif_of(dai), dai->driver->id, ss->stream, true);
	return 0;
}

static int lpaif_daiops_stop(struct snd_pcm_substream *ss,
			     struct snd_soc_dai *dai)
{
	lpaif_i2s_set_dir(lpaif_of(dai), dai->driver->id, ss->stream, false);
	return 0;
}

/*
 * ASoC asks the codec side for the format. This DAI never implements
 * set_fmt / set_sysclk / set_tdm_slot, exactly like in-tree lpass-cpu.c. See
 * MASTER/SLAVE at the top of this file for why.
 */
static const struct snd_soc_dai_ops sdm439_lpaif_dai_ops = {
	.probe		= lpaif_daiops_probe,
	.startup	= lpaif_daiops_startup,
	.shutdown	= lpaif_daiops_shutdown,
	.hw_params	= lpaif_daiops_hw_params,
	.hw_free	= lpaif_daiops_hw_free,
	.start		= lpaif_daiops_start,
	.stop		= lpaif_daiops_stop,
};

/*
 * The two DAIs.
 *
 * DENSE on purpose: the array is handed straight to
 * devm_snd_soc_register_component(), which creates one DAI per element, so a
 * designated-initializer hole at index 1 (MI2S_SECONDARY, which this driver
 * does not provide) would create a nameless zeroed DAI. The .id fields carry
 * the real MI2S ids, and of_xlate_dai_name() matches on .id, not on index.
 */
static struct snd_soc_dai_driver sdm439_lpaif_dai_driver[] = {
	{
		.id		= SDM439_LPAIF_MI2S_PRIMARY,
		.name		= "sdm439-lpaif-primary",
		.symmetric_rate	= 1,
		.playback	= {
			.stream_name	= "Primary Playback",
			.formats	= SNDRV_PCM_FMTBIT_S16 |
					  SNDRV_PCM_FMTBIT_S24 |
					  SNDRV_PCM_FMTBIT_S32,
			.rates		= SDM439_LPAIF_RATES_DEFAULT,
			.rate_min	= 8000,
			.rate_max	= 48000,
			.channels_min	= 1,
			.channels_max	= 2,
		},
		.capture	= {
			.stream_name	= "Primary Capture",
			.formats	= SNDRV_PCM_FMTBIT_S16 |
					  SNDRV_PCM_FMTBIT_S24 |
					  SNDRV_PCM_FMTBIT_S32,
			.rates		= SDM439_LPAIF_RATES_DEFAULT,
			.rate_min	= 8000,
			.rate_max	= 48000,
			.channels_min	= 1,
			.channels_max	= 2,
		},
		.ops		= &sdm439_lpaif_dai_ops,
	},
	{
		.id		= SDM439_LPAIF_MI2S_TERTIARY,
		.name		= "sdm439-lpaif-tertiary",
		.symmetric_rate	= 1,
		.capture	= {
			.stream_name	= "Tertiary Capture",
			.formats	= SNDRV_PCM_FMTBIT_S16 |
					  SNDRV_PCM_FMTBIT_S24 |
					  SNDRV_PCM_FMTBIT_S32,
			.rates		= SDM439_LPAIF_RATES_DEFAULT,
			.rate_min	= 8000,
			.rate_max	= 48000,
			.channels_min	= 1,
			.channels_max	= 2,
		},
		.ops		= &sdm439_lpaif_dai_ops,
	},
};

#define SDM439_LPAIF_NUM_DAI \
	((int)(sizeof(sdm439_lpaif_dai_driver) / \
	       sizeof(sdm439_lpaif_dai_driver[0])))

/*
 * Translates the DT "sound-dai = <&lpass MI2S_PRIMARY>" argument into a DAI
 * driver .name. This is the same mechanism in-tree lpass-cpu.c uses
 * (asoc_qcom_of_xlate_dai_name()) and it is required because qcom_snd_parse_of()
 * sets link->id = args.args[0] and apq8016_sbc.c then switches on cpu_dai->id.
 */
static int sdm439_lpaif_of_xlate_dai_name(struct snd_soc_component *component,
					  const struct of_phandle_args *args,
					  const char **dai_name)
{
	int i;

	if (!args || args->args_count < 1)
		return -EINVAL;

	for (i = 0; i < SDM439_LPAIF_NUM_DAI; i++) {
		if (sdm439_lpaif_dai_driver[i].id == args->args[0]) {
			*dai_name = sdm439_lpaif_dai_driver[i].name;
			return 0;
		}
	}

	return -EINVAL;
}

static struct snd_soc_component_driver sdm439_lpaif_dai_comp_driver = {
	.name		= "sdm439-lpaif-dai",
	.of_xlate_dai_name = sdm439_lpaif_of_xlate_dai_name,
	.legacy_dai_naming = 1,
};

/* ------------------------------------------------------------------------ */
/* Platform (PCM) component ops.                                            */
/* ------------------------------------------------------------------------ */

static int lpaif_pcmops_pcm_new(struct snd_soc_component *component,
				struct snd_soc_pcm_runtime *rtd)
{
	struct snd_pcm *pcm = rtd->pcm;
	struct device *dev = component->dev;
	struct device_node *np = dev->of_node;
	unsigned int buf = SDM439_LPAIF_BUF_BYTES;
	unsigned int periods = SDM439_LPAIF_PERIODS;
	int ret;

	/*
	 * Two optional DT overrides, so buffer and period can be retuned from
	 * the device tree without rebuilding the module:
	 *
	 *	qcom,sdm439-lpaif,buffer-size   bytes,  default 49152
	 *	qcom,sdm439-lpaif,periods      count,  default 2
	 *
	 * lpass-platform.c hard-codes 24*2*1024 bytes in 2 periods, which at
	 * 48 kHz / 2ch / S16_LE is 6144 frames per period, i.e. 256 ms of
	 * end-to-end latency. 8 periods divides the same buffer evenly and cuts
	 * that to 64 ms if the DTB can be changed.
	 */
	if (np) {
		of_property_read_u32(np, "qcom,sdm439-lpaif,buffer-size", &buf);
		of_property_read_u32(np, "qcom,sdm439-lpaif,periods", &periods);
	}

	if (!buf || !periods || buf % periods) {
		dev_err(dev, "bad buffer/period request: %u bytes in %u periods\n",
			buf, periods);
		return -EINVAL;
	}

	pcm->hw.buffer_bytes_max	= buf;
	pcm->hw.period_bytes_max	= buf / periods;
	pcm->hw.period_bytes_min	= buf / periods;
	pcm->hw.periods_min		= periods;
	pcm->hw.periods_max		= periods;

	/*
	 * SNDRV_DMA_TYPE_NONCOHERENT, exactly as lpass-platform.c does. The LPAIF
	 * DMA is not coherent with the CPU on this block, so the buffer must be
	 * allocated with dma_alloc_noncoherent() (which
	 * snd_pcm_set_fixed_buffer_all() does) and must never be touched
	 * directly. .copy below is the only CPU path into it.
	 *
	 * component->dev is the lpass platform device, which is the same device
	 * whose "iommus" property probe validated. That is why probe refuses to
	 * continue without one.
	 */
	ret = snd_pcm_set_fixed_buffer_all(pcm, SNDRV_DMA_TYPE_NONCOHERENT,
					   dev, buf);
	if (ret < 0)
		return ret;

	return 0;
}

static int lpaif_pcmops_open(struct snd_soc_component *component,
			     struct snd_pcm_substream *ss)
{
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(ss->runtime, 0);
	struct sdm439_lpaif *lpaif = lpaif_of(cpu_dai);
	struct sdm439_lpaif_pcm *pcm_data;
	int ch;

	pcm_data = devm_kzalloc(component->dev, sizeof(*pcm_data), GFP_KERNEL);
	if (!pcm_data)
		return -ENOMEM;

	ch = lpaif_alloc_dma_ch(lpaif, ss->stream);
	if (ch < 0) {
		dev_err_ratelimited(cpu_dai->dev,
				    "no free LPAIF DMA channel for stream %d\n",
				    ss->stream);
		devm_kfree(component->dev, pcm_data);
		return ch;
	}

	pcm_data->lpaif		= lpaif;
	pcm_data->i2s_port	= cpu_dai->driver->id;
	pcm_data->dma_ch	= ch;

	ss->runtime->private_data = pcm_data;

	/*
	 * Publish the substream so the LPAIF IRQ handler can find it. Done
	 * after the channel is reserved, under the same lock, so the handler
	 * can never see a half-initialised channel.
	 */
	spin_lock_irq(&lpaif->lock);
	lpaif->substream[ch] = ss;
	spin_unlock_irq(&lpaif->lock);

	/* LPAIF_DMACTL_REG = 0 while idle, as lpass-platform.c's .open does. */
	lpaif_w32(lpaif, SDM439_LPAIF_DMA_CTL(sdm439_lpaif_dma_block(ch)), 0);

	dev_dbg(cpu_dai->dev,
		"stream %d: i2s port %u, DMA channel %u (%s)\n",
		ss->stream, pcm_data->i2s_port, pcm_data->dma_ch,
		ss->stream == SNDRV_PCM_STREAM_PLAYBACK ? "RDMA" : "WRDMA");

	return 0;
}

static int lpaif_pcmops_close(struct snd_soc_component *component,
			      struct snd_pcm_substream *ss)
{
	struct sdm439_lpaif_pcm *pcm_data = lpaif_pcm_data(ss);

	if (pcm_data)
		lpaif_free_dma_ch(pcm_data->lpaif, pcm_data->dma_ch);

	/*
	 * runtime->private_data is left alone: lpass-platform.c frees its own
	 * per-stream struct here, but this one came from devm_kzalloc() on the
	 * platform device and is released with it. Only the channel reservation
	 * and the substream back-pointer are taken back.
	 */
	return 0;
}

static int lpaif_pcmops_hw_params(struct snd_soc_component *component,
				  struct snd_pcm_substream *ss,
				  struct snd_pcm_hw_params *params)
{
	struct sdm439_lpaif_pcm *pcm_data = lpaif_pcm_data(ss);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(ss->runtime, 0);
	struct sdm439_lpaif *lpaif = lpaif_of(cpu_dai);
	unsigned int channels = params_channels(params);
	int bitwidth = snd_pcm_format_width(params_format(params));
	u32 ctl = SDM439_LPAIF_DMA_CTL(sdm439_lpaif_dma_block(pcm_data->dma_ch));
	u32 audintf, wpscnt;

	if (!pcm_data)
		return -EINVAL;

	if (bitwidth < 0)
		return bitwidth;

	if (channels != 1 && channels != 2) {
		dev_err(cpu_dai->dev,
			"only 1 or 2 channels are wired on this board, got %u\n",
			channels);
		return -EINVAL;
	}

	/*
	 * AUDINTF. lpass-platform.c computes
	 *	dma_port = i2s_port + dmactl_audif_start
	 * with dmactl_audif_start == 1 for apq8016, then writes
	 *	LPAIF_DMACTL_AUDINTF(dma_port) into the 4-bit field at bit 4.
	 */
	audintf = (pcm_data->i2s_port + SDM439_LPAIF_DMACTL_AUDIF_START) <<
		  SDM439_LPAIF_DMACTL_AUDINTF_SHIFT;

	/*
	 * Words per service cycle. lpass-platform.c's table, non-DP-RX case,
	 * all bit widths: 1 ch -> ONE, 2 ch -> TWO, 4 ch -> FOUR, 6 ch -> SIX,
	 * 8 ch -> EIGHT. Only 1 and 2 are reachable here.
	 */
	wpscnt = (channels == 1) ? SDM439_LPAIF_DMACTL_WPSCNT_ONE :
				    SDM439_LPAIF_DMACTL_WPSCNT_TWO;

	lpaif_rmw(lpaif, ctl, SDM439_LPAIF_DMACTL_AUDINTF_MASK, audintf);
	lpaif_rmw(lpaif, ctl, SDM439_LPAIF_DMACTL_BURSTEN_MASK,
		  SDM439_LPAIF_DMACTL_BURSTEN_MASK);
	lpaif_rmw(lpaif, ctl, SDM439_LPAIF_DMACTL_FIFOWM_MASK,
		  SDM439_LPAIF_DMACTL_FIFOWM_8);
	lpaif_rmw(lpaif, ctl, SDM439_LPAIF_DMACTL_WPSCNT_MASK, wpscnt);

	/* DYNCLK is deliberately left alone: in-tree only touches it for DP
	 * (LPASS_DP_RX) and the CDC DMA ports, neither of which exists here.
	 */

	dev_dbg(cpu_dai->dev,
		"hw_params: %u Hz, %u ch, %d bit, channel %u (%s), AUDINTF %u\n",
		params_rate(params), channels, bitwidth, pcm_data->dma_ch,
		ss->stream == SNDRV_PCM_STREAM_PLAYBACK ? "RDMA" : "WRDMA",
		pcm_data->i2s_port + SDM439_LPAIF_DMACTL_AUDIF_START);

	return 0;
}

static int lpaif_pcmops_hw_free(struct snd_soc_component *component,
				struct snd_pcm_substream *ss)
{
	struct sdm439_lpaif_pcm *pcm_data = lpaif_pcm_data(ss);

	if (!pcm_data)
		return 0;

	lpaif_w32(pcm_data->lpaif,
		  SDM439_LPAIF_DMA_CTL(sdm439_lpaif_dma_block(pcm_data->dma_ch)),
		  0);

	return 0;
}

/*
 * DMABASE, DMABUFF, DMAPER and DMACURR, from the DMA fields of the DAI's
 * dma_params.
 *
 * After snd_pcm_set_fixed_buffer_all() the buffer is described both by
 * substream->dma_params (a struct dma_params) and by substream->dma_addr /
 * substream->dma_len. dma_params is read first, because that is the struct the
 * brief names; the substream fields are the fallback.
 *
 * DMACURR is written in addition to the three lpass-platform.c writes. If the
 * previous stream did not stop cleanly the engine resumes at a stale address
 * and the stream comes up silent; reloading it in .prepare() is the cheapest
 * thing available. lpass-platform.c does not do this; it is a deliberate
 * difference, not an oversight.
 */
static int lpaif_pcmops_prepare(struct snd_soc_component *component,
				struct snd_pcm_substream *ss)
{
	struct sdm439_lpaif_pcm *pcm_data = lpaif_pcm_data(ss);
	struct snd_pcm_runtime *rt = ss->runtime;
	struct sdm439_lpaif *lpaif;
	u32 block, ctl;
	dma_addr_t addr;
	size_t len, buf_bytes, per_bytes;

	if (!pcm_data)
		return -EINVAL;

	lpaif = pcm_data->lpaif;
	block = sdm439_lpaif_dma_block(pcm_data->dma_ch);
	ctl = SDM439_LPAIF_DMA_CTL(block);

	addr = rt->dma_params.dma_addr;
	len = rt->dma_params.dma_len;
	if (!addr) {
		addr = rt->dma_addr;
		len = rt->dma_len;
	}

	if (!addr || !len) {
		dev_err(component->dev,
			"no DMA buffer yet (addr 0x%llx len %zu)\n",
			(unsigned long long)addr, len);
		return -EINVAL;
	}

	buf_bytes = snd_pcm_lib_buffer_bytes(ss);
	per_bytes = snd_pcm_lib_period_bytes(ss);

	if (!buf_bytes || !per_bytes || (buf_bytes & 3u) || (per_bytes & 3u)) {
		dev_err(component->dev,
			"buffer/period size must be a non-zero multiple of 4 (%zu/%zu)\n",
			buf_bytes, per_bytes);
		return -EINVAL;
	}

	lpaif_w32(lpaif, SDM439_LPAIF_DMA_BASE(block), (u32)(u64)addr);
	lpaif_w32(lpaif, SDM439_LPAIF_DMA_BUFF(block),
		  (u32)((buf_bytes >> 2) - 1));
	lpaif_w32(lpaif, SDM439_LPAIF_DMA_PER(block),
		  (u32)((per_bytes >> 2) - 1));
	lpaif_w32(lpaif, SDM439_LPAIF_DMA_CURR(block), (u32)(u64)addr);

	lpaif_rmw(lpaif, ctl, SDM439_LPAIF_DMACTL_ENABLE_MASK,
		  SDM439_LPAIF_DMACTL_ENABLE_MASK);

	dev_dbg(component->dev,
		"%s: dma 0x%llx len %zu, buff %zu per %zu, channel %u\n",
		ss->stream == SNDRV_PCM_STREAM_PLAYBACK ? "playback" : "capture",
		(unsigned long long)addr, len, buf_bytes, per_bytes,
		pcm_data->dma_ch);

	return 0;
}

static int lpaif_pcmops_trigger(struct snd_soc_component *component,
				struct snd_pcm_substream *ss, int cmd)
{
	struct sdm439_lpaif_pcm *pcm_data = lpaif_pcm_data(ss);
	struct sdm439_lpaif *lpaif;
	u32 ctl, mask;

	if (!pcm_data)
		return -EINVAL;

	lpaif = pcm_data->lpaif;
	ctl = SDM439_LPAIF_DMA_CTL(sdm439_lpaif_dma_block(pcm_data->dma_ch));
	mask = SDM439_LPAIF_IRQ_ALL(pcm_data->dma_ch);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		lpaif_rmw(lpaif, ctl, SDM439_LPAIF_DMACTL_ENABLE_MASK,
			  SDM439_LPAIF_DMACTL_ENABLE_MASK);
		lpaif_rmw(lpaif,
			  SDM439_LPAIF_IRQCLEAR_OFF(SDM439_LPAIF_IRQ_PORT_HOST),
			  mask, mask);
		lpaif_rmw(lpaif,
			  SDM439_LPAIF_IRQEN_OFF(SDM439_LPAIF_IRQ_PORT_HOST),
			  mask, mask);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		lpaif_rmw(lpaif, ctl, SDM439_LPAIF_DMACTL_ENABLE_MASK, 0u);
		lpaif_rmw(lpaif,
			  SDM439_LPAIF_IRQEN_OFF(SDM439_LPAIF_IRQ_PORT_HOST),
			  mask, 0u);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/*
 * The SPKEN/MICEN pair, on the platform side. Present so that a platform which
 * reaches this component without the CPU DAI still gets the direction bits
 * handled. Idempotent with .trigger() and with the DAI's .start()/.stop().
 */
static int lpaif_pcmops_start(struct snd_soc_component *component,
			      struct snd_pcm_substream *ss)
{
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(ss->runtime, 0);

	lpaif_i2s_set_dir(lpaif_of(cpu_dai), cpu_dai->driver->id,
			  ss->stream, true);
	return 0;
}

static int lpaif_pcmops_stop(struct snd_soc_component *component,
			     struct snd_pcm_substream *ss)
{
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(ss->runtime, 0);

	lpaif_i2s_set_dir(lpaif_of(cpu_dai), cpu_dai->driver->id,
			  ss->stream, false);
	return 0;
}

static snd_pcm_uframes_t lpaif_pcmops_pointer(struct snd_soc_component *component,
					     struct snd_pcm_substream *ss)
{
	struct sdm439_lpaif_pcm *pcm_data = lpaif_pcm_data(ss);
	struct sdm439_lpaif *lpaif;
	u32 block, base, curr;

	if (!pcm_data)
		return 0;

	lpaif = pcm_data->lpaif;
	block = sdm439_lpaif_dma_block(pcm_data->dma_ch);
	base = lpaif_r32(lpaif, SDM439_LPAIF_DMA_BASE(block));
	curr = lpaif_r32(lpaif, SDM439_LPAIF_DMA_CURR(block));

	return bytes_to_frames(ss->runtime, (u32)(curr - base));
}

/*
 * The only CPU access to the (non-coherent) buffer. Mirrors
 * lpass_platform_copy() in lpass-platform.c, minus the io_remap branch, which
 * only applies to the CDC DMA ports in low-power memory that this driver does
 * not touch.
 */
static int lpaif_pcmops_copy(struct snd_soc_component *component,
			     struct snd_pcm_substream *ss, int channel,
			     unsigned long pos, struct iov_iter *buf,
			     unsigned long bytes)
{
	struct snd_pcm_runtime *rt = ss->runtime;
	void *area = (void *)((char *)rt->dma_area + pos +
			     channel * (rt->dma_bytes / rt->channels));
	int ret = 0;

	if (ss->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		if (copy_from_iter(area, bytes, buf) != bytes)
			ret = -EFAULT;
	} else {
		if (copy_to_iter(area, bytes, buf) != bytes)
			ret = -EFAULT;
	}

	return ret;
}

static struct snd_soc_component_driver sdm439_lpaif_pcm_comp_driver = {
	.name		= "sdm439-lpaif-pcm",
	.pcm_new	= lpaif_pcmops_pcm_new,
	.open		= lpaif_pcmops_open,
	.close		= lpaif_pcmops_close,
	.hw_params	= lpaif_pcmops_hw_params,
	.hw_free	= lpaif_pcmops_hw_free,
	.prepare	= lpaif_pcmops_prepare,
	.trigger	= lpaif_pcmops_trigger,
	.start		= lpaif_pcmops_start,
	.stop		= lpaif_pcmops_stop,
	.pointer	= lpaif_pcmops_pointer,
	.copy		= lpaif_pcmops_copy,
};

/* ------------------------------------------------------------------------ */
/* IRQ. Threaded pair.                                                       */
/* ------------------------------------------------------------------------ */

/*
 * The hard action only wakes the thread. Returning IRQ_WAKE_THREAD makes
 * IRQF_ONESHOT mandatory: the line has to stay masked while the thread runs,
 * otherwise a second period interrupt arrives before the first has been
 * serviced, and LPAIF IRQCLEAR is a write-1-to-clear register, so the earlier
 * event would be lost.
 *
 * The DT name must be "lpass-irq-lpaif". There is deliberately no fallback to a
 * positional interrupt, so a typo in interrupt-names is a loud probe failure
 * instead of a silently wrong line. Upstream asks for IRQF_TRIGGER_RISING on
 * this IRQ (lpass-platform.c:1296) and the DT declares it level-high; that is
 * reproduced here.
 */
static irqreturn_t sdm439_lpaif_irq_hard(int irq, void *data)
{
	return IRQ_WAKE_THREAD;
}

static irqreturn_t sdm439_lpaif_irq_thread(int irq, void *data)
{
	struct sdm439_lpaif *lpaif = data;
	unsigned int irqclear_off, ch, stat;

	irqclear_off	= SDM439_LPAIF_IRQCLEAR_OFF(SDM439_LPAIF_IRQ_PORT_HOST);
	stat		= lpaif_r32(lpaif,
				     SDM439_LPAIF_IRQSTAT_OFF(SDM439_LPAIF_IRQ_PORT_HOST));

	for (ch = 0; ch < SDM439_LPAIF_MAX_DMA_CH; ch++) {
		struct snd_pcm_substream *ss;
		u32 mask = SDM439_LPAIF_IRQ_ALL(ch);

		if (!(stat & mask))
			continue;

		spin_lock_irq(&lpaif->lock);
		ss = lpaif->substream[ch];
		spin_unlock_irq(&lpaif->lock);

		/* The mask is latched in IRQCLEAR even when no stream owns it. */
		lpaif_rmw(lpaif, irqclear_off, mask, mask);

		if (!ss)
			continue;

		if (stat & SDM439_LPAIF_IRQ_PER(ch))
			snd_pcm_period_elapsed(ss);

		if (stat & SDM439_LPAIF_IRQ_XRUN(ch)) {
			dev_warn_ratelimited(lpaif->dev,
					     "xrun on LPAIF channel %u\n", ch);
			snd_pcm_stop_xrun(ss);
		}

		if (stat & SDM439_LPAIF_IRQ_ERR(ch)) {
			dev_err(lpaif->dev,
				"bus access error on LPAIF channel %u\n", ch);
			snd_pcm_stop(ss, SNDRV_PCM_STATE_DISCONNECTED);
		}
	}

	return IRQ_HANDLED;
}

/* ------------------------------------------------------------------------ */
/* Probe / remove.                                                          */
/* ------------------------------------------------------------------------ */

static int sdm439_lpaif_apply_rate_mask(struct device *dev)
{
	int i;

	if (!rates)
		return dev_err(dev, "rates=0 would offer nothing\n");

	for (i = 0; i < SDM439_LPAIF_NUM_DAI; i++) {
		if (sdm439_lpaif_dai_driver[i].playback.channels_max)
			sdm439_lpaif_dai_driver[i].playback.rates = rates;
		if (sdm439_lpaif_dai_driver[i].capture.channels_max)
			sdm439_lpaif_dai_driver[i].capture.rates = rates;
	}

	return 0;
}

static int sdm439_lpaif_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct sdm439_lpaif *lpaif;
	struct resource *res;
	struct dma_config dma_cfg;
	resource_size_t win_end;
	void __iomem *i2sctl_p0, *i2sctl_p2;
	unsigned int i;
	int irq, ret;

	if (!np) {
		dev_err(dev, "no device node\n");
		return -EINVAL;
	}

	lpaif = devm_kzalloc(dev, sizeof(*lpaif), GFP_KERNEL);
	if (!lpaif)
		return -ENOMEM;

	lpaif->dev		= dev;
	lpaif->write_en		= write_en;
	lpaif->wssrc_external	= wssrc_external;
	lpaif->sd_lines		= sd_lines;
	spin_lock_init(&lpaif->lock);

	platform_set_drvdata(pdev, lpaif);

	/*
	 * Same guard as in-tree lpass-cpu.c:
	 *
	 *	dsp_of_node = of_parse_phandle(..., "qcom,adsp", 0);
	 *	if (dsp_of_node) { ... return -EBUSY; }
	 *
	 * A qcom,adsp phandle on this node is an unconditional probe failure.
	 * sd/pine-sound.dtsi deliberately does not write one.
	 */
	if (of_parse_phandle(np, "qcom,adsp", 0)) {
		dev_err(dev,
			"'qcom,adsp' present: the DSP holds the audio resources, refusing to probe\n");
		return -EBUSY;
	}

	/*
	 * Exactly one MMIO region, named "lpass-lpaif". The name is tried
	 * first; a DT with no reg-names at all falls back to the unnamed
	 * region, which is the same single-region call in-tree would make.
	 */
	ret = of_platform_get_num_reg(np, IORESOURCE_MEM);
	if (ret != 1) {
		dev_err(dev, "expected exactly 1 MMIO region, found %d\n", ret);
		return -EINVAL;
	}

	ret = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					   SDM439_LPAIF_REG_NAME, &lpaif->res);
	if (ret) {
		dev_info(dev,
			 "no '%s' in reg-names; using the unnamed region instead\n",
			 SDM439_LPAIF_REG_NAME);
		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
		if (!res)
			return -ENODEV;
		lpaif->res = *res;
	}

	lpaif->win_start	= lpaif->res.start;
	lpaif->win_size		= resource_size(&lpaif->res);

	lpaif->base = devm_ioremap_resource(dev, &lpaif->res);
	if (IS_ERR(lpaif->base))
		return PTR_ERR(lpaif->base);

	win_end		= lpaif->win_start + lpaif->win_size;
	i2sctl_p0	= lpaif->base + SDM439_LPAIF_I2SCTL_OFF(SDM439_LPAIF_MI2S_PRIMARY);
	i2sctl_p2	= lpaif->base + SDM439_LPAIF_I2SCTL_OFF(SDM439_LPAIF_MI2S_TERTIARY);

	/*
	 * Announce the base BEFORE anything else happens, so it can be checked
	 * against /proc/iomem and against devmem2 output before a single
	 * register is touched. This is the most useful line the driver
	 * produces.
	 */
	dev_info(dev,
		 "LPAIF window %pa..%pa (%#zx bytes); I2SCTL primary %p, I2SCTL tertiary %p\n",
		 &lpaif->win_start, &win_end, lpaif->win_size,
		 (void *)i2sctl_p0, (void *)i2sctl_p2);

	if (lpaif->win_size < SDM439_LPAIF_MIN_WIN_SIZE)
		dev_warn(dev,
			 "region is only %#zx bytes; this driver addresses up to +0x%x, so anything above that will be refused\n",
			 lpaif->win_size, SDM439_LPAIF_MIN_WIN_SIZE - 4);

	if (!lpaif->write_en)
		dev_warn(dev,
			 "write_en=0: NO register will ever be written, including the LPAIF IRQ mask\n");

	/*
	 * Clocks. devm_clk_bulk_get() looks each one up by name, so the DT order
	 * is free. A provider that has not registered yet yields -EPROBE_DEFER,
	 * which is returned unchanged: clk-sdm439-oot.ko supplies these seven,
	 * and the moment its probe succeeds really_probe() calls
	 * driver_deferred_probe_trigger() and this probe runs again.
	 */
	for (i = 0; i < (unsigned int)SDM439_LPAIF_NUM_CLKS; i++) {
		lpaif->clks[i].id  = sdm439_lpaif_clk_names[i];
		lpaif->clks[i].clk = NULL;
	}

	ret = devm_clk_bulk_get(dev, SDM439_LPAIF_NUM_CLKS, lpaif->clks);
	if (ret == -EPROBE_DEFER) {
		dev_info(dev,
			 "clocks not ready ('%s' et al); deferring until the SDM439 AON audio clock provider registers\n",
			 sdm439_lpaif_clk_names[0]);
		return -EPROBE_DEFER;
	}
	if (ret) {
		dev_err(dev, "devm_clk_bulk_get failed: %d\n", ret);
		return ret;
	}

	/*
	 * Fabric ports and the AHB/I-X fabric clock stay on for the lifetime of
	 * the probe, as apq8016_lpass_init() does. The four MI2S bit clocks are
	 * deliberately not enabled here; they are per stream.
	 */
	for (i = 0; i < (unsigned int)SDM439_LPAIF_NUM_FABRIC_CLKS; i++)
		clk_prepare_enable(lpaif->clks[sdm439_lpaif_fabric_clk_idx[i]].clk);

	lpaif->ahbix_clk = lpaif->clks[0].clk;

	ret = clk_set_rate(lpaif->ahbix_clk, SDM439_LPAIF_AHBIX_HZ);
	if (ret)
		/* Upstream apq8016_lpass_init() treats this as fatal. */
		dev_warn(dev,
			 "clk_set_rate(ahbix-clk, %lu) failed: %d; continuing\n",
			 SDM439_LPAIF_AHBIX_HZ, ret);

	/*
	 * Interrupt. The DT must name it "lpass-irq-lpaif".
	 */
	irq = platform_get_irq_byname(pdev, SDM439_LPAIF_IRQ_NAME);
	if (irq < 0) {
		dev_err(dev, "no '%s' entry in interrupt-names: %d\n",
			SDM439_LPAIF_IRQ_NAME, irq);
		return irq;
	}
	lpaif->irq = irq;

	/*
	 * iommus. Required by default, and no IOMMU is invented. Without one,
	 * on this SoC, dma_alloc_noncoherent() in pcm_new() would return an
	 * address the LPAIF DMA cannot reach, and that would surface much later
	 * and much less clearly than it does here.
	 *
	 * NOTE for whoever fills in the DT: mainline's own msm8916.dtsi puts NO
	 * iommus on its lpass node, and neither msm8937.dtsi nor sdm439.dtsi
	 * carries dma-mask or dma-ranges anywhere under &soc. So the reference
	 * this driver copies runs with no SMMU in front of the LPAIF DMA at all,
	 * and iommus_required=0 is the way to reproduce that if the correct SMMU
	 * cannot be identified. The default stays strict on purpose.
	 */
	if (!of_property_present(np, "iommus")) {
		if (iommus_required) {
			dev_info(dev,
				 "no 'iommus' property: refusing to invent an IOMMU, deferring\n");
			return -EPROBE_DEFER;
		}

		dev_warn(dev,
			 "no 'iommus' property and iommus_required=0: the LPAIF DMA will use a non-IOMMU address; this is what mainline msm8916.dtsi does\n");
	} else {
		/*
		 * Take the config explicitly. A failure here is a warning, not a
		 * probe error: of_dma_get_config() insists on a usable "dma-mask"
		 * on the node itself, and no node under pine's &soc has one, so
		 * it can legitimately fail on a DT that dma_configure() in
		 * really_probe() handles fine anyway. The OF core has already run
		 * dev->of_dma_configure() before ->probe in that case, so the
		 * explicit call below is guarded on dev->dma_mask being unset.
		 */
		ret = of_dma_get_config(np, &dma_cfg);
		if (ret) {
			dev_warn(dev,
				 "of_dma_get_config failed: %d; relying on the OF core's dma_configure() instead\n",
				 ret);
		} else if (!dev->dma_mask) {
			dev->of_dma_configure(dev, &dma_cfg);
		}
	}

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "dma_set_mask(32) failed\n");

	ret = sdm439_lpaif_apply_rate_mask(dev);
	if (ret)
		return ret;

	/*
	 * The IRQ is requested LAST, once every resource the handler can
	 * dereference is in place.
	 */
	ret = devm_request_irq(dev, irq, sdm439_lpaif_irq_hard,
			       IRQF_TRIGGER_RISING | IRQF_ONESHOT,
			       SDM439_LPAIF_IRQ_NAME, lpaif);
	if (ret) {
		dev_err(dev, "devm_request_irq('%s', %d) failed: %d\n",
			SDM439_LPAIF_IRQ_NAME, irq, ret);
		return ret;
	}

	dev_info(dev,
		 "probed: irq %d, rates 0x%04x, sd_lines %u, wssrc %s, write_en %s\n",
		 irq, rates, sd_lines,
		 wssrc_external ? "EXTERNAL" : "INTERNAL",
		 write_en ? "yes" : "no");

	/*
	 * ORDER MATTERS, and matches in-tree lpass:
	 *   1. the DAI component, which of_xlate_dai_name() resolves the
	 *      &lpass "sound-dai = <&lpass N>" argument against;
	 *   2. the platform component, which qcom_snd_parse_of() reaches via
	 *      link->platforms->of_node == link->cpus->of_node == &lpass.
	 */
	ret = devm_snd_soc_register_component(dev,
					      &sdm439_lpaif_dai_comp_driver,
					      sdm439_lpaif_dai_driver,
					      SDM439_LPAIF_NUM_DAI);
	if (ret) {
		dev_err(dev, "registering the DAI component: %d\n", ret);
		return ret;
	}

	ret = devm_snd_soc_register_component(dev,
					      &sdm439_lpaif_pcm_comp_driver,
					      NULL, 0);
	if (ret) {
		dev_err(dev, "registering the PCM component: %d\n", ret);
		return ret;
	}

	return 0;
}

static int sdm439_lpaif_remove(struct platform_device *pdev)
{
	struct sdm439_lpaif *lpaif = platform_get_drvdata(pdev);
	unsigned int i;

	dev_info(lpaif->dev, "%u register writes skipped (write_en=0)\n",
		 lpaif->skipped_writes);

	for (i = 0; i < (unsigned int)SDM439_LPAIF_NUM_FABRIC_CLKS; i++)
		clk_disable_unprepare(
			lpaif->clks[sdm439_lpaif_fabric_clk_idx[i]].clk);

	return 0;
}

static const struct of_device_id sdm439_lpaif_of_match[] = {
	{ .compatible = "qcom,apq8016-lpass-cpu" },
	{ .compatible = "qcom,lpass-cpu-apq8016" },
	{ }
};
MODULE_DEVICE_TABLE(of, sdm439_lpaif_of_match);

static struct platform_driver sdm439_lpaif_driver = {
	.probe	= sdm439_lpaif_probe,
	.remove	= sdm439_lpaif_remove,
	.driver	= {
		.name		= "sdm439-lpaif-cpu",
		.of_match_table	= sdm439_lpaif_of_match,
	},
};
module_platform_driver(sdm439_lpaif_driver);

MODULE_AUTHOR("grayfox951 <admin@dnr.qzz.io>");
MODULE_DESCRIPTION("SDM439 LPAIF CPU DAI / PCM platform (Redmi 7A, pine)");
MODULE_LICENSE("GPL");
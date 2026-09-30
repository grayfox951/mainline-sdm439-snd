# sdm439-lpaif — out-of-tree LPAIF CPU DAI / PCM platform for Redmi 7A (pine)

SDM439 (Xiaomi Redmi 7A, codename **pine**), kernel
`msm89x7-mainline/linux` @ `msm89x7/7.1.3`, arm64.

> ## THIS HAS NEVER BEEN COMPILED
>
> No kernel build tree is configured in this checkout and none was created.
> Every statement below about kernel behaviour is the result of **reading
> source at that ref**, not of running it. `sdm439-lpaif.c` has not been
> through a compiler, a `checkpatch`, or a reviewer. Treat the first build as
> an experiment.

---

## 1. What this is, and why it is not just `lpass-cpu`

The in-tree driver stack for this block is

| file | role |
| --- | --- |
| `sound/soc/qcom/lpass-cpu.c` | generic LPAIF CPU DAI |
| `sound/soc/qcom/lpass-apq8016.c` | apq8016/msm8916 register map + the four DAI drivers |
| `sound/soc/qcom/lpass-platform.c` | the ALSA↔LPAIF DMA bridge (`pcm_new`, `open`, `hw_params`, `prepare`, `trigger`, `pointer`, the LPAIF IRQ) |
| `sound/soc/qcom/apq8016_sbc.c` | the machine driver pine uses |

All four were fetched from the GitHub contents API at ref `msm89x7/7.1.3` of
`msm89x7-mainline/linux`, together with `sound/soc/qcom/common.c`,
`sound/soc/qcom/lpass.h` and `sound/soc/qcom/lpass-lpaif-reg.h`, and read.
(`fork-lpass-cpu.c` and `fork-lpass-apq8016.c` already in the repository
root are byte-identical to the 7.1.3 copies — verified with `cmp`.)

Those drivers work. The problem is one number: **the LPAIF register base on
SDM439 is in no device tree in any tree.** Not in pine's include chain, not in
`msm8916.dtsi`, not in `sdm439-audio.dtsi`, not in the CAF `sdm439-mtp`/`ctp`
trees, not in the k419 stock vendor tree. So:

* in-tree code cannot be corrected without a kernel rebuild (the base is
  compiled into the DT that ships with the kernel, and there is no in-tree
  place to get it from);
* an out-of-tree module can take it from a DT `reg` property, which means
  **one `dtc`/`dtbo` flash between candidates and no kernel rebuild at all.**

That is the entire justification. The register programming is a
re-expression of what `lpass-platform.c` does, not an improvement on it.

**You must not load this module and the in-tree lpass modules together.**
`sdm439_lpaif_of_match[]` lists the same two compatibles
`lpass-apq8016.c` lists, so both drivers bind the same DT node and race for
the same component names. Pick one set:

```sh
# out-of-tree path (this directory)
modprobe sdm439_lpaif

# or in-tree path (unchanged, and what snd/README.md section 7 describes)
modprobe snd-soc-lpass-cpu
modprobe snd-soc-lpass-apq8016
```

If `modprobe snd-soc-lpass-apq8016` succeeds *and* `sdm439_lpaif` is loaded,
`dev_info` from both will appear for the same node. Unload one before
loading the other; `rmmod` order matters (see §8).

---

## 2. Design decision: option (b), and why option (a) is actively harmful

The brief offered two ways to hand the DAI to the card.

### Option (a) — create a child `pcm` platform node, like `q6apm`/`q6afedai`

**Rejected, because it breaks the card.** The machine driver's parser,
`sound/soc/qcom/common.c: qcom_snd_parse_of()`, does:

```c
if (platform) {
        link->platforms->of_node = of_parse_phandle(platform, "sound-dai", 0);
        ...
} else {
        link->platforms->of_node = link->cpus->of_node;   /* <- the &lpass node */
}

if (codec) {
        ret = snd_soc_of_get_dai_link_codecs(dev, codec, link);
        if (platform) {
                /* DPCM backend */
                link->no_pcm = 1;
                link->ignore_pmdown_time = 1;
        }
}
if (platform || !codec) {
        /* DPCM */
        link->ignore_suspend = 1;
        link->nonatomic = 1;
}
```

Adding a `platform { sound-dai = <...>; };` child to each dai-link therefore
sets `link->no_pcm = 1` and makes it a **DPCM backend**. pine declares no
frontend, so playback would have nowhere to go and the link would be useless.
Option (a) is not "harder", it is wrong.

### Option (b) — one `lpass` node, two components on one device. **Chosen.**

With no `platform` child, `link->platforms->of_node` is the `&lpass` node
itself, so the ASoC core looks for a platform component on the platform device
bound to `&lpass`. In-tree lpass registers both components on that one device,
in this order:

1. `lpass-cpu.c:1263` → `devm_snd_soc_register_component(dev, &lpass_cpu_comp_driver, variant->dai_driver, variant->num_dai)`
2. `lpass-platform.c:1390` → `devm_snd_soc_register_component(&pdev->dev, &lpass_component_driver, NULL, 0)`

`sdm439_lpaif_probe()` reproduces exactly that, in that order. The DT node
stays the same shape as `msm8916.dtsi:557`, so `sd/pine-sound.dtsi`'s dai-links
need no edits.

Consequences worth knowing:

* the node needs `#sound-dai-cells = <1>` and nothing else — no
  `#address-cells`, no `#size-cells`, and **no child nodes**;
* `sd/pine-sound.dtsi` section 3 also defines an `&lpass` node. Use **one or
  the other**, not both; two nodes with the same label will not compile. See
  §6.
* `qcom_snd_parse_of()` also rejects `link->id >= LPASS_MAX_PORT`, and
  `apq8016_sbc.c` switches on `cpu_dai->id`, so the MI2S ids must be the real
  ones. They are: `MI2S_PRIMARY 0`, `MI2S_TERTIARY 2`.

### DAI names registered

| DAI id | `.name` | streams | streams to `mi2s-bit-clkN` |
| --- | --- | --- | --- |
| `MI2S_PRIMARY` (0) | `sdm439-lpaif-primary` | playback + capture | `mi2s-bit-clk0` |
| `MI2S_TERTIARY` (2) | `sdm439-lpaif-tertiary` | capture only | `mi2s-bit-clk2` |

Component names: `sdm439-lpaif-dai` and `sdm439-lpaif-pcm`, both registered on
the `lpass` platform device. The DAI component carries
`.of_xlate_dai_name` so `sound-dai = <&lpass MI2S_PRIMARY>` resolves by
argument, the same way `asoc_qcom_of_xlate_dai_name()` does in
`lpass-cpu.c`.

---

## 3. Master/slave: this DAI is a slave, deliberately

**There is no `set_fmt()`, no `master_capable` and no `slave_capable` anywhere
in the file.** Nothing this driver does can make the DAI claim to be I2S
master. That is required by the brief (the codec divides its own MCLK for
`dmic0_clk`) and it is also what the reference does:

* `qcom_snd_parse_of()` never parses `"dai-format"`, so `link->dai_fmt == 0`;
* `grep -c 'set_fmt' sound/soc/qcom/lpass-cpu.c
  sound/soc/codecs/msm8916-wcd-digital.c sound/soc/codecs/msm8916-wcd-analog.c`
  is `0 0 0` — see `snd/README.md` §1.5, which reaches the same conclusion.

**One honest caveat.** What *is* programmed is `LPAIF_I2SCTL.WSSRC`, set to
`INTERNAL`, matching `lpass_cpu_daiops_hw_params()` and consistent with
`apq8016_sbc.c` writing `SPKR_CTL_PRI_WS_SLAVE_SEL_11` into `spkr-iomux` for
`MI2S_PRIMARY` and `MIC_CTRL_TER_WS_SLAVE_SEL | MIC_CTRL_TLMM_SCLK_EN` into
`mic-iomux` for `MI2S_TERTIARY`. On apq8016/msm8916 that combination means
**LPASS is the WS and bit-clock source**, which reads as the opposite of
"the codec is master". No file available locally settles which reading is
right for SDM439, so it is one bit of module parameter:

```sh
insmod sdm439_lpaif.ko wssrc_external=1     # WSSRC = EXTERNAL
```

**What could not be verified:** how `snd_soc_dai_link_set_dai_fmt()` resolves
master/slave when `link->dai_fmt == 0` and neither side implements
`set_fmt`. `sound/soc/soc-dai.c` is not present in this checkout, so that was
not read. The reasoning above only establishes that *this driver* never
asserts master, which is verifiable and is what the brief asked for.

**Also deviating from the brief, on purpose:** the brief says `SPKMODE none`.
`LPAIF_I2SCTL_SPKMODE_NONE` is `0`, selects no SD line at all, and would make
the port emit nothing. The working reference writes
`LPAIF_I2SCTL_SPKMODE_SD0` / `LPAIF_I2SCTL_MICMODE_SD0` for 1–2 channels, so
that is what this does, with the line selectable:

```sh
insmod sdm439_lpaif.ko sd_lines=1      # SD1 instead of SD0
```

---

## 4. Every DT property this driver needs

The node is `lpaif/dts-fragment.dtsi`. Required:

| property | value | notes |
| --- | --- | --- |
| `compatible` | `"qcom,apq8016-lpass-cpu"` (+ optional second string) | `"qcom,lpass-cpu-apq8016"` also accepted, deprecated in-tree |
| `#sound-dai-cells` | `<1>` | the MI2S port id |
| `reg` | `<BASE 0x10000>` | **BASE IS UNKNOWN**, see §7 |
| `reg-names` | `"lpass-lpaif"` | the name is tried first; a node without `reg-names` falls back to the unnamed region |
| `interrupts` | `<SPI NR IRQ_TYPE_LEVEL_HIGH>` | **NR IS UNKNOWN** (160 is msm8916's) |
| `interrupt-names` | `"lpass-irq-lpaif"` | exact name, no positional fallback |
| `clocks` / `clock-names` | the seven names below | supplied by `clk/dts-fragment.dtsi` |
| `iommus` | SMMU phandle + id | **required** unless `iommus_required=0` |

Clock names, in this exact order to match `clk/dts-fragment.dtsi`:

```
ahbix-clk  mi2s-bit-clk0  mi2s-bit-clk1  mi2s-bit-clk2  mi2s-bit-clk3
pcnoc-mport-clk  pcnoc-sway-clk
```

`devm_clk_bulk_get()` matches by name, so the order is documentation, not ABI.
`mi2s-bit-clk0` and `mi2s-bit-clk1` deliberately name the same PRI_I2S gate,
as `msm8916.dtsi:563-566` does.

Optional, and their defaults:

| property | default | effect |
| --- | --- | --- |
| `qcom,sdm439-lpaif,buffer-size` | `49152` | bytes; `lpass-platform.c`'s value |
| `qcom,sdm439-lpaif,periods` | `2` | at 48 kHz/2ch/S16_LE that is 256 ms of latency; `8` gives 64 ms |

**Must NOT be present:** `qcom,adsp`. `lpass-cpu.c:1111` and this driver both
do `of_parse_phandle(np, "qcom,adsp", 0)` and return `-EBUSY` if it resolves,
because the DSP owns the audio resources. `sd/pine-sound.dtsi` deliberately
omits it.

### Module parameters

| parameter | default | what it does |
| --- | --- | --- |
| `write_en` | `1` | `0` = no register is ever written, including the LPAIF IRQ mask |
| `rates` | `8000\|16000\|32000\|44100\|48000` | `SNDRV_PCM_RATE_*` mask; `64` = 48 kHz only |
| `wssrc_external` | `0` | `1` = `LPAIF_I2SCTL.WSSRC = EXTERNAL` |
| `sd_lines` | `0` | MI2S SD line, `0..3` |
| `iommus_required` | `1` | `0` = warn instead of deferring when `iommus` is absent |

---

## 5. Sample rates and formats

48000/2ch/S16_LE is what this card actually runs at, and the only rate
expected to sound right. The LPAIF bit clock **cannot be reprogrammed** on this
SoC: `clk/gcc-sdm439-oot.c` implements `.set_rate` as a no-op returning 0,
because the RCG2 parents (`GCC_GPLL1`, `GCC_XO_GPLL0_BIMC`) do not exist in
`drivers/clk/qcom/gcc-msm8917.c`. Whatever rate the bootloader left in the
RCG2 is what runs. Playback at 44.1 kHz will therefore come out pitch-shifted,
not broken — `insmod … rates=64` to hide that.

For reference, the *other* machine-driver compatible, `"qcom,msm8916-qdsp6-sndcard"`,
installs `msm8916_qdsp6_be_hw_params_fixup()`, which force-pins every link to
exactly 48000 / 2ch / S16_LE. That string selects the QDSP6 AFE DAIs, which do
not exist in this kernel, so the card uses `"qcom,apq8016-sbc-sndcard"` and
that fixup is **not** installed.

Formats: `S16_LE`, `S24_LE`, `S24_3LE`, `S32_LE` (via
`SNDRV_PCM_FMTBIT_S16|S24|S32`). Channels: 1 or 2 only — that is all the
board's I2S pinmux brings out (`sd/pine-sound.dtsi` §5, gpio69..74), and
`lpass_cpu_daiops_hw_params()` maps anything above 2 onto SD groupings that
pinmux does not reach.

---

## 6. Device tree

Three fragments have to agree. Apply **all** of them:

1. `clk/dts-fragment.dtsi` — the `gcc_sdm439_aon_audio` clock provider node and
   the `&lpass` clock hunk.
2. `snd/pine-sound.dtsi` — `&wcd_codec`, `&lpass_codec`, `&sound` and the
   TLMM states.
3. `lpaif/dts-fragment.dtsi` (this directory) — the `&lpass` node.

**Conflict to resolve first:** `snd/pine-sound.dtsi` §3 defines its own
`lpass:` node, complete with the seven clocks, at `reg = <0x0c008000 0x10000>`
and `interrupts = <GIC_SPI 160 …>`. `lpaif/dts-fragment.dtsi` also defines
`lpass:`. Two nodes with the same label will not compile. Either

* comment out `snd/pine-sound.dtsi` §3 entirely and use this fragment's node
  (recommended: this fragment's `reg` is the loud msm8916 placeholder you must
  edit anyway), or
* keep `sd/pine-sound.dtsi` §3 and delete `&lpass { ... };` from this
  fragment, editing `0x0c008000` there instead. Note that `sd`'s clock order
  differs from this fragment's; that does not matter, `devm_clk_bulk_get()`
  matches by name.

Then:

```sh
cd /path/to/linux
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-android- \
     pine-xiaomi-sdm439-audio.dtb

adb push arch/arm64/boot/dts/qcom/pine-xiaomi-sdm439-audio.dtb /data/local/tmp/
adb shell su -c 'dd if=/data/local/tmp/pine-xiaomi-sdm439-audio.dtb \
    of=/dev/block/by-name/dtbo_b'          # NOT dtbo_a, see §7
```

---

## 7. Finding the register base empirically

This is the open question, and it is the reason this module exists.

### 7.1 What is known, and why it does not settle the question

| block | msm8916 | sdm439 | delta inside AON |
| --- | --- | --- | --- |
| AON base | `0x07700000` | `0x0c000000` | — |
| LPAIF | `AON+0x08000` = `0x07708000` | **unknown** | — |
| sound iomux | `AON+0x02000` = `0x07702000` | `AON+0x51000` = `0x0c051000` | **+0x4f000** |
| WCD digital | `AON+0x1c000` = `0x0771c000` | `AON+0xf0000` = `0x0c0f0000` | **+0xd4000** |

The iomux moved by `+0x4f000` and the WCD digital block by `+0xd4000`. There is
no constant, so "same offset inside AON" is a hypothesis, not a derivation.
Candidates, in order:

1. **`0x0c008000`** = AON + 0x8000. What `snd/pine-sound.dtsi` §3/§6.1 guesses.
2. **`0x0c056000`** = AON + 0x56000, i.e. the SDM439 iomux at `+0x51000` with
   msm8916's 0x3000 iomux→LPAIF delta applied. Pure speculation, listed only so
   it is not re-derived from scratch later.

The `caf/msm8937.dtsi:1603` and `caf/sdm439-pm8953.dtsi:98` node
`qcom,lpass@c200000` is **not** a candidate: its compatible is
`"qcom,pil-tz-generic"`, `reg = <0xc200000 0x00100>`, and it carries
`firmware-name = "adsp"`. It is the PIL proxy-remoteproc node for the LPASS
*subsystem from the DSP's point of view*, 0x100 bytes long, not the LPAIF.

### 7.2 The procedure

`probe/read.sh` in the repository root reads eight words from an address and
classifies the result. Use it; do not type `devmem2` by hand.

**Keep the working DTB on the flash partition unchanged.** A wrong `reg` aims
the driver's writes at whatever really lives at that address — on this SoC that
could be a GCC clock register, an SPMI controller register, or a doorbell — and
any of those can wedge the board hard. `probe/safe-write.sh` exists for this
reason: it records `/proc/uptime` before and after the write, because the
expected failure mode is uptime simply stopping.

```sh
# 1. what does the kernel already think is mapped?
adb shell su -c 'cat /proc/iomem' | grep -iE '0c0|0c1|lpaif|aon'

# 2. read-only probe of a candidate (repeat for each)
adb push probe/read.sh /data/local/tmp/
adb shell su -c 'sh /data/local/tmp/read.sh 0c008000'

# 3. sanity expectations for a real LPAIF base
#    +0x0000 .. +0x0fff  reserved / must not be plain zeroes
#    +0x1000  I2SCTL port 0  - not 0x00000000, not 0xffffffff
#    +0x3000  I2SCTL port 2 (tertiary)
#    +0x8400  RDMA0 DMACTL  - looks like small integers
#    +0x6000  IRQ IRQEN/IRQSTAT/IRQCLEAR
#    +0xb000  WRDMA5 DMACTL
```

For an **unmapped hole** the reads return a constant (`0x00000000` or
`0xffffffff`) or `devmem2` reports an error. That is the signal to move on.

### 7.3 Bring-up order that cannot brick anything

```sh
# 1. clocks first, otherwise this module defers forever
insmod /data/local/tmp/gcc-sdm439-oot.ko

# 2. write-free. probe prints the base; NOTHING is written, not even the
#    LPAIF IRQ mask. Safe with any reg value, including a wrong one.
insmod /data/local/tmp/sdm439_lpaif.ko write_en=0

# 3. read dmesg. This is the line to check.
#    sdm439_lpaif-cpu: LPAIF window 0x.....-0x..... (0x10000 bytes);
#      I2SCTL primary 0x..., I2SCTL tertiary 0x...

# 4. only once the base is confirmed against §7.2's expectations:
insmod /data/local/tmp/sdm439_lpaif.ko
```

`rmmod` prints how many writes were skipped, so `write_en=0` never passes
silently:

```
sdm439_lpaif-cpu: 14 register writes skipped (write_en=0)
```

---

## 8. Build and load

Prerequisites, the same two as every other module in this repository
(`clk/README.md` §4):

```sh
grep -E 'CONFIG_MODULES=|CONFIG_MODVERSIONS' /path/to/linux/.config
#   CONFIG_MODULES=y
#   # CONFIG_MODVERSIONS is not set      <- required: no Module.symvers needed
```

Build (arm64, against a configured and *prepared* tree — `scripts` must have
been run at least once, and there must be a generated `include/linux` etc.):

```sh
cd /data/data/com.termux/files/home/pine-audio/lpaif
make -C /path/to/linux-7.1.3 M=$PWD modules

# clean
make -C /path/to/linux-7.1.3 M=$PWD clean
```

Output: `sdm439_lpaif.ko` plus `.mod`, `.mod.c`, `.symvers`, `.order`,
`modules.order`, `.cache.mk`.

Load:

```sh
adb push gcc-sdm439-oot.ko sdm439_lpaif.ko /data/local/tmp/

adb shell su -c 'insmod /data/local/tmp/gcc-sdm439-oot.ko'
adb shell su -c 'insmod /data/local/tmp/sdm439_lpaif.ko write_en=0'

adb shell su -c 'dmesg | grep -iE "lpaif|lpass|wcd|apq8016"'
```

Unload, in this order (the machine driver must let go of the card first):

```sh
adb shell su -c 'rmmod sdm439_lpaif'
```

`module_platform_driver()` registers the platform driver from `module_init`, so
`insmod` immediately runs `probe`. If the clocks are not ready yet the probe
returns `-EPROBE_DEFER` and is replayed automatically once
`gcc-sdm439-oot.ko` registers its provider — `really_probe()` calls
`driver_deferred_probe_trigger()`.

If you use the built-in path instead (`/etc/modules.d/`), the order from
`snd/README.md` §7 applies, with `sdm439_lpaif` in place of steps 2's
`snd-soc-lpass-cpu` + `snd-soc-lpass-apq8016`.

---

## 9. Fallback plan, if the block really is not there

In increasing order of desperation:

1. **`write_en=0` forever.** The card never registers a PCM, so nothing
   changes. Useful only to confirm the probe path.
2. **External codec over a different transport.** SDM439 does have PCM/I2S
   resources elsewhere (the `msm8937.dtsi` TLMM groups include `pri_mi2s`,
   `sec_mi2s`, `quat_mi2s`, `pdm` and `pri_mi2s_1`), and `snd/sdm439-snd-card.c`
   already exists as a place to hang a board-specific machine driver. Building
   a small CPU DAI on a PL011-style I2S, or bit-banging, is more work than this
   driver but needs no LPASS.
3. **`sound-dw` / generic DMA + the `msm8916-wcd-digital` codec**, if a TI
   I2S block turns out to exist on this SoC. The codec side
   (`msm8916-wcd-digital.c`) is not the problem — it is proven to work on
   msm8916/msm8917 and only needs `ahbix-clk` plus an MCLK, and
   `sd/pine-sound.dtsi` §4 already stubs the MCLK.
4. **Accept analog-only playback through the stock ROM path.** If nothing works,
   the honest outcome is a device that still plays audio via the stock path and
   gains nothing. Say so rather than shipping a module that probes and produces
   silence.
5. **If the block exists but the DMA is behind an SMMU whose id is unknown**,
   the fix is a single `iommus = <&apps_iommu N>;` line, not a rewrite. That
   is why `iommus_required=0` exists rather than a hard failure.

Do **not** "fix" silence by adding register writes. `write_en=0` exists
precisely so that the difference between "wrong address" and "wrong register
layout" stays distinguishable.

---

## 10. What was verified, and what was not

### Verified by reading source at `msm89x7/7.1.3`

* the machine driver's parser: `qcom_snd_parse_of()` in
  `sound/soc/qcom/common.c`, including `link->platforms->of_node =
  link->cpus->of_node` and the `link->no_pcm = 1` DPCM rule that rules out
  option (a);
* `apq8016_sbc.c`'s `of_device_id`, the `cpu_dai->id` switch, the iomux
  programming, the jack, and `msm8916_qdsp6_be_hw_params_fixup`;
* the whole apq8016 LPAIF register map (`apq8016_data`), the DMA channel
  numbering, `dmactl_audif_start`, and the `playback→RDMA / capture→WRDMA`
  sense (including the seemingly-inverted `__LPAIF_DMA_REG` macro);
* `lpass-platform.c`'s pcm hardware caps (24·2·1024 bytes, 2 periods),
  `open`/`close`/`hw_params`/`hw_free`/`prepare`/`trigger`/`pointer`/`copy`,
  `pcm_new`, the IRQ request and the IRQSTAT/IRQCLEAR discipline;
* `lpass-cpu.c`'s I2SCTL field programming, `clk_set_rate()` on
  `mi2s-bit-clkN`, the `qcom,adsp` refusal, `of_xlate_dai_name`, and the
  two-components-on-one-device order;
* `lpass.h`'s `LPASS_AHBIX_CLOCK_FREQUENCY` and `LPASS_MAX_DMA_CHANNELS`;
* `include/dt-bindings/sound/qcom,lpass.h`:
  `MI2S_PRIMARY 0`, `MI2S_TERTIARY 2`;
* `include/dt-bindings/sound/apq8016-lpass.h` — it only includes
  `qcom,lpass.h`, it defines nothing of its own;
* `k419/arch/arm64/boot/dts/qcom/msm8916.dtsi:557-581` (the `lpass` node) and
  `apq8016-sbc.dtsi` (the dai-links and the `sound` node);
* `clk/gcc-sdm439-oot.c` and `clk/dts-fragment.dtsi` — the seven clock names,
  their order, the six CBCR offsets, and that `clk_set_rate()` returns 0;
* `sd/pine-sound.dtsi`, `snd/README.md`, `clk/README.md`, `aw87329/README.md`
  for the iomux addresses, the `lpass_codec` address, the codec supply names,
  the WCD interrupt names, the rate/format situation, and the convention that
  `&sound` must contain nothing but dai-links.

### NOT verified — no local source, or no way to know

* **anything in `sound/soc/core/`** — `soc-dai.c`, `soc-component.c`,
  `soc-pcm.c`, `of.c` are not in this checkout. Concretely this means:
  * `snd_soc_component_get_drvdata()` == `dev_get_drvdata(dev)`: inferred from
    `lpass-platform.c` using that accessor on components registered from
    `&pdev->dev`, not read from `soc-component.c`;
  * whether `struct snd_soc_component_driver` still carries `.start`/`.stop`
    (it does on every kernel this was written against, and both are provided
    here in any case, idempotently, alongside `.trigger`);
  * how `snd_soc_dai_link_set_dai_fmt()` resolves master/slave with
    `link->dai_fmt == 0`;
* **every kernel symbol not present in one of the six qcom files or the two
  codec drivers**, i.e. the plain core API: `devm_platform_ioremap_resource*`,
  `devm_ioremap_resource`, `devm_clk_bulk_get`, `platform_get_resource_byname`,
  `of_platform_get_num_reg`, `of_dma_get_config`, `dev->of_dma_configure`,
  `dma_set_mask_and_coherent`, `devm_request_irq`, `snd_pcm_set_fixed_buffer_all`,
  `snd_pcm_lib_buffer_bytes`, `snd_pcm_lib_period_bytes`, `bytes_to_frames`,
  `snd_soc_rtd_to_cpu`, `snd_soc_component_get_drvdata`,
  `devm_snd_soc_register_component`, `copy_from_iter`, `copy_to_iter`.
  None of them is second-guessed by this driver, but none was read either;
* **`LPASS_MAX_PORT`**, the bound `qcom_snd_parse_of()` enforces on
  `link->id`. It lives in `sound/soc/qcom/common.h`, which was not fetched.
  Irrelevant here (the largest id this driver accepts is 2) but not confirmed;
* **the SDM439 LPAIF base address.** Not in any tree. See §7;
* **the SDM439 LPAIF GIC SPI.** 160 is msm8916's value, carried over as a
  placeholder;
* **the SDM439 LPAIF internal register layout.** The apq8016 map is used as a
  hypothesis; it is not confirmed against any SDM439 documentation, and no SDM439
  description of this block is available locally;
* **which SMMU (if any) the LPAIF DMA goes through on SDM439.** Neither
  `apps_iommu` nor `adreno_smmu` has been confirmed, and their secure-id spaces
  differ between `msm8937.dtsi` and `msm8916.dtsi`;
* **whether `codec is master` or `LPASS is master`** on this board. §3;
* **the SDM439 MI2S bit-clock rate.** `clk/gcc-sdm439-oot.c` cannot program the
  RCG2s, so `clk_set_rate()` is a no-op and the rate is whatever the bootloader
  left;
* **`msm8937.dtsi`/`sdm439.dtsi` `dma-mask`/`dma-ranges`.** Grepped: none under
  `&soc`. That is why `of_dma_get_config()` failure is a warning here rather
  than a probe error;
* **whether the DT compiles.** `dts-fragment.dtsi` was not run through `cpp` +
  `dtc` here; no kernel tree or dtc invocation was performed. The label
  conflicts in §6 are real and are called out, but the file itself is unbuilt.

### A file the brief asked to read that does not exist

`pine-audio/ANALYSIS-pdm-path.md` is not in this repository. `find` over the
whole tree returns nothing of that name. Nothing in this module depends on it,
but if it existed and contained a different address or a different analysis,
§7 and §10 are where the reconciliation would go.
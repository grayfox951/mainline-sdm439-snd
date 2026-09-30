# Sound card for Redmi 7A (pine) / SDM439

Target kernel: `msm89x7-mainline/linux` @ `msm89x7/7.1.3`, arm64.

| File | What it is |
|---|---|
| `snd/msm439-snd-card.c` | Speaker-amplifier supervisor. **Not** an ASoC machine driver — see §1. |
| `snd/pine-sound.dtsi` | The whole sound section: WCD analog codec, LPASS, WCD digital codec, sound card, amp-supervisor node. |
| `snd/README.md` | This file. |

All paths below are relative to the kernel source tree unless prefixed with
`fk/` (the partial clone at `/data/data/com.termux/files/home/pine-audio/fk/`).

---

## 0. Include chain — read this before touching the DT

```
sdm439-xiaomi-pine.dts
  └─ sdm439.dtsi
       └─ msm8937.dtsi          <-- does NOT include msm8916.dtsi
  └─ pm8953.dtsi                <-- does NOT include pm8937.dtsi
  └─ pmi632.dtsi
```

Consequences that drive every decision in `pine-sound.dtsi`:

| Fact | Evidence |
|---|---|
| `msm8937.dtsi` does not include `msm8916.dtsi` | `grep -n 'msm8916.dtsi' fk/arch/arm64/boot/dts/qcom/msm8937.dtsi` → no output |
| Therefore no `lpass:` label exists for pine | only `lpass:` in the fork is `fk/arch/arm64/boot/dts/qcom/msm8916.dtsi:2125` |
| `sound:` and `lpass_codec:` come from `msm8937.dtsi`, not `msm8916.dtsi` | `msm8937.dtsi:2340`, `:2353` |
| `pm8953.dtsi` does not include `pm8937.dtsi`, so `wcd_codec` is **not** visible | the only `wcd_codec` on a pine-visible path is `pm8937.dtsi:158` |
| ⇒ `&lpass` and `wcd_codec` are **created** by `pine-sound.dtsi` | §3 and §2 of the dtsi |
| `msm8937.dtsi` includes `qcom,gcc-msm8917.h`, **not** `qcom,gcc-msm8916.h` | `msm8937.dtsi:7` |

Verify:

```sh
grep -n 'msm8916.dtsi' fk/arch/arm64/boot/dts/qcom/msm8937.dtsi     # empty
grep -n 'pm8937.dtsi'  fk/arch/arm64/boot/dts/qcom/pm8953.dtsi     # empty
grep -nE '^\s+(lpass|lpass_codec|sound):' \
	fk/arch/arm64/boot/dts/qcom/msm8937.dtsi                     # lpass_codec, sound
grep -rn 'lpass:' fk/arch/arm64/boot/dts/qcom/*.dtsi                # msm8916.dtsi only
```

---

## 1. Is `apq8016_sbc.c` reusable? — **Yes. No variant is written.**

`sound/soc/qcom/apq8016_sbc.c` is used as-is with
`compatible = "qcom,apq8016-sbc-sndcard"`.

Six independent reasons, all verified against the fork:

1. **The right `of_device_id` entry already exists** and the driver dispatches on
   it via `device_get_match_data()`:

   ```c
   { .compatible = "qcom,apq8016-sbc-sndcard", .data = apq8016_sbc_add_ops },
   { .compatible = "qcom,msm8916-qdsp6-sndcard", .data = msm8916_qdsp6_add_ops },
   ```

   `apq8016_sbc_add_ops()` is *only*:

   ```c
   for_each_card_prelinks(card, i, link)
           link->init = apq8016_sbc_dai_init;
   ```

   `msm8916_qdsp6_add_ops()` — the variant that sets
   `card->components = "qdsp6"` and pins every link to 48 kHz/2ch/`S16_LE` for
   the absent QDSP6 AFE DAIs — is never selected. Nothing AFE- or
   q6apm-related is reached at runtime. (The file does include
   `qdsp6/q6afe.h` and `dt-bindings/sound/qcom,q6afe.h`; those are headers
   only, and the q6apm code they belong to is not in this kernel at all.)

2. **Both needed DAI ids are handled.** `apq8016_dai_init()` switches on
   `cpu_dai->id` and covers `MI2S_PRIMARY` (writes
   `SPKR_CTL_PRI_WS_SLAVE_SEL_11` into `spkr-iomux`) and `MI2S_TERTIARY`
   (writes `MIC_CTRL_TER_WS_SLAVE_SEL | MIC_CTRL_TLMM_SCLK_EN` into
   `mic-iomux`). Those are the only two LPASS DAI ids this board uses.
   `MI2S_QUINARY` is present in the driver but `lpass-apq8016.c` registers no
   DAI for it, so that branch is dead on this SoC anyway.

3. **The iommux registers are already at the pine addresses.**
   `apq8016_sbc_platform_probe()` maps `mic-iomux` and `spkr-iomux` by name via
   `devm_platform_ioremap_resource_byname()` and returns early if either is
   missing. `msm8937.dtsi:2340` already supplies:

   ```
   mic-iomux   0x0c051000   REQUIRED
   spkr-iomux  0x0c051004   REQUIRED
   pri-iomux   0x0c055000   present, unused by the driver
   quin-iomux  0x0c052000   optional, only read on MI2S_QUINARY
   ```

   Since the driver writes those registers directly, the `reg` list is left
   byte-identical to what `msm8937.dtsi` defines.

4. **The jack is created by the driver**, so the DT adds no jack widgets.
   `apq8016_dai_init()` calls
   `snd_soc_card_jack_new_pins(card, "Headset Jack", SND_JACK_HEADSET |
   SND_JACK_HEADPHONE | SND_JACK_BTN_0..4, ...)`.

5. **No `dai-format`, no `set_fmt` is needed or possible.**
   `qcom_snd_parse_of()` never parses `dai-format`. Neither `lpass-cpu`,
   `msm8916-wcd-analog` nor `msm8916-wcd-digital` declares
   `master_capable`/`slave_capable` or a `set_fmt` op (grep: zero hits for
   `set_fmt` in all three). `lpass_cpu_daiops_hw_params()` instead hard-codes
   `LPAIF_I2SCTL_WSSRC_INTERNAL` and programs `LPAIF_I2SCTL_BITWIDTH` /
   `SPKMODE` / `MICMODE` itself, and sets
   `clk_set_rate(mi2s_bit_clk[id], rate * bitwidth * 2)`. LPASS is therefore
   always the bit-clock and word-select source, and `dai_fmt == 0` is correct.
   Every working msm8916 board in this kernel (`msm8916-huawei-g7.dts`,
   `msm8916-acer-a1-724.dts`, `apq8016-sbc.dts`) runs that way. Adding a
   `set_fmt` call would change nothing and would risk disagreeing with the
   iomux programming above.

6. **`lpass-apq8016.c` has no `MI2S_QUINARY` DAI and no mandatory sd-line
   property.** `of_lpass_cpu_parse_dai_data()` defaults every port to
   `LPAIF_I2SCTL_MODE_8CH`, which `lpass_cpu_daiops_hw_params()` collapses to
   `SD0` for 1–2 channels. `qcom,playback-sd-lines` / `qcom,capture-sd-lines`
   are therefore optional here.

```sh
grep -n 'of_device_id apq8016_sbc_device_id' -A 5 \
	sound/soc/qcom/apq8016_sbc.c
grep -n 'apq8016_sbc_add_ops' -A 8 sound/soc/qcom/apq8016_sbc.c
grep -n 'case MI2S_' sound/soc/qcom/apq8016_sbc.c
grep -c 'set_fmt\|master_capable\|slave_capable' \
	sound/soc/qcom/lpass-cpu.c \
	sound/soc/codecs/msm8916-wcd-analog.c \
	sound/soc/codecs/msm8916-wcd-digital.c     # 0 0 0
grep -n 'dai-format' sound/soc/qcom/common.c  # no output
```

### 1b. What `msm439-snd-card.c` actually is

One board-specific gap has no in-tree home: **the speaker amplifier**.

The AW87329 is an **analog-input** class-K PA on `blsp1_i2c2` at `0x58`, ported
as its own out-of-tree module (`aw87329/aw87329.c`). It carries no I2S, so it
is correctly **not** a codec and **not** a DAI, and `pine-sound.dtsi` does not
model it as one. It *does* have to be switched in step with the WCD's speaker
path, and the only hook that fires at exactly the right moment is
`pm8916_wcd_analog_enable_spk_pa()` in
`sound/soc/codecs/msm8916-wcd-analog.c` — an in-tree file an out-of-tree
module cannot patch.

`msm439-snd-card.c` supplies only that missing coupling:

* binds a plain platform device `qcom,msm439-snd-card` (no DAI, no codec, no
  AFE reference);
* finds the amplifier through a DT phandle (`amp-phandle = <&aw87329>`) using
  `of_property_read_phandle()` + `of_find_device_by_phandle()` — the AW87329
  node stays owned by its own driver;
* calls `aw87329_spk_enable()` / `aw87329_spk_disable()`, resolved with
  `__symbol_get()` so there is no link-time dependency and the module builds,
  loads and unloads with or without `aw87329.ko`;
* exports `msm439_spk_amp_enable()` / `msm439_spk_amp_disable()` so a future
  in-tree patch to `pm8916_wcd_analog_enable_spk_pa()` can call them from
  `SND_SOC_DAPM_POST_PMU` / `SND_SOC_DAPM_POST_PMD` and get the ordering right;
* exposes `spk_amp_enable` in sysfs for use until that patch exists.

It duplicates nothing: the AW87329 register protocol lives only in `aw87329.c`.

**One change was needed outside `snd/`:** `aw87329/dts-fragment.dtsi` now labels
its node `aw87329: aw87329@58`, because `pine-sound.dtsi` references it as
`&aw87329`. A bare node name is not referenceable from another dtsi. Labels are
inert, so this costs nothing if nothing refers to it. **If that fragment is not
included, `pine-sound.dtsi` will not compile** — the `msm439_snd_card` node and
its `amp-phandle` are the only thing that depends on it, so dropping §6 of the
dtsi along with the fragment is the alternative.

---

## 2. DAI links

Both links carry **two codecs**: `lpass_codec` is the serial endpoint,
`wcd_codec` is the codec that owns the jack, the headphone MUXes and
`set_jack`. `apq8016_dai_init()` loops `for_each_rtd_codec_dais()` calling
`set_sysclk()` and `set_jack()` on each, so the analog codec has to be in the
link for the jack to work.

| # | `link-name` | CPU DAI | Codecs | Direction |
|---|---|---|---|---|
| 1 | `WCD-Playback` | `&lpass MI2S_PRIMARY` | `&lpass_codec 0` (`AIF1 Playback`), `&wcd_codec 0` (`pm8916_wcd_analog_pdm_rx`) | playback |
| 2 | `WCD-Capture` | `&lpass MI2S_TERTIARY` | `&lpass_codec 1` (`AIF1 Capture`), `&wcd_codec 1` (`pm8916_wcd_analog_pdm_tx`) | capture |

Direction is decided by the DAI ids: `lpass-apq8016.c` registers
`MI2S_TERTIARY` as **capture-only** and `MI2S_PRIMARY` as **playback-only**, so
one link covers each direction on its own and no `direction` property is needed.

### Why the DMIC needs `lpass_codec`, not `wcd_codec`

`pm8916_wcd_analog_pdm_tx` (id 1) is a **misnomer**. It is the WCD analog
codec's generic serial *output*, and it reaches `ADC1`/`ADC2`/`ADC3` — the
**analog** ADC block. It contains zero DMIC registers. Decisive:

```sh
grep -c DMIC sound/soc/codecs/msm8916-wcd-analog.c          # 0
grep -c DMIC sound/soc/codecs/msm8916-wcd-digital.c         # 42
```

The real chain is in `msm8916-wcd-digital.c`:
`DMIC1 → DEC1 MUX → CIC1 MUX → I2S TX1 → AIF1 Capture → dai id 1`, and only
`msm8916_wcd_digital_dai[]` has an `.ops` that programs
`LPASS_CDC_CLK_TX_I2S_CTL` FS rate and word width. So **digital-mic capture
requires `lpass_codec` to probe**, and it still does **not** require the QDSP6
AFE.

### `qcom_snd_parse_of()` constraint — read before adding anything to `&sound`

```c
num_links = of_get_available_child_count(dev->of_node);
for_each_available_child_of_node(dev->of_node, np) { /* make a dai_link */ }
```

**Every child node of `&sound` that is not `status = "disabled"` becomes a
dai_link.** Each needs `link-name`, a `cpu` node with `sound-dai`, and a `codec`
(or `platform`) node. Adding a `widgets` node, an amplifier node or anything
else inside `&sound` breaks the card. That is why `msm439_snd_card` is a
sibling of `&sound` under `&soc`.

The properties it *does* parse are: `model` (with `qcom,model` as a fallback),
`widgets`, `audio-routing` (with `qcom,audio-routing` as a fallback),
`pin-switches`, `aux-devs`. `pine-sound.dtsi` uses `model` and `audio-routing`.

---

## 3. Widget names — every one grep-verified

### `audio-routing` in `pine-sound.dtsi`

| Name | Defined in | Kind |
|---|---|---|
| `AMIC1` | `msm8916-wcd-analog.c` | `SND_SOC_DAPM_INPUT` |
| `AMIC2` | `msm8916-wcd-analog.c` | `SND_SOC_DAPM_INPUT` |
| `AMIC3` | `msm8916-wcd-analog.c` | `SND_SOC_DAPM_INPUT` |
| `MIC BIAS Internal1` | `msm8916-wcd-analog.c` | `SND_SOC_DAPM_SUPPLY` |
| `MIC BIAS Internal2` | `msm8916-wcd-analog.c` | `SND_SOC_DAPM_SUPPLY` |
| `MIC BIAS External1` | `msm8916-wcd-analog.c` | `SND_SOC_DAPM_SUPPLY` |
| `Handset Mic` | `apq8016_sbc.c` card widget | `SND_SOC_DAPM_MIC` |
| `Headset Mic` | `apq8016_sbc.c` card widget | `SND_SOC_DAPM_MIC` |
| `Secondary Mic` | `apq8016_sbc.c` card widget | `SND_SOC_DAPM_MIC` |
| `Mic Jack` | `apq8016_sbc.c` card widget | `SND_SOC_DAPM_MIC` |
| `DMIC1` | `msm8916-wcd-digital.c` | `SND_SOC_DAPM_ADC_E` |
| `DMIC2` | `msm8916-wcd-digital.c` | `SND_SOC_DAPM_ADC_E` |

```sh
for w in AMIC1 AMIC2 AMIC3 "MIC BIAS Internal1" "MIC BIAS Internal2" \
         "MIC BIAS External1" "Handset Mic" "Headset Mic" "Secondary Mic" \
         "Mic Jack" DMIC1 DMIC2; do
	printf '%-22s' "$w"
	grep -lc "\"$w\"" sound/soc/codecs/msm8916-wcd-analog.c \
		sound/soc/codecs/msm8916-wcd-digital.c \
		sound/soc/qcom/apq8016_sbc.c
done
```

### Deliberately **not** routed

* **`SPK_OUT`** — a codec `SND_SOC_DAPM_OUTPUT` with no codec-side sink. The
  AW87329 is fed from it but carries no I2S; modelling it as a card widget or a
  DAI would be wrong. It is driven from `msm439-snd-card.c` instead (§1b).
* **`HPH_L` / `HPH_R` / `EAR`** — no upstream msm8916 board in this kernel
  routes the headphone or earpiece jack; the WCD's own `hphl`/`hphr` MUX and
  its MBHC state machine do it. Adding routes would only add unverified paths.
* **`Digital Mic1` / `Digital Mic2`** — a **name collision**. Both
  `msm8916-wcd-digital.c:808-809` **and** `apq8016_sbc.c` register identically
  named card `SND_SOC_DAPM_MIC` widgets. That leaves two widgets with the same
  name on one card, and `dapm_get_widget()` returns whichever was registered
  first, so a route to that name attaches to an arbitrary one of the two.
  `DMIC1 → MIC BIAS External1` is used instead, matching
  `msm8916-acer-a1-724.dts`.

**Any unknown name in `audio-routing` fails the whole card probe**, so these
twelve are not cosmetic:

```
qcom_snd_parse_of() -> snd_soc_of_parse_audio_routing() -> snd_soc_dapm_add_route()
   -> dapm_get_widget() returns NULL -> error propagates out of qcom_snd_parse_of()
```

Full verified widget inventory (for auditing):

```sh
grep -oE 'SND_SOC_DAPM_[A-Z_0-9]+\("[^"]+"' sound/soc/codecs/msm8916-wcd-analog.c \
	| sed 's/.*("//; s/"$//' | sort -u
grep -oE 'SND_SOC_DAPM_[A-Z_0-9]+\("[^"]+"' sound/soc/codecs/msm8916-wcd-digital.c \
	| sed 's/.*("//; s/"$//' | sort -u
grep -oE 'SND_SOC_DAPM_[A-Z_0-9]+\("[^"]+"' sound/soc/qcom/apq8016_sbc.c \
	| sed 's/.*("//; s/"$//' | sort -u
```

The analog codec's playback graph is **already complete on its own** — no card
routes are required for it to reach the outputs:

```
PDM Playback → PDM_RX1/2/3
  PDM_RX3 → SPK DAC → SPK PA → SPK_OUT            (speaker)
  PDM_RX1 → HPHL DAC → HPHL PA → HPH_L            (left headphone)
  RDAC2 MUX ← PDM_RX1/PDM_RX2 → HPHR DAC → HPHR PA → HPH_R
  HPHL/HPHR DAC → EAR PA → EAR                    (earpiece)
```

---

## 4. The WCD analog codec: two places this DT deviates from stock

### 4a. Supply names — **stock DT would fail probe**

Stock `sdm439-audio.dtsi` uses the CAF names (`cdc-vdd-io-supply`,
`cdc-vdda-cp-supply`, `cdc-vdd-pa-supply`, `cdc-vdd-mic-bias-supply`).
`msm8916-wcd-analog.c` reads **none** of those. Its only regulator request is:

```c
static const char * const supply_names[] = {
	"vdd-cdc-io",
	"vdd-cdc-tx-rx-cx",
};
...
ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(priv->supplies), priv->supplies);
if (ret) {
	dev_err(dev, "Failed to get regulator regulator supplies %d\n", ret);
	return ret;                         /* <-- probe dies */
}
```

`devm_regulator_bulk_get()` looks up `"<supply_name>-supply"`, so the node needs
`vdd-cdc-io-supply` and `vdd-cdc-tx-rx-cx-supply`. The mainline spelling is
taken from the closest sibling that already works on this driver,
`fk/arch/arm64/boot/dts/qcom/msm8917-xiaomi-wingtech.dtsi:537`:

```
vdd-cdc-io-supply       = <&pm8937_l5>;
vdd-cdc-tx-rx-cx-supply = <&pm8937_s4>;
```

Rail mapping for pine (labels verified in `sdm439-xiaomi-pine.dts`):

| Driver supply | Pine rail | Voltage | Stock CAF equivalent |
|---|---|---|---|
| `vdd-cdc-io` | `pm8953_l5` | 1.80 V fixed | `cdc-vdd-io` |
| `vdd-cdc-tx-rx-cx` | `pm8953_s4` | 1.90–2.04 V | `cdc-vdda-cp` (and `cdc-vdd-pa`, no consumer) |
| `vdd-micbias` (not requested; recorded) | `pm8953_l13` | 3.05–3.10 V | `cdc-vdd-mic-bias` |

The CAF-named properties are kept in `pine-sound.dtsi` as documentation and
downstream parity; the mainline driver ignores them.

### 4b. Interrupt names — three of the fourteen must differ

The driver looks these up **by name** in its probe and has no fallback:

```c
irq = platform_get_irq_byname(pdev, "mbhc_switch_int");
if (irq < 0)
	return irq;
...
if (priv->mbhc_btn_enabled) {
	irq = platform_get_irq_byname(pdev, "mbhc_but_press_det");
	if (irq < 0)
		return irq;
	irq = platform_get_irq_byname(pdev, "mbhc_but_rel_det");
	if (irq < 0)
		return irq;
}
```

| Stock `sdm439-audio.dtsi` | Used in `pine-sound.dtsi` | Why |
|---|---|---|
| `mbhc_int` | **`mbhc_switch_int`** | the name the driver looks up |
| `but_press_det` | **`mbhc_but_press_det`** | ditto |
| `but_rel_det` | **`mbhc_but_rel_det`** | ditto |

(`pm8937.dtsi:158` already used `mbhc_but_rel_det` / `mbhc_but_press_det` but
still had the wrong `cdc_switch_int`, so that node would not probe either.)

`mbhc_btn_enabled` is true only if **both** `qcom,mbhc-vthreshold-low` and
`qcom,mbhc-vthreshold-high` are present 5-element arrays. Both are supplied, so
button detection stays on. The other eleven interrupt names are labels only
(`platform_get_irq()` is positional) and are byte-identical to stock.

### 4c. No `clocks` on this node

`pm8937.dtsi:158` hangs `wcd_codec` off `&q6afecc`, compatible
`"qcom,q6afe-clocks"`, for which **no driver exists in this kernel**. But
`msm8916-wcd-analog.c` contains **no `clk_get()` at all** — it never touches a
clock — so no `clocks` property is needed and none is supplied.

```sh
grep -n 'clk_get\|clk_prepare\|clk_set_rate' sound/soc/codecs/msm8916-wcd-analog.c   # empty
grep -rn 'q6afe-clocks' --include=*.c --include=*.h drivers/ sound/                    # empty
```

---

## 5. Clocks — verified vs. TODO

### Verified tokens (all in `fk/include/dt-bindings/clock/qcom,gcc-sdm439.h`)

Used by `&lpass`:

| `clock-name` | Token | Header line | Gate in `gcc-sdm439.c` |
|---|---|---|---|
| `pcnoc-mport-clk` | `GCC_ULTAUDIO_PCNOC_MPORT_CLK` | 17 | `:3584` |
| `pcnoc-sway-clk` | `GCC_ULTAUDIO_PCNOC_SWAY_CLK` | 18 | `:3600` |
| `ahbix-clk` | `GCC_ULTAUDIO_AHBFABRIC_IXFABRIC_CLK` | 21 | `:3499` |
| `mi2s-bit-clk0` | `GCC_ULTAUDIO_LPAIF_PRI_I2S_CLK` | 23 | `:3550` |
| `mi2s-bit-clk1` | `GCC_ULTAUDIO_LPAIF_PRI_I2S_CLK` | 23 | `:3550` |
| `mi2s-bit-clk2` | `GCC_ULTAUDIO_LPAIF_SEC_I2S_CLK` | 24 | `:3567` |
| `mi2s-bit-clk3` | `GCC_ULTAUDIO_LPAIF_AUX_I2S_CLK` | 25 | `:3533` |

`mi2s-bit-clk0` and `mi2s-bit-clk1` deliberately share `PRI_I2S_CLK`; that is
what `msm8916.dtsi:2136-2137` does and it is not a typo.
`mi2s-osr-clk0..3` are **not** listed — `lpass-cpu.c:1224` uses
`devm_clk_get_optional()` for them.

Used by `&lpass_codec`:

| `clock-name` | Token | Status |
|---|---|---|
| `ahbix-clk` | `GCC_ULTAUDIO_AHBFABRIC_IXFABRIC_CLK` | verified |
| `mclk` | — | **TODO, no provider exists** |

```sh
for t in GCC_ULTAUDIO_PCNOC_MPORT_CLK GCC_ULTAUDIO_PCNOC_SWAY_CLK \
         GCC_ULTAUDIO_AHBFABRIC_IXFABRIC_CLK GCC_ULTAUDIO_LPAIF_PRI_I2S_CLK \
         GCC_ULTAUDIO_LPAIF_SEC_I2S_CLK GCC_ULTAUDIO_LPAIF_AUX_I2S_CLK \
         GCC_CODEC_DIGCODEC_CLK; do
	printf '%-40s %s\n' "$t" \
		"$(grep -c "^#define $t" fk/include/dt-bindings/clock/qcom,gcc-sdm439.h)"
done
```

### TODO (blocking, for the clock agent)

1. **`gcc-sdm439.o` is not in `drivers/clk/qcom/Makefile`.** Only
   `obj-$(CONFIG_MSM_GCC_8917) += gcc-msm8917.o` is there (line 69).
   `gcc-msm8917.c` *does* match `"qcom,gcc-sdm439"` (`:4179`, bound to
   `gcc_sdm439_desc` at `:4096`), but `grep -c ultaudio
   drivers/clk/qcom/gcc-msm8917.c` is **0** — that table has no ULTAUDIO gates.
   `gcc-sdm439.c` *does* implement all six ULTAUDIO gates and registers the same
   compatible (`:4418`). Until the Makefile line is added, **even `ahbix-clk`
   is dead.**

   ```sh
   grep -n 'gcc-sdm439\|gcc-msm8917' drivers/clk/qcom/Makefile
   grep -c ultaudio drivers/clk/qcom/gcc-msm8917.c    # 0
   grep -c 'qcom,gcc-sdm439' drivers/clk/qcom/gcc-sdm439.c   # >0
   ```

2. **`lpass_codec`'s `mclk` has no provider.**
   `msm8916-wcd-digital.c` requires `ahbix-clk` and `mclk` by name and does
   `clk_prepare_enable()` on both, so probe fails without them. But
   `GCC_CODEC_DIGCODEC_CLK` is **not** in `qcom,gcc-sdm439.h` — the ULTAUDIO
   block stops at index 158, one short of the msm8916 value of 159
   (`qcom,gcc-msm8916.h:168`) — and `gcc-sdm439.c` contains **no** digcodec
   gate at all. `pine-sound.dtsi` uses a `fixed-clock` at 9600000 as a
   labelled placeholder; that is correct only because the board only ever runs
   the WCD digital codec at 9600000 (`apq8016_sbc.c` `DEFAULT_MCLK_RATE`, the
   only value ever passed to `snd_soc_component_set_sysclk()`). **Required from
   the clock agent:** a real, gateable, settable 9.6 MHz root for the WCD CDC
   block — either a new `GCC_CODEC_DIGCODEC_CLK` token plus a gate in
   `gcc-sdm439.c`, or a separate clock driver.

3. **`qcom,adsp` must stay off `&lpass`.** `lpass-cpu.c` does
   `of_parse_phandle(pdev->dev.of_node, "qcom,adsp", 0)` and returns `-EBUSY`
   unconditionally if it resolves. The stock-derived pine DTB does contain
   such a phandle; it is simply never written here.

---

## 6. TODO — board facts that could not be verified locally

None of these are guessed in the DT. Each is called out in-place.

1. **`&lpass` base address `0x0c008000` is a HYPOTHESIS.**
   msm8916 puts LPAIF at `AON_base + 0x8000` with its sound iomux at
   `AON_base + 0x2000`. sdm439's `AON_base` is `0x0c000000`, but its internal
   offsets clearly differ — the sound iomux is at `AON+0x51000`, not `AON+0x2000`,
   and the WCD digital block is at `AON+0xf0000`, not `AON+0x1c000` — so "same
   offset inside AON" is not a safe assumption. **A wrong base is harmless**
   (lpass-cpu only reads its AHBIX config register and drives the DMAs).

   ```sh
   # read-only test once the clocks are in place
   devmem2 0x0c00918 32     # LPAIF_I2SCTL port 0: expect sane bits, not all-0/all-1
   devmem2 0x0c01818 32     # the msm8916-delta candidate AON+0x6000
   ```

2. **`&lpass` `interrupts = <GIC_SPI 160 …>`** — msm8916's value; needs stock
   confirmation.

3. **Primary MI2S pinmux `gpio69..74 / cdc_pdm0`** — pin numbers and the
   function string are both verified for this SoC from
   `fk/arch/arm64/boot/dts/qcom/msm8937-qdsp6.dtsi:80-100`, but whether pine
   bonds Primary MI2S to that group is not confirmed. Stock CAF routes it via a
   `msm_cdc_pinctrl_pri` node whose `pinctrl-0` is the **pair**
   `cdc_pdm_lines_act + cdc_pdm_lines_2_act` — twelve `cdc_pdm0` pins, not six.

4. **Digital-mic pinmux `gpio0`/`gpio1` → `dmic0_clk`/`dmic0_data`** — as
   `msm8916.dtsi:1366-1391` does. Those two function strings are **not
   verifiable** from the available clone (`drivers/pinctrl/qcom/` is not
   present), and `pine.dtsi` already reserves `gpio0-3` with
   `gpio-reserved-ranges = <0 4>`. **Deliberately omitted rather than
   guessed.** Until it is added, digital-mic capture registers but has no DMIC
   clock on the pin.

5. **MBHC thresholds** — the msm8916 reference set is used; pine-specific
   values should be measured against a real headset.

6. **`DMIC_B1_CTL_DMIC0_CLK_EN_ENABLE` is defined but never written** anywhere
   in `msm8916-wcd-digital.c` (`enable_dmic()` only sets the `CLK_SEL`
   divider and `TX1/TX2_DMIC_CTL`). Whether DMIC capture works without that bit
   is a hardware question this source does not answer. Flagged, not speculated.

---

## 7. Enable sequence

Once the clock TODO (§5.1, §5.2) and the LPAIF base TODO (§6.1) are resolved,
and with `aw87329/dts-fragment.dtsi` included:

```sh
# 1. LPASS clocks first - everything else defers without them
modprobe qcom_gcc

# 2. CPU DAI  (lpass-apq8016.c registers the components; lpass-cpu.c the DAI)
modprobe snd-soc-lpass-cpu
modprobe snd-soc-lpass-apq8016

# 3. Codecs, MBHC last (the WCD drivers select SND_SOC_WCD_MBHC)
modprobe snd-soc-msm8916-analog
modprobe snd-soc-msm8916-digital
modprobe snd-soc-wcd-mbhc

# 4. Machine driver - registers the card
modprobe snd-soc-apq8016

# 5. Speaker amplifier and its supervisor (out-of-tree, order-independent;
#    the supervisor retries the lookup if the amplifier probed later)
modprobe aw87329
modprobe msm439_snd_card
```

Or, for a normal boot, via `/etc/modules`:

```
qcom_gcc
snd-soc-lpass-cpu
snd-soc-lpass-apq8016
snd-soc-msm8916-analog
snd-soc-msm8916-digital
snd-soc-wcd-mbhc
snd-soc-apq8016
aw87329
msm439_snd_card
```

### Expected log markers, in order

```
apq8016-lpass-cpu ... (no -EPROBE_DEFER, no "error getting mi2s-bit-clk")
qcom-wcd-analog ... Failed to get regulator supplies  <-- MUST NOT appear
qcom-wcd-analog ... cannot request mbhc switch irq   <-- MUST NOT appear
apq8016-sbc-sndcard: sound-card: ... (machine driver)
asoc: ASoC: Pre-registered ...  2 x (WCD-Playback, WCD-Capture)
```

### First check

```sh
cat /proc/asound/cards
aplay -l
arecord -l
aplay -D 0 --dump-hw-params /dev/snd/pcmC0D0p
arecord -D 0 -f S16_LE -r 48000 -c 2 -d 5 /tmp/t.wav   # capture
aplay /usr/share/sounds/alsa/Front_Center.wav            # playback
```

### Speaker amplifier sequencing

The correct order matters: enable the amp **after** the codec's SPK path is up,
disable it **before** the path goes down, or the speaker thumps. Until
`pm8916_wcd_analog_enable_spk_pa()` calls into `msm439-snd-card.c`, drive it by
hand:

```sh
AMP=$(ls -d /sys/bus/i2c/devices/*aw87329* | head -1)   # the aw87329 device

# before playback
echo 1 > /sys/devices/platform/*msm439-snd-card*/spk_amp_enable
aplay /usr/share/sounds/alsa/Front_Center.wav
echo 0 > /sys/devices/platform/*msm439-snd-card*/spk_amp_enable

# or use the amplifier's own sysfs switch, which is what
# aw87329/README.md documents:
echo 1 > "$AMP/enable"
```

Once `pm8916_wcd_analog_enable_spk_pa()` is patched in-tree
(`SND_SOC_DAPM_POST_PMU → msm439_spk_amp_enable()`,
`SND_SOC_DAPM_POST_PMD → msm439_spk_amp_disable()`) the sequencing is automatic
and the sysfs writes must be dropped — they would double-drive the amplifier.

---

## 8. Full verification command list

### Labels referenced by `pine-sound.dtsi`

```sh
cd fk/arch/arm64/boot/dts/qcom
grep -n 'sound: sound-card'      msm8937.dtsi    # sound      -> :2340
grep -n 'lpass_codec: codec'     msm8937.dtsi    # lpass_codec-> :2353
grep -n 'pmic1: pmic@1'          pm8953.dtsi     # pmic1      -> :122
grep -n 'spmi_bus: spmi'         msm8937.dtsi    # spmi_bus   -> :1737
grep -n 'tlmm: pinctrl'          msm8937.dtsi    # tlmm       -> :820
grep -n 'soc: soc@0'             msm8937.dtsi    # soc        -> :612
grep -n 'pm8953_gpios: gpio'     pm8953.dtsi
grep -n 'pm8953_l5\|pm8953_s4\|pm8953_l13' \
	fk/arch/arm64/boot/dts/qcom/sdm439-xiaomi-pine.dts
grep -rn 'lpass:'                 .               # only msm8916.dtsi:2125
grep -rn 'wcd_codec:'             .               # only pm8937.dtsi:158
```

### Headers included

```sh
grep -n 'MI2S_PRIMARY\|MI2S_TERTIARY' fk/include/dt-bindings/sound/qcom,lpass.h
grep -n 'GIC_SPI\|IRQ_TYPE_' fk/include/dt-bindings/interrupt-controller/arm-gic.h
grep -n 'IRQ_TYPE_NONE' fk/include/dt-bindings/interrupt-controller/irq.h
grep -n 'pm8953.dtsi\|irq.h' fk/arch/arm64/boot/dts/qcom/pm8953.dtsi
grep -n 'arm-gic.h\|gcc-msm8917.h' fk/arch/arm64/boot/dts/qcom/msm8937.dtsi
```

### Sound node as inherited

```sh
sed -n '2340,2352p' fk/arch/arm64/boot/dts/qcom/msm8937.dtsi
```

Confirms `compatible = "qcom,msm8916-qdsp6-sndcard"` (overridden) and the four
iomux regions `0x0c051000 / 0x0c051004 / 0x0c055000 / 0x0c052000`.

### Machine driver behaviour

```sh
grep -n 'apq8016_sbc_platform_probe' -A 45 sound/soc/qcom/apq8016_sbc.c
grep -n 'apq8016_dai_init' -A 95 sound/soc/qcom/apq8016_sbc.c | head -100
grep -n 'qcom_snd_parse_of' -A 120 sound/soc/qcom/common.c | grep -n \
	'model\|audio-routing\|pin-switches\|aux-devs\|widgets\|of_get_available_child_count'
grep -n 'mi2s_osr_clk\[dai_id\]\|dai_bit_clk_names\[i\]' sound/soc/qcom/lpass-cpu.c
grep -n 'LPAIF_I2SCTL_WSSRC_INTERNAL\|clk_set_rate(drvdata->mi2s_bit_clk' \
	sound/soc/qcom/lpass-cpu.c
```

### Codec drivers

```sh
grep -n 'supply_names\[\]' -A 5 sound/soc/codecs/msm8916-wcd-analog.c
grep -n 'platform_get_irq_byname' sound/soc/codecs/msm8916-wcd-analog.c
grep -n 'pm8916_wcd_analog_dai\[\]' -A 25 sound/soc/codecs/msm8916-wcd-analog.c
grep -n 'msm8916_wcd_digital_dai\[\]' -A 28 sound/soc/codecs/msm8916-wcd-digital.c
grep -n 'devm_clk_get' -B2 -A6 sound/soc/codecs/msm8916-wcd-digital.c
grep -c DMIC sound/soc/codecs/msm8916-wcd-analog.c    # 0
grep -c DMIC sound/soc/codecs/msm8916-wcd-digital.c   # 42
grep -n 'of_parse_phandle(pdev->dev.of_node, "qcom,adsp"' sound/soc/qcom/lpass-cpu.c
```

### Clocks

See §5.

---

## 9. Status summary

| Item | State |
|---|---|
| Machine driver | **`apq8016_sbc.c` reusable as-is**, no variant |
| `msm439-snd-card.c` | amp supervisor only; not an ASoC machine driver |
| `wcd_codec` node | complete: 14 IRQs, mainline supply names, MBHC thresholds |
| `&lpass` node | complete except base address + IRQ (**hypotheses**) |
| `&lpass_codec` node | `ahbix-clk` verified; `mclk` **placeholder** |
| 2 dai_links | complete, both directions, two codecs each |
| `audio-routing` | 12 names, all verified |
| DL1 playback pinmux | pins + function verified; board assignment unconfirmed |
| DMIC pinmux | **omitted, not guessed** |
| AW87329 | separate module; coupled via sysfs / exported symbols |
| **Blocking** | clock provider (`gcc-sdm439.o` Makefile + `mclk` gate), LPAIF base address, DMIC pinmux |
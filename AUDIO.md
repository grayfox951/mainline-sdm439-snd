# Audio on Xiaomi Redmi 7A (pine), SDM439

Complete audio stack for mainline Linux on the Redmi 7A. Written against
`msm89x7-mainline/linux @ msm89x7/7.1.3`.

* Edited by: grayfox951 <admin@dnr.qzz.io>
* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause

---

## 1. Hardware, as verified on the device

| Part | Fact | Where it came from |
|---|---|---|
| SoC | SDM439 (msm89x7 family), PM8953 + PMI632 | device tree |
| Analog codec | PM8953 WCD @ SPMI `0xf000`, `qcom,pm8916-wcd-analog-codec` | stock `sdm439-audio.dtsi:88` |
| Digital codec | `qcom,msm8916-wcd-digital-codec` @ `0x0c0f0000` | stock `sdm439-audio.dtsi:150` |
| Speaker amp | Awinic **AW87329**, i2c `blsp1_i2c2`, addr `0x58`, reset tlmm 125 **active low** | stock `base/pine/audio.dtsi` |
| SLIMbus | `0x0c140000`, size `0x2c000` | stock `msm8937.dtsi:374` |
| AON base | `0x0c000000` | node name span `@c0a300`..`@c3f000` |
| GCC | `clock-controller@1800000`, reg `0x01800000 0x80000` | `msm8937.dtsi:1152` |
| Touch | Holitech **GT1151Q**, i2c `0x14` | device dmesg: `IC VERSION:GT1158_06000E`, `[TP-IC]GT1151Q` |
| Panel | `ili9881c_c3e`, 720x1440 | `dtbo/common/panel/dsi-panel-ili9881c-hdplus-video_c3e.dtsi` |
| Battery | Peony 4000 mAh, 78 kohm | `base/common/batterydata/Peony-Default-4000mah-78kohm.dtsi` |

The stock kernel log is unambiguous about the touch chip:

```
<<GTP-INF>>[gt1x_read_version:1025] IC VERSION:GT1158_06000E(Patch)_0102(Mask)_00(SensorID)
[Vendor]holitech(TP) + holitech(LCD), [TP-IC]GT1151Q, [FW]Ver:14 CFG:51
[FTS][Error]The current ic is goodix!!!
```

The last line is the Focaltech driver having bound `0x38`, read the id, seen a Goodix part and
refused. `edt,edt-ft5406`, which upstream shipped, is not a real compatible and never matched
any driver.

## 2. No AFE. No q6apm. That is verified, not assumed.

`sound/soa/qcom/` (q6apm) and `sound/soc/msm/` (the AFE) return 404 **both in the fork and in
upstream v7.1**. They were never ported for any Qualcomm part.

That absence does not block this platform, and the proof is the working msm8916 reference in
this repository's history, `apq8016-sbc.dtsi`:

```
AFE occurrences in that file : 0
lpass@7708000   { }      /* CPU-side DAI  */
lpass_codec: codec { }   /* WCD digital   */
sound: sound { compatible = "qcom,apq8016-sbc-sndcard"; }
```

Zero AFE references, and audio works on that part. All 24 official CAF device trees for SDM439
(`android-msm-baiji-4.9-pie-wear-dr`) were also checked: no AFE node, no LPAIF node, and no
`0x584xxxx` in any `reg`. The complete AON map CAF describes is just

```
0x0c0f0000   WCD digital codec
0x0c140000   SLIMbus
0x0c200000   ADSP PIL remoteproc   (named "qcom,lpass@..." - a CAF misnomer, it is the ADSP)
```

So the QDSP6 path is simply absent, and this stack does not use it.

### The root cause of the missing sound in the fork

```dtsi
/* msm8937.dtsi:2341, as shipped */
sound: sound-card@c051000 {
        compatible = "qcom,msm8916-qdsp6-sndcard";   /* WRONG */
```

That compatible selects `msm8916_qdsp6_add_ops()`, which sets `card->components = "qdsp6"` and
pins every link to 48 kHz / 2ch / S16_LE, waiting for AFE DAIs that do not exist. The card
defers forever. Changing it to `qcom,apq8016-sbc-sndcard` selects `apq8016_sbc_add_ops()`,
which is only `link->init = apq8016_dai_init` - no QDSP6 anything.

## 3. The signal path

```
                      AON audio clock gates, GCC+0x1c000
                      (ahbix, mi2s-bit0..3, pcnoc-mport, pcnoc-sway)
                                 |
   I2S / MI2S  <------------>  sdm439-lpaif (CPU DAI)      [lpaif/]
        |                            MI2S_PRIMARY   playback + capture
        |                            MI2S_TERTIARY  capture only
        v
   +--------------------------------------------------+
   |  lpass_codec  qcom,msm8916-wcd-digital-codec      |
   |  0x0c0f0000                                      |
   |                                                  |
   |  playback : AIF1  -> WCD analog                  |
   |  capture  : DMIC1 -> DEC1 -> CIC1 -> I2S TX1 -> AIF1 Capture
   +--------------------------------------------------+
        |
        v
   +--------------------------------------------------+
   |  wcd_codec  qcom,pm8916-wcd-analog-codec         |
   |  SPMI 0xf000, size 0x200, 14 interrupts          |
   |                                                  |
   |   headphones   HPHL / HPHR  + HPHL PA / HPHR PA  |
   |   microphones  AMIC1/2/3 + MIC_BIAS, DMIC1/2     |
   |   speaker      SPK OUT + SPK PA (internal)       |
   +--------------------------------------------------+
        |
        v
   +--------------------------------------------------+
   |  aw87329  awinic,aw87329_pa                       |
   |  i2c blsp1_i2c2 addr 0x58, reset tlmm 125         |
   |  analog in, class-D out -> the loudspeaker        |
   +--------------------------------------------------+
```

### Frequencies and formats

| Parameter | Value | Source |
|---|---|---|
| Master clock (MCLK) | **9.6 MHz** | stock `qcom,cdc-mclk-clk-rate = <9600000>` |
| Bit clock on I2S | `9600000 / (sample_rate * 2 * 16)` -> 1.92 MHz at 48 kHz | `msm8916_wcd_analog_enable_hph_pa` divides MCLK this way |
| Codec slave rates | 48 / 44.1 / 32 / 16 / 8 kHz | `msm8916-wcd-analog.c` |
| Word length | 16 / 24 / 32 bit S/D/T | `msm8916-wcd-analog.c` |
| Channels | up to 2 | |
| Digital mic clock (`dmic0_clk`) | derived from MCLK by the codec | `msm8916-wcd-digital.c` |
| PDM mic clock | TLMM 69, 73, 74 (`cdc_pdm0`) | stock `msm8937-qdsp6.dtsi` |
| AON audio gates enabled at boot | `0x181c004` = `0x20008001`, bit 0 set | measured on the device |

### Protocols

| Link | Protocol |
|---|---|
| SoC -> WCD digital | **I2S / MI2S**, LPCM clock controller, DMA from `dmabase`/`dmactl`/`dmacurr` |
| WCD digital -> analog codec | internal WCD CDC, no external bus |
| Analog codec -> AW87329 | **analog line level**, chip is analog-in / class-D out, no digital audio on it |
| Digital mic | **PDM** into the codec's DMIC1/DMIC2, decoded inside the WCD |

The AW87329 carries no I2S. It only needs enable, gain and a DSP config blob, which is why it
has no DAI and no `dai_link`.

## 4. Files

### `lpaif/sdm439-lpaif.c`
The CPU-side PCM DAI. Two DAIs on one platform device:
`sdm439-lpaif-primary` (id `MI2S_PRIMARY`, playback + capture) and
`sdm439-lpaif-tertiary` (id `MI2S_TERTIARY`, capture only). Programs `LPAIF_I2SCTL_REG` in
`hw_params` and sets up RDMA/WRDMA from `dma_params`.

Two design decisions that matter:

* **It registers components on the `&lpass` device, not on a child `platform` node.**
  `qcom_snd_parse_of()` in `sound/soc/qcom/common.c` assigns `link->platforms->of_node =
  link->cpus->of_node` when a link has no `platform` child. Adding one sets
  `link->no_pcm = 1` and `link->nonatomic = 1`, i.e. a DPCM backend, which would require a
  frontend that does not exist here.
* **`write_en` defaults to 0.** Probe and every PCM path perform reads only until it is set.
  This is what makes the module safe to test with an unverified register base: a wrong `reg`
  cannot wedge the board.

The DAI is a **slave**. The codec is the I2S master because it derives `dmic0_clk` from its own
MCLK. `lpass_cpu_daiops_hw_params()` in the in-tree driver hard-codes `WSSRC_INTERNAL`, so no
`set_fmt` is implemented.

### `clk/gcc-sdm439-oot.c`
The AON audio clock provider, as an out-of-tree module, because no in-tree driver binds
`qcom,q6afe-clocks` and `gcc-msm8917.c` has zero `ULTAUDIO` references. It creates its own
regmap over the existing GCC window and registers only the 7 AON gates, in its own ID space, so
it cannot collide with the in-tree driver that already owns `0x01800000`.

It reaches the hardware through `qcom,gcc = <&gcc>` + `devm_of_iomap()` rather than
`request_mem_region`, which is provably conflict-free. Real gate `clk_ops` are implemented
because `lpass-cpu.c` hard-fails on `clk_bulk_prepare_enable` and on `clk_set_rate` of the bit
clock, and a parentless `fixed-clock` stub would return `-EINVAL` from `clk_set_rate`.

**This was not decorative.** My first attempt used `fixed-clock` stubs and it provably could
not have worked.

### `aw87329/aw87329.c`
Port of the CAF `aw87329_audio.c`. i2c + regmap, optional supply, optional reset GPIO, sysfs
`chipid`/`enable`/`gain`/`mode`, module parameters for gain, DSP config and mode.

Register map, taken verbatim from the vendor driver and cross-checked against the AW87319
sibling:

| Reg | Name | Note |
|---|---|---|
| `0x00` | CHIPID | expect **0x39** |
| `0x01` | SYSCTRL | **written twice** (`&0xF7` then full) - regmap caching must stay off |
| `0x02` | MODECTRL | |
| `0x03` | CPOVP | |
| `0x04` | CPP | |
| `0x05` | GAIN | |
| `0x06`-`0x0A` | AGC3_PO, AGC3, AGC2_PO, AGC2, AGC1 | protection + AGC |
| `SYSCTRL = 0x0C` | | parks the chip |

CAF-only symbols were replaced: `xiaomi_sdm439_mach_get()` dropped, the three `EXPORT_SYMBOL`s
became `aw87329_spk_enable/disable`, `i2c_smbus_*` became regmap, `request_firmware_nowait`
dropped in favour of compiled-in defaults, and `MODULE_LICENSE("GPL v2")` corrected to
`"GPL-2.0"` - the former is not valid SPDX and modpost rejects it.

### `snd/pine-sound.dtsi`
The device tree: the WCD analog codec node, the sound card with the four iomux regions at
`0x0c051000 / 0c051004 / 0c055000 / 0c052000`, the `lpass` node, and two `dai_link`s.

Three corrections that are easy to get wrong and were made here against the driver source,
not against the stock tree:

* The codec driver requests **`vdd-cdc-io`** and **`vdd-cdc-tx-rx-cx`**. The stock names
  `cdc-vdd-io`, `cdc-vdd-pa`, `cdc-vdd-mic-bias` are never read, and a node carrying only the
  stock names fails probe.
* MBHC needs **`mbhc_switch_int`**, `mbhc_but_press_det`, `mbhc_but_rel_det`. Stock's
  `mbhc_int` is not the property the driver looks for.
* The supply voltages from stock are kept (`cdc-vdd-io` on `l5` 1.8 V, `cdc-vdda-cp` on `s4`
  1.9-2.05 V, `cdc-vdd-pa` on `s4`, `cdc-vdd-mic-bias` on `l13` 3.075 V), re-exposed under the
  names the driver actually asks for.

`qcom,audio-routing` uses only widget names verified by grep in the drivers: `AMIC1`, `AMIC2`,
`AMIC3`, `MIC BIAS Internal1/2`, `MIC BIAS External1`, `DMIC1`, `DMIC2`, `Handset Mic`,
`Headset Mic`, `Secondary Mic`, `Mic Jack`, `Headphone Jack`. `SPK_OUT` is intentionally
omitted: the amplifier is not a codec widget.

### `snd/sdm439-snd-card.c`
Not a machine driver. `apq8016_sbc.c` is reusable as-is. This is only a supervisor for the
AW87329: it finds the amp by DT phandle and toggles it alongside the codec's `SPK PA`.

### `probe/`
`read.sh` reads eight words from an address. `safe-write.sh` writes one word.
Both are safe against bricking because the device tree on disk is never modified - writes go
to RAM only, so a hang is just an auto-reboot into the phone's own working DTB.

## 5. What is still unknown

**The LPASS register base for SDM439.** It is not in any tree:

| Source searched | Result |
|---|---|
| fork, full tree, 67 778 files | no `lpass` node in `msm8937.dtsi` |
| upstream v7.1 | no LPASS node for any 8x2q part |
| MiCode/Xiaomi_Kernel_OpenSource, full partial clone, 60 920 files | only `msm8916.dtsi`'s `lpass@07708000` |
| Mi-Thorium, all 9 repos and all branches | no LPAIF |
| 24 official CAF SDM439 device trees | no LPAIF, no AFE, no `0x584xxxx` |
| register scan on the device, 30 candidate addresses in AON | no LPAIF signature |

What was ruled out by measurement, not by argument:

```
0x0c008000 reads 0x80000000   reserved pattern; writing there hangs the board. NOT the base.
0x181c004  reads 0x20008001   genuine AON audio clock register - this one is real
```

### How to find it safely

Because `write_en` defaults to 0, the module can be loaded against any candidate address
without writing a single register. So the search is read-only:

```sh
# for each candidate base, read a window
sudo sh probe/read.sh 0x0c0a0000
```

An LPAIF in reset reads as near-zero, so content alone will not identify it. The discriminator
that does work is the **interrupt**: `GIC_SPI` 160 is the LPAIF line on msm8916. On the device:

```sh
cat /proc/interrupts | grep -iE "lpaif|160"
sudo devmem2 0x0c0XXXXX 32      # step 4 KB across the AON window
```

Alternative route if the block genuinely does not exist as a standalone register window: put
`lpass_codec` (`0x0c0f0000`, confirmed twice from stock) in the CPU slot and drive the I2S
through the WCD digital block itself. That needs a different driver and is out of scope here.

## 6. Build and load, no kernel rebuild

`CONFIG_MODVERSIONS` is off in this kernel, so an out-of-tree module needs only
`make modules_prepare`, never a kernel build.

```sh
cd /path/to/pine-audio
make -C /path/to/msm89x7-linux M="$PWD/clk"     ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules
make -C /path/to/msm89x7-linux M="$PWD/lpaif"   ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules
make -C /path/to/msm89x7-linux M="$PWD/aw87329" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules
```

Load order matters - clocks first, then the DAI, then the amplifier:

```sh
insmod clk/gcc-sdm439-oot.ko
insmod lpaif/sdm439-lpaif.ko
insmod aw87329/aw87329.ko
```

Verify:

```sh
cat /sys/bus/i2c/devices/2-0058/aw87329/chipid      # expect 0x39
ls /dev/snd/
aplay -l
arecord -l
speaker-test -D hw:0,0 -c 2 -t sine -l 1
```

Undo:

```sh
rmmod aw87329 sdm439_lpaif gcc_sdm439_oot
```

**None of this has been compiled.** No build tree is available in this environment. Every
symbol was checked against the 7.1.3 sources by reading; the ones that could not be read are
listed in each component's README.

## 7. Expected result

| | |
|---|---|
| Headphone output | yes |
| Jack detect, headset routing | yes, via `wcd-mbhc-v2` |
| Analog microphones | yes |
| Digital PDM microphone | yes, decoded inside the WCD |
| Loudspeaker | yes, via AW87329 |
| Voice calls, DSP effects | **no** - requires the AFE and q6apm, which do not exist in any tree |
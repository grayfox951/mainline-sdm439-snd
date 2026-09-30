# mainline-sdm439-snd

Audio bring-up for **Xiaomi Redmi 7A (pine)**, SoC **SDM439**, kernel **msm89x7-mainline/linux @ msm89x7/7.1.3**.

## What the hardware actually is

| | |
|---|---|
| Codec | PM8953 WCD analog codec @ SPMI `0xf000`, compatible `qcom,pm8916-wcd-analog-codec` |
| Digital codec | `qcom,msm8916-wcd-digital-codec` @ `0x0c0f0000` (confirmed twice from stock) |
| Speaker PA | Awinic **AW87329** @ i2c bus 2, addr `0x58`, reset tlmm 125 **ACTIVE_LOW** |
| Touch | Holitech **GT1151Q** @ i2c `0x14` (stock log: `IC VERSION:GT1158`, `[FTS]Error` = FT driver refused) |
| Panel | **ili9881c_c3e**, 720x1440 (stock: `dsi-panel-ili9881c-hdplus-video_c3e.dtsi`) |
| Battery | Peony 4000 mAh, 78 kohm |

## Why the stock mainline port has no sound

`sound/soa/qcom/` (q6apm) and `sound/soc/msm/` (AFE) are absent **in the fork and in upstream v7.1**.
So the QDSP6 digital path was never available. What remains is the analog + PDM path
inside the WCD codec itself.

Key finding: `msm8916-wcd-analog.c` has **41 PDM references** and DAI ids
`pm8916_wcd_analog_pdm_rx` / `pm8916_wcd_analog_pdm_tx`. The digital microphone is decoded
**by the codec**, not by the AFE, so PDM does not require the AFE on this board.

## Hard-won facts

* `0x0c008000` is **not** the LPASS base. Reading it returns `0x80000000` (reserved
  pattern) and writing there hangs the machine. Do not use it.
* AON audio clock gates are at `GCC(0x1800000) + 0x1c000`, same offsets as
  `gcc-msm8916.c`. Confirmed on device: `0x181c004` reads `0x20008001`, bit 0 set.
  No driver binds `qcom,q6afe-clocks`, so `lpass-cpu` defers with `-EPROBE_DEFER`.
* `CONFIG_BACKLIGHT_TI_LM3697`, `CONFIG_REGULATOR_PM8150B_VBUS` are **not in the kernel
  config** - they must be built out-of-tree as modules (`CONFIG_MODVERSIONS` is off,
  so `make modules_prepare` is enough, no kernel rebuild).

## probe/

`safe-write.sh` / `read.sh` - register access that cannot brick the device:
the DTB on disk is never modified, writes go to RAM only, so a hang just
auto-reboots into the phone's own working DTB.

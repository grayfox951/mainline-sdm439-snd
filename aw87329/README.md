# AW87329 speaker PA — out-of-tree Linux driver

Port of the Xiaomi/Qualcomm vendor driver `tp/audio_pa/aw87329_audio.c` (v1.1.2,
Awinic Technology + Xiaomi) to a clean mainline-style module for:

| | |
|---|---|
| Board | Xiaomi Redmi 7A (`pine`) |
| SoC | SDM439 |
| Kernel | `msm89x7-mainline/linux` @ `msm89x7/7.1.3` (arm64) |
| Chip | Awinic AW87329 class-K smart speaker PA |
| Address | i2c bus 2, `0x58` |
| Reset | `tlmm` pin 125, `GPIO_ACTIVE_LOW` |
| Driver version | 1.0.0 |

## What this is

The AW87329 is an **analog-input** amplifier: the PM8953 WCD codec's line output
feeds it, and it drives the speaker. It carries **no I2S/TDM stream**, so this
driver deliberately contains:

* no DAI driver
* no `dai_link`
* no AFE / QDSP6 / `mach` dependency

Only an on/off + gain control surface, in sysfs. See
[Codec coupling](#codec-coupling) for how it gets tied to the codec's SPK PA.

## Files

| File | Purpose |
|---|---|
| `aw87329.c` | the driver |
| `Makefile` | `obj-m += aw87329.o` (out-of-tree) |
| `Kconfig` | only if you later fold this into the kernel tree; unused by the OOT build |
| `dts-fragment.dtsi` | corrected DT node, mainline property names |
| `README.md` | this file |

## Register map (confirmed)

Taken from the vendor driver, `aw87329_audio.c:56-66`, and cross-checked against
`tp8937/audio_pa/AW87319_Audio_M.c`:

| Reg | Name | Notes |
|---|---|---|
| `0x00` | `CHIPID` | reads `0x39`; read-only in practice |
| `0x01` | `SYSCTRL` | written **twice** per config, see below |
| `0x02` | `MODECTRL` | |
| `0x03` | `CPOVP` | charge-pump over-voltage protect |
| `0x04` | `CPP` | charge-pump period |
| `0x05` | `GAIN` | runtime-tunable |
| `0x06` | `AGC3_PO` | |
| `0x07` | `AGC3` | |
| `0x08` | `AGC2_PO` | |
| `0x09` | `AGC2` | |
| `0x0A` | `AGC1` | |
| `0x0C` | — | value written to `SYSCTRL` to park the amp (`aw87329_CHIP_DISABLE`) |

**No other register values are used, and none were invented.** The three
factory configurations below are copied byte for byte from
`aw87329_audio.c:127-135`, mapped onto registers `0x01..0x0A`:

```
kspk   0e a3 06 05 10 07 52 06 08 96      <- speaker, the one this board uses
drcv   0a ab 06 05 00 0f 52 09 08 97
abrcv  0a af 06 05 00 0f 52 09 08 97
```

The vendor arrays are 11 bytes including a leading `0x39`, which is the chip ID
at `0x00`; the write sequence never writes `0x00`, so this driver stores 10 bytes.

### The write sequence is not optional

Reproduced exactly from `aw87329_audio.c:260-270`:

```
SYSCTRL  = cfg[0x01] & 0xF7     stage: bit 3 clear, config not yet latched
MODECTRL, CPOVP, CPP, GAIN,
AGC3_PO, AGC3, AGC2_PO, AGC2, AGC1
SYSCTRL  = cfg[0x01]            commit
```

`SYSCTRL` is written **twice with two different values** and both must reach the
part in order. This is why `aw87329_regmap_config` has `.cache = false`: a
regcache would coalesce or reorder the two writes and lose the commit.

## Build

### Why no kernel rebuild is needed

This kernel has `CONFIG_MODVERSIONS` **off**. Without it, a module built against
`modules_prepare` output has no symbol version hashes to match, so a full kernel
rebuild and reinstall is not required — only `make modules_prepare` in a tree
that has been configured at least once.

There is a second, stronger reason it matters here: the AW87329 node is **not in
the running DTB**. Adding a DT node normally forces a kernel rebuild plus DTB
replacement. This driver is loaded with `insmod`/`modprobe` against the *stock*
DTB, so nothing in the kernel image changes.

### The exact command

Run from this directory (`aw87329/`), with `K` pointing at a configured
msm89x7-mainline tree, and `ARCH`/`CROSS_COMPILE` set to your arm64 toolchain:

```sh
make -C /path/to/msm89x7-linux M="$PWD" ARCH=arm64 \
     CROSS_COMPILE=aarch64-linux-gnu- modules
```

With clang instead of GCC:

```sh
make -C /path/to/msm89x7-linux M="$PWD" ARCH=arm64 LLVM=1 modules
```

Output: `aw87329.ko` in this directory.

If the tree has never been configured, `make modules_prepare` must be run once in
the kernel tree first, against its own `.config`.

> ### ⚠️ THIS DRIVER HAS NOT BEEN COMPILED
>
> No configured kernel build tree exists on this machine, so `aw87329.ko` has
> **never been built**. Do not assume it builds.
>
> **You must run this exact command before doing anything else:**
>
> ```sh
> make -C /path/to/msm89x7-linux M="$PWD" ARCH=arm64 \
>      CROSS_COMPILE=aarch64-linux-gnu- modules
> ```
>
> What *has* been done: the source was checked by inspection against the
> headers in this tree, and the `dsp_cfg` parameter parser was extracted and
> unit-tested on a host compiler against 14 inputs (valid hex/decimal, commas,
> mixed case, short, long, out-of-range, trailing junk, empty, NULL) — all
> behave correctly. Full-file syntax has been parsed with `-Wall -Wextra`
> against hand-written stub headers that mirror the real prototypes, with zero
> diagnostics. That is not a substitute for a real build: stub headers cannot
> catch a genuine API drift.
>
> **Expect to fix compile errors on the first real build.** The most likely
> candidates are listed under [If it fails to build](#if-it-fails-to-build).

## Device tree

The stock node:

```
aw87329@58 {
        status = "okay";
        compatible = "awinic,aw87329_pa";
        reg = <0x58>;
        reset-gpio = <&tlmm 125 0x01>;
};
```

To use the driver **without** touching the kernel, put the node in a small
overlay or add it to the board `.dtsi` and rebuild only the DTB.

Corrections in `dts-fragment.dtsi`:

1. **`reset-gpio` → `reset-gpios`.** The stock property is singular, which is not
   a binding name; the GPIO bindings define `reset-gpios`. The driver uses
   `devm_gpiod_get_optional(dev, "reset", ...)`, which resolves `reset-gpios` and
   then falls back to the legacy `reset-gpio` spelling — so the stock name
   happens to work, but it produces a `dt_binding` warning. The driver handles
   **both** names, so either DT works.
2. **`0x01` → `GPIO_ACTIVE_LOW`.** Same value, symbolic spelling.
3. **Bus enable.** The I2C bus is `disabled` in `msm8937.dtsi` and nothing on this
   board turns it on, so the PA would never probe.

> ### ⚠️ Bus label: `blsp1_i2c2`, not `blsp1_i2c0`
>
> The stock node lives under `i2c@78b6000`. In this tree that node is labelled
> **`blsp1_i2c2`** — it is QUP2 of BLSP1, hence "2". There is no
> `blsp1_i2c0` in `msm8937.dtsi`; that name appears only in unrelated SoC files
> (`qcs404.dtsi`, `ipq6018.dtsi`). Referencing `&blsp1_i2c0` is a compile error.

## Load

```sh
insmod aw87329.ko
```

or, if you install it into a module directory:

```sh
install -m 0644 aw87329.ko /lib/modules/$(uname -r)/extra/
depmod -a
modprobe aw87329
```

With parameters, e.g. to start in speaker mode with the vendor gain:

```sh
insmod aw87329.ko default_mode=1 gain=0x10
```

## Verify

### 1. Probe succeeded

```sh
dmesg | grep -i aw87329
```

Expected:

```
aw87329 0-0028: probe ...   (bus number depends on registration order)
aw87329 0-0028: chip id 0x39
aw87329 0-0028: aw87329 probed, version 1.0.0
```

The chip ID **must be `0x39`**. If probe fails you get
`no AW87329 on i2c bus N addr 0x58`. Usual causes, in order of likelihood:

* the I2C bus is still `disabled` (most likely — see the DT section);
* reset polarity is inverted: retry with `insmod aw87329.ko reset_active_high=1`;
* wrong address.

### 2. sysfs

The device path is the i2c client, so:

```sh
# find it
find /sys/devices -name 'aw87329' -type d 2>/dev/null
# or
ls -d /sys/bus/i2c/devices/*/aw87329
```

Set `D` to the result, e.g. `D=/sys/bus/i2c/devices/2-0058/aw87329`. All
attributes are admin-only (0600/0400).

| Path | Read | Write |
|---|---|---|
| `$D/chipid` | `0x39` | — |
| `$D/enable` | `0` / `1` | `1` / `0` |
| `$D/gain` | e.g. `0x10` | hex byte, e.g. `0x12` |
| `$D/mode` | `1 kspk` | `0`=off `1`=kspk `2`=drcv `3`=abrcv |
| `$D/registers` | full `0x00..0x0a` dump | **read-only by design** |

Bring-up sequence:

```sh
cat $D/chipid            # expect 0x39
cat $D/registers         # expect 00:39 and the kspk payload after enabling
echo 1 > $D/enable
cat $D/registers         # 01:0e 02:a3 03:06 04:05 05:10 06:07 07:52 08:06 09:08 0a:96
```

After `echo 1 > $D/enable`, registers `0x01..0x0a` should read back the kspk
payload above. If they do not, the write sequence did not commit.

**Safety:** the vendor driver had a *writable* `reg` sysfs attribute
(`aw87329_audio.c:567`) that wrote any register/value pair. This port omits it.
`registers` is read-only. Tuning goes through `gain`, `mode` and the `dsp_cfg`
module parameter, which reject malformed input rather than half-applying it.

### 3. Module parameters

```sh
ls /sys/module/aw87329/parameters/
# dsp_cfg  default_mode  gain  reset_active_high
```

`dsp_cfg` replaces the payload of `default_mode` only; the other two modes keep
their factory payloads, so a bad override cannot corrupt a path it does not own.
It is parsed as 10 hex bytes and rejects short, long, out-of-range or malformed
input outright. Toggle `enable` afterwards to push it.

```sh
echo "0x0e 0xa3 0x06 0x05 0x12 0x07 0x52 0x06 0x08 0x96" > /sys/module/aw87329/parameters/dsp_cfg
cat /sys/module/aw87329/parameters/dsp_cfg   # read back
echo 0 > $D/enable; echo 1 > $D/enable        # re-arm
```

Start low: `0x10` is the vendor speaker gain. Raise it a step at a time, and
watch for distortion — a class-K PA driven too hard will damage the speaker.
Note that the `gain` parameter overrides the GAIN register from the config
array, so setting `gain` wins over byte 5 of `dsp_cfg`.

## Codec coupling

The amplifier must be enabled together with the codec's SPK PA, otherwise you
get a silent or distorted speaker. The vendor driver exported three symbols
(`xiaomi_sdm439_aw87329_audio_kspk/drcv/off`) for the CAF machine driver to call.
This port exports two, with mainline names:

```c
int aw87329_spk_enable(struct device *dev);
int aw87329_spk_disable(struct device *dev);
```

`dev` is the AW87329 i2c client device, i.e. `&client->dev` for
`aw87329@58`.

The intended call site is `pm8916_wcd_analog_enable_spk_pa()` in
`sound/soc/codecs/msm8916-wcd-analog.c`, which already runs on SPK PA
power-up and power-down:

* `SND_SOC_DAPM_POST_PMU` → `aw87329_spk_enable()`
* `SND_SOC_DAPM_POST_PMD` → `aw87329_spk_disable()`

That file is **in-tree**, so an out-of-tree module cannot patch it. Until it is
edited, drive the amp from sysfs — `echo 1 > $D/enable` after the codec's SPK
path comes up, `echo 0 > $D/enable` before it goes down. Wiring it into the
codec properly requires a kernel rebuild, which is the one step this OOT
approach deliberately avoids.

## Undo

```sh
rmmod aw87329
```

`remove` writes the disable value, asserts reset, drops the regulator, and
releases the sysfs group, so the amplifier is left quiet and the GPIO is handed
back. All other resources are `devm`-managed.

If the module was installed:

```sh
rm /lib/modules/$(uname -r)/extra/aw87329.ko
depmod -a
```

To stop it loading at boot, delete the `insmod`/`modprobe` line from
`/etc/modules` or the init script.

## CAF / techpack symbols replaced

The vendor driver was written for the CAF techpack. Each of these was replaced
with a mainline equivalent; all are noted in the driver header too.

| Vendor symbol | Replacement |
|---|---|
| `xiaomi_sdm439_mach_get()` (`<xiaomi-sdm439/mach.h>`) | dropped; the DT node is the gate. Unlinkable from an OOT module |
| `EXPORT_SYMBOL(xiaomi_sdm439_aw87329_audio_{kspk,drcv,off})` | `aw87329_spk_enable()` / `aw87329_spk_disable()` |
| `of_get_named_gpio()` + `devm_gpio_request_one()` + `gpio_set_value_cansleep()` | `devm_gpiod_get_optional()` + `gpiod_set_raw_value_cansleep()` |
| `i2c_smbus_write_byte_data()` / `read_byte_data()` | regmap over `devm_regmap_init_i2c()` |
| `request_firmware_nowait()` + `.bin` blob loading | dropped; compiled-in defaults are the same values the vendor falls back to |
| `hrtimer` + `work_struct` deferred config load | dropped with the above |
| file-static singleton, manual `init_flag`/`hwen_flag` | per-device struct, `devm_*`, mutex, devm sysfs group |
| `MODULE_LICENSE("GPL v2")` | `MODULE_LICENSE("GPL-2.0")` — `"GPL v2"` is not a valid SPDX id |
| `sysfs_create_group()` with no matching remove | `devm_device_add_group()` |
| `#include <asm/io.h>`, `<linux/pci.h>`, `<linux/gameport.h>`, `<linux/fs.h>`, `<asm/uaccess.h>` | removed; unused, some do not exist in 7.x |

The legacy GPIO API silently discarded the `GPIO_ACTIVE_LOW` flag and wrote raw
`0`/`1`. The descriptor API honours the flag, so this port uses
`gpiod_set_raw_value_cansleep()` to reproduce the vendor's exact physical pin
levels, and offers `reset_active_high=1` if a board variant is wired the other
way.

## If it fails to build

Likely fixes, roughly in order of probability:

1. **`__ATTR_RO_MODE` / `DEVICE_ATTR_ADMIN_RO`** — older trees lack the
   `DEVICE_ATTR_ADMIN_*` helpers. Replace with `DEVICE_ATTR_RO`/`DEVICE_ATTR_RW`.
2. **`.remove` signature** — `void (*remove)` since 6.9, `int` before that. This
   driver assumes `void` (7.x).
3. **`i2c_check_functionality` / `i2c_adapter_id`** — include `<linux/i2c.h>`;
   `I2C_FUNC_*` comes from `<uapi/linux/i2c.h>` via that header.
4. **`devm_device_add_group`** — needs `CONFIG_SYSFS`; swap for
   `device_create_file()` calls if absent.
5. **`gpiod_set_raw_value_cansleep`** — should be present; if not, use
   `gpiod_set_value_cansleep()` and drop the `reset_active_high` override.
6. `MODULE_VERSION` / `MODULE_DEVICE_TABLE` — include `<linux/module.h>` and
   `<linux/mod_devicetable.h>` (pulled in by `<linux/i2c.h>`).

# SDM439 AON audio / LPASS clocks, out of tree

Redmi 7A (pine), SDM439, `msm89x7-mainline/linux` @ `msm89x7/7.1.3`.
The kernel may not be rebuilt, so this ships as a loadable module.

---

## 1. What is broken, in one paragraph

`msm8937.dtsi:1152` declares the GCC as `clock-controller@1800000`,
compatible `"qcom,gcc-msm8937"`, and that string is matched by
`drivers/clk/qcom/gcc-msm8917.c`. That driver contains **zero** occurrences
of the string `ULTAUDIO`, and `include/dt-bindings/clock/qcom,gcc-msm8917.h`
defines **none** of the `GCC_ULTAUDIO_*` tokens. So the seven clocks

| clock-name        | needed by                            | how                |
|-------------------|--------------------------------------|--------------------|
| `ahbix-clk`       | `apq8016.c:186` `devm_clk_get()`      | required           |
| `pcnoc-mport-clk` | `apq8016.c:266` `devm_clk_bulk_get()`| required           |
| `pcnoc-sway-clk`  | `apq8016.c:267` `devm_clk_bulk_get()`| required           |
| `mi2s-bit-clk0`   | `lpass-cpu.c:1226` `devm_clk_get()`  | required           |
| `mi2s-bit-clk1`   | `lpass-cpu.c:1226` `devm_clk_get()`  | required           |
| `mi2s-bit-clk2`   | `lpass-cpu.c:1226` `devm_clk_get()`  | required           |
| `mi2s-bit-clk3`   | `lpass-cpu.c:1226` `devm_clk_get()`  | required           |

can never resolve. `lpass-cpu` and `apq8016` both sit on `-EPROBE_DEFER`
forever and the sound card never appears. `lpass-cpu.c:1228` even prints the
failing name, so this is directly observable in `dmesg`.

The clocks exist in `drivers/clk/qcom/gcc-msm8916.c` as plain CBCR gate
branches in the AON region, and **the register offsets hold on this board**:
the GCC node is `clock-controller@1800000` in both `msm8916.dtsi` and
`msm8937.dtsi`, and reading `0x181c004` (= GCC `0x1800000` + `0x1c004`,
`gcc_ultaudio_pcnoc_sway_clk`) returned `0x20008001` with bit 0 set.

---

## 2. Design: separate provider over a NON-EXCLUSIVE map of the GCC window

The chosen design is the one in the brief: **a second platform device, its own
DT node with no `reg`, a `devm_of_iomap()` mapping of the existing GCC window,
its own regmap, and exactly the seven clocks.**

Why each part:

**Why not just fix `gcc-msm8917.c` in tree?** Because the kernel may not be
rebuilt. `gcc-msm8917.c` is built into `vmlinux` and holds the whole GCC
window through `request_mem_region()`. A second device cannot take that
resource, so the module cannot own it.

**How the window is reached.** `devm_platform_ioremap_resource_bindmap()` —
the API that exists precisely to let a second driver reuse an already-claimed
resource — **does not exist in this tree**. Verified:

```
grep -rn "ioremap_resource_bindmap" fk/include/    ->  0 hits
```

So `devm_platform_ioremap_resource()` / `devm_ioremap_resource()` are both
out (they return `-EBUSY`). What is used instead:

```c
regs = devm_of_iomap(dev, gcc_np, 0, NULL);   /* gcc_np from a "qcom,gcc" phandle */
aon->map = devm_regmap_init_mmio(dev, regs, &cfg);
```

`devm_of_iomap()` is `of_iomap()` + `devm_ioremap()`. `devm_ioremap()` is a
plain `ioremap()` with a devres free hook and **never calls
`request_mem_region()`**, so it cannot conflict. The two drivers end up with
two independent mappings and two independent regmaps of the same physical
window; they only ever touch disjoint registers (the in-tree driver has no
clock in `0x1c000..0x1c098` at all).

**Why the DT node has no `reg`.** Because the addresses we need are *inside*
`0x01800000/0x80000`. Giving our node a `reg` would make `of_platform_populate()`
hand us a resource that overlaps the GCC's. Instead the node carries a
phandle, `qcom,gcc = <&gcc>;`, and no `reg` at all, so it claims no address
space. This is the "no overlapping reg" requirement satisfied literally.

**Why 7 clocks and not 114.** The in-tree driver already publishes the other
114. Re-registering them would collide in the global clk name hash and in the
OF provider list.

---

## 3. no-op provider vs. real gates: the recommendation is REAL GATES

This is the decision the brief asked to be made explicitly.

### Does anything besides `lpass-cpu` depend on these clocks actually being toggled?

Yes, and it is more than `lpass-cpu`. `apq8016.c`'s `apq8016_lpass_init()`
- which `lpass-cpu.c` calls from `variant->init` - touches **three** of the
seven and treats every step as fatal:

```c
	ret = devm_clk_bulk_get(dev, drvdata->num_clks, drvdata->clks);   /* pcnoc-mport, pcnoc-sway */
	ret = clk_bulk_prepare_enable(drvdata->num_clks, drvdata->clks); /* prepare + enable, hard error */

	drvdata->ahbix_clk = devm_clk_get(dev, "ahbix-clk");            /* hard error */
	ret = clk_set_rate(drvdata->ahbix_clk, LPASS_AHBIX_CLOCK_FREQUENCY);
	if (ret) { ... goto err_ahbix_clk; }                              /* PROBE FAILS */
	ret = clk_prepare_enable(drvdata->ahbix_clk);                   /* hard error */
```

and `lpass-cpu.c` drives the four bit clocks:

```c
	ret = clk_set_rate(drvdata->mi2s_bit_clk[id], rate * bitwidth * 2);
	if (ret) { dev_err(...); return ret; }        /* lpass-cpu.c:288-295, DAI hw_params fails */
	ret = clk_enable(drvdata->mi2s_bit_clk[id]); /* lpass-cpu.c:334, prepare */
	clk_disable(drvdata->mi2s_bit_clk[dai->driver->id]); /* lpass-cpu.c:359 */
```

### The finding that decides it

A `fixed-clock` DT stub — which is what `pine-audio.dts` currently uses — is
a **parentless** clock whose `hw->ops` has **no `.set_rate`**.
`clk_core_set_rate_nolock()` handles that case explicitly:

```c
	if (!core->parent) {
		if (core->hw->ops->set_rate)
			ret = clk_hw_set_rate(core->hw, core, rate);
		else
			ret = -EINVAL;          /* <-- fixed-clock lands here */
		return ret;
	}
```

So `clk_set_rate()` on a fixed-clock stub returns `-EINVAL`, and the two
call sites above **both propagate it**. The existing fixed-clock stubs are
therefore not a working fallback: even if they made `devm_clk_get()` succeed,
`apq8016_lpass_init()` would still fail at line 194 and take the whole CPU
DAI down with it. *(I could not execute this tree's `drivers/clk/clk.c` — it
is absent from `fk/` — so the exact core code path is argued from the header
contracts, not read. The module is written so it returns 0 under either
reading; see §5.)*

### The recommendation

**Real gates**, for three reasons:

1. `clk_set_rate()` must return 0. That is a hard requirement of
   `apq8016.c:194` and `lpass-cpu.c:288`. A naive no-op `clk_ops` with
   `.enable = NULL` and no `.set_rate` returns `-EINVAL` and fails probe.
2. Real gates are what makes the enable/disable *mean* something.
   `clk_enable()` in `lpass_cpu_daiops_prepare()` / `_trigger()` exists to
   ungate the LPAIF bit clock before DMA and regate it after. With a no-op
   provider that call becomes a no-op and we are relying entirely on the
   bootloader having left `0x1c000..0x1c098` in a usable state. We know one
   register (0x1c004) is ungated. We do **not** know the other five.
3. The write is one bit, in a register whose offset is verified live on this
   board, in the same register the in-tree driver already pokes 114 times per
   boot. There is no realistic way this wedges the AON.

### Cost of real gates, stated plainly

`clk_disable()` on shutdown clears `BIT(0)`. If the bootloader had, say,
`gcc_ultaudio_lpaif_pri_i2s_clk` running and something else on this SoC needs
it outside LPASS, we would now regate it. Nothing in this tree does - only
`lpass-cpu` consumes these seven names - so this is a theoretical concern
only.

`clk_set_rate()` is a **no-op that returns 0**. The LPAIF bit rate is produced
by the RCG2 at `0x1c054` / `0x1c06c` / `0x1c084`, and its parents require
`GCC_GPLL1` and `GCC_XO_GPLL0_BIMC`. `gcc-msm8917.c` has **no `gpll1`**, no
`bimc_pll_vote`, no `gpll1_vote` and no `sleep_clk`; `missing.txt` in `fk/`
lists exactly these. So the bit rate stays whatever the bootloader programmed.
See §7.

---

## 4. Build and load

### Build

```sh
cd /data/data/com.termux/files/home/pine-audio/clk

# confirm the two config prerequisites
grep -E 'CONFIG_MODULES=|CONFIG_MODVERSIONS' /path/to/linux/.config
#   CONFIG_MODULES=y
#   # CONFIG_MODVERSIONS is not set      <- required, no Module.symvers needed

# build against a prepared kernel source tree (scripts must be run at least once)
make -C /path/to/linux M=$PWD modules
```

Output: `gcc-sdm439-oot.ko` plus `gcc-sdm439-oot.mod`, `.mod.c`, `.symvers`,
`.order`, `modules.order`, `.cache.mk`.

Clean:

```sh
make -C /path/to/linux M=$PWD clean
```

### Load

```sh
adb push gcc-sdm439-oot.ko /data/local/tmp/
adb shell su -c 'mount -o remount,rw /system'
adb push gcc-sdm439-oot.ko /system/lib/modules/
adb shell su -c 'insmod /system/lib/modules/gcc-sdm439-oot.ko'
```

or without root paths:

```sh
adb push gcc-sdm439-oot.ko /data/local/tmp/
adb shell su -c 'insmod /data/local/tmp/gcc-sdm439-oot.ko'
```

Unload:

```sh
adb shell su -c 'rmmod gcc_sdm439_oot'
```

`module_platform_driver()` registers the platform driver from `module_init`,
so `insmod` immediately runs `probe`, which registers the OF clock provider.
`really_probe()` calls `driver_deferred_probe_trigger()` on success, which
replays every driver parked on `-EPROBE_DEFER` - that is the mechanism that
re-probes `lpass-cpu`.

---

## 5. Why the reference parent clock exists

`lpass-cpu` and `apq8016` both require `clk_set_rate()` to succeed. The clk
core can return `-EINVAL` from two different places:

* `!core->parent && !hw->ops->set_rate` → `-EINVAL` directly;
* with a parent, the core recurses into
  `clk_core_set_rate_nolock(parent, rate)`, which takes the first branch.

So the leaf needs a parent, **and** that parent needs a `.set_rate`. The
module therefore registers one private, parentless, fixed-rate reference
(`gcc_sdm439_aon_audio_ref`, `qcom,aon-audio-hz`, default 133330000) with a
`.set_rate` that returns 0 and a `.recalc_rate` that reports the rate, and
hangs all six gates off it. `clk_set_rate()` then returns 0 no matter which
order the core does the checks in.

The reference clock is deliberately **parentless** - `parent_hws = NULL`,
`num_parents = 0`. Pointing it at itself would make `core->parent ==
&aon->ref` and `clk_core_prepare()` would recurse into itself.

---

## 6. DT

`dts-fragment.dtsi` has two parts. Apply both.

**Note on the clock IDs.** It does *not* `#include`
`<dt-bindings/clock/qcom,gcc-msm8916.h>`, because `msm8937.dtsi:7` already
includes `<dt-bindings/clock/qcom,gcc-msm8917.h>` and the two headers declare
**119 macros with the same name and different values**, so including both in
one DT translation unit is a cpp redefinition error. The numbers are copied
into locally-prefixed macros instead, and they are only meaningful against
`&gcc_sdm439_aon_audio`, which has its own ID space.

Rebuild the DTB:

```sh
cd /path/to/linux
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-android- \
     pine-xiaomi-sdm439-audio.dtb

adb push arch/arm64/boot/dts/qcom/pine-xiaomi-sdm439-audio.dtb /data/local/tmp/
adb shell su -c 'dd if=/data/local/tmp/pine-xiaomi-sdm439-audio.dtb of=/dev/block/by-name/boot_a/by-name/dtbo_a'
```

Make sure the provider node is reachable by `of_platform_populate()`; it is
placed at the DT root (outside `&soc`) with `status = "okay"`, so it needs no
extra parent.

---

## 7. What you get, and what you do not

**You get**

* `lpass-cpu` probes: the seven `devm_clk_get()` / `devm_clk_bulk_get()`
  calls resolve, `apq8016_lpass_init()` passes, and the CPU DAI registers.
* `lpass_codec`, `apq8016_sbc` and the analog WCD path become reachable.
* The LPAIF bit clocks and the AHB/PCNOC fabric ports are explicitly ungated
  before every stream and regated after every stop, at verified offsets.

**You do not get**

* **The LPAIF bit rate is not reprogrammed.** `clk_set_rate()` returns 0 and
  writes nothing. The rate comes from the LPAIF RCG2s at `0x1c054` /
  `0x1c06c` / `0x1c084`, whose parents need `GCC_GPLL1` and
  `GCC_XO_GPLL0_BIMC`, neither of which exists in `gcc-msm8917.c`. Whatever
  the bootloader left there is what runs. Practical consequence: play at the
  rate that is already programmed (the stock ROM drives the analog path, so
  48 kHz / 2ch / 16 bit is the safe bet), and if playback comes out
  pitch-shifted or silent, read `0x181c054`, `0x181c06c`, `0x181c084` and
  compare against `devmem2 0x181c054 32` on a working ROM.
* **`&lpass_codec` is not fixed by this.** The fork's `lpass_codec` node asks
  `&q6afecc` for `LPASS_CLK_ID_INTERNAL_DIGICAL_CODEC_CORE`, and no file in
  the tree binds `"qcom,q6afe-clocks"` (`grep -rl q6afe-clocks fk/drivers` is
  empty), so that node is still dead weight. Fixing it means a digcodec gate -
  `0x1c088`/`GCC_CODEC_DIGCODEC_CLK`, which is defined in the in-tree
  header as token 198 (see §8) - added to this module, or the in-tree
  `gcc-sdm439.c` path.
* The PDM microphone, the speaker amplifier and everything else that needs the
  QDSP6 AFE. Out of scope; the AFE does not exist in this kernel.

---

## 8. In-tree fixes applied under `fk/`

| file | change |
|---|---|
| `drivers/clk/qcom/Makefile` | added `obj-$(CONFIG_COMMON_CLK_QCOM_GCC_SDM439) += gcc-sdm439.o` (it was missing; the `Kconfig` symbol existed with no user) |
| `include/dt-bindings/clock/qcom,gcc-sdm439.h` | **renumbered 183..199** and added `CODEC_DIGCODEC_CLK_SRC`, `GCC_CODEC_DIGCODEC_CLK`, `GCC_MSS_Q6_BIMC_AXI_CLK` |
| `drivers/clk/qcom/gcc-sdm439.c` | added the missing `#include <dt-bindings/clock/qcom,gcc-sdm439.h>`; moved the seven `GCC_ULTAUDIO_*` entries from `gcc_msm8917_clocks[]` to `gcc_msm8937_clocks[]`; removed the duplicate `"qcom,gcc-sdm439"` match entry |

**Why the renumbering was mandatory.** `gcc-sdm439.c` has one
designator-indexed `clk_regmap` table per variant, indexed by both headers'
tokens at once. `qcom,gcc-msm8917.h` already spends 150/151/154/156/157/158
on `JPEG0_CLK_SRC`, `MCLK0_CLK_SRC`, `MDP_CLK_SRC`, `PDM2_CLK_SRC`,
`SDCC1_APPS_CLK_SRC`, `SDCC1_ICE_CORE_CLK_SRC`. Copying msm8916's values
would have silently overwritten `[JPEG0_CLK_SRC]` and taken the JPEG pipeline
down. The new space starts at 183, above msm8917's maximum of 182.

The module (`gcc-sdm439-oot.c`) is unaffected: it owns its own DT node and
therefore its own ID space, and it uses the msm8916 values verbatim because
that is what `dts-fragment.dtsi` uses.

### `gcc-sdm439.c` still does not build - and cannot, on this tree

Beyond the mechanical fixes above, its ported objects reference three symbols
that exist nowhere in this tree:

| referenced | at | defined? |
|---|---|---|
| `gpll1_vote` | `gcc-sdm439.c:3626`, `:3632`, `:3640` | no |
| `bimc_pll_vote` | `gcc-sdm439.c:3620` | no |
| `gcc_codec_digcodec_clk` | removed; noted in-array | no |

`gcc-msm8917.c` has no `gpll1`, no `gpll0_bimc` and no `sleep_clk`, so the
RCG2 parents of the AON audio clocks are not reconstructible from what this
tree provides. `gcc-sdm439.c` is therefore parked as a draft with the blockers
named in-place; the out-of-tree module is the deliverable that actually runs,
because it does not need those parents.

---

## 9. Verification

**Checked mechanically, in `fk/`:**

* `devm_platform_ioremap_resource_bindmap` — **absent**; `platform_ioremap_resource_bindmap` — absent. Hence `devm_of_iomap`.
* `devm_regmap_init_mmio` — `include/linux/regmap.h:1187`. `regmap_update_bits` `:1341`, `regmap_read` `:1327`, `regmap_write` `:1312`, `struct regmap_config` `:409`, `REGMAP_ENDIAN_DEFAULT` `:217`.
* `devm_of_iomap` — `include/linux/device/devres.h:120` (and `:137` for the `!HAS_IOMEM` case). `devm_ioremap_resource` `:117` exists but is unusable here.
* `devm_clk_hw_register` — `include/linux/clk-provider.h:1359`, `__must_check`.
* `devm_of_clk_add_hw_provider` / `of_clk_add_hw_provider` / `of_clk_del_provider` — `clk-provider.h:1617` / `:1613` / `:1621`. Note `struct of_clk_provider` is **not** exported to out-of-tree code in this tree (zero hits under `fk/include`), so `of_clk_src_onecell_get()` is unusable; a hand-written `.get` callback taking `struct of_phandle_args *` is used instead, which is the pattern `drivers/clk/clk-gpio.c` uses.
* `struct of_phandle_args { np; args_count; args[MAX_PHANDLE_ARGS]; }` — `include/linux/of.h:71`.
* `struct clk_ops` — `clk-provider.h`, and in this tree `set_rate` is
  `int (*)(struct clk_hw *, unsigned long rate, unsigned long parent_rate)`
  (three arguments, not two). `recalc_rate` is
  `unsigned long (*)(struct clk_hw *, unsigned long parent_rate)`.
* `struct clk_init_data { name; ops; parent_names; parent_data; parent_hws; num_parents; flags; }` — `clk-provider.h`.
* `CLK_SET_RATE_PARENT` — `clk-provider.h:21`.
* `of_parse_phandle` — `include/linux/of.h:1027`. `of_property_read_u32` `:1465`. `of_match_ptr` present.
* `devm_kzalloc` — `include/linux/device/devres.h:48`. `module_platform_driver` — `include/linux/platform_device.h:325`.
* `dev_err_probe` — `include/linux/dev_printk.h:278`.
* Clock gates take `void __iomem *` in this tree (`__clk_hw_register_gate`,
  `clk-provider.h:535`), and `regmap_gate_clk_register` / `struct
  clk_gate_registers` **no longer exist** anywhere under `fk/include`. That is
  why the module implements its own four clk_ops over its own regmap instead
  of calling into `drivers/clk/qcom/clk-regmap.c`.

**Checked against the real `gcc-msm8916.c`:** the six offsets `0x1c000`,
`0x1c004`, `0x1c028`, `0x1c068`, `0x1c080`, `0x1c098` and `enable_mask = BIT(0)`
are `halt_reg = enable_reg` with `BIT(0)`, at lines 1271 (ahbfabric), 1350
(pri i2s), 1381 (sec i2s), 1412 (aux i2s), 1520 (pcnoc mport) and 1536
(pcnoc sway) of `drivers/clk/qcom/gcc-msm8916.c`.

**Checked against the real DT:** `mainline msm8916.dtsi:2073` lists the lpass
node with exactly these seven names in exactly this order, with
`mi2s-bit-clk0` and `mi2s-bit-clk1` pointing at the same
`GCC_ULTAUDIO_LPAIF_PRI_I2S_CLK`. `dts-fragment.dtsi` mirrors that.

**Parsed with both compilers.** The module body was fed through clang 21 and
gcc with `-Wall -Wextra -Wshadow -Wmissing-prototypes -Wstrict-prototypes`
against hand-written stubs whose signatures are copied verbatim from the
headers above: **0 errors, 0 warnings on both**. The kernel headers in `fk/`
cannot be used directly because `fk/include/asm-generic/` has no `byteorder.h`
and there is no `asm/` directory, so the shim route was the only available
compile.

### Could NOT be verified

1. **`clk_set_rate()` returning `-EINVAL` for a parentless clock without
   `.set_rate`.** `drivers/clk/clk.c` and `drivers/clk/clk-gate.c` are absent
   from `fk/`. This is argued from `include/linux/clk.h` contracts and from
   the fact that both call sites propagate the return value. The module
   returns 0 under either reading, so this uncertainty does not change the
   outcome.
2. **`EXPORT_SYMBOL` status** of `devm_clk_hw_register`,
   `devm_of_clk_add_hw_provider`, `devm_of_iomap` and `devm_regmap_init_mmio`.
   `drivers/clk/clk.c` and `drivers/base/devres.c` are absent. Since
   `CONFIG_MODVERSIONS` is off and `gcc-msm8917.c` is built into `vmlinux`,
   built-in symbols resolve at link time regardless; at worst modpost emits
   `"module ... uses symbol ... which is not exported"`, which is a warning.
   Watch the build log.
3. **Whether `CONFIG_REGULATOR`, `CONFIG_REGMAP_MMIO`, `CONFIG_OF` are set**
   in the shipped `.config`. `REGMAP_MMIO` must be, since `gcc-msm8917.c`
   uses `devm_regmap_init_mmio()` itself.
4. **Whether `lpass-cpu`, `apq8016`, `lpass_codec` and `msm8916-wcd-analog`
   are actually built into the running kernel.** They are not in `fk/`, which
   only carries `drivers/clk/`, `arch/arm64/boot/dts/`, `include/` and
   `scripts/`. Confirm with `grep -E 'L1PASS|LPASS|COMMON_CLK_QCOM' .config`
   and with `ls /sys/bus/platform/drivers/ | grep -i lpass` on the device.
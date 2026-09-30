# SPDX-License-Identifier: GPL-2.0
#
# Out-of-tree build for the SDM439 LPAIF CPU DAI / PCM platform
# (Redmi 7A "pine", SDM439, msm89x7-mainline/linux @ msm89x7/7.1.3, arm64).
#
# Build against a configured+prepared kernel source tree:
#
#     make -C /path/to/linux-7.1.3 M=$PWD modules
#
# Requires only CONFIG_MODULES=y. CONFIG_MODVERSIONS must be OFF (it is, in
# this kernel) so the module links without Module.symvers:
#
#     grep MODVERSIONS /path/to/linux-7.1.3/.config
#     #   # CONFIG_MODVERSIONS is not set
#
# No in-tree change is required. snd/soc/qcom/lpass-cpu.o,
# snd/soc/qcom/lpass-apq8016.o, snd/soc/qcom/lpass-platform.o and
# snd/soc/qcom/apq8016.o MUST NOT be loaded at the same time as this module:
# they bind the same two compatibles and would race for the same DT node.
# See README.md.
#
# Single translation unit, no in-tree headers. The LPAIF register map is
# re-expressed locally from sound/soc/qcom/lpass-apq8016.c and
# sound/soc/qcom/lpass-lpaif-reg.h, so no -I into the kernel's sound/soc/qcom
# directory is needed and lpass.h is not required.

obj-m += sdm439_lpaif.o
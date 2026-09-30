# SPDX-License-Identifier: GPL-2.0
#
# Out-of-tree build for the SDM439 AON audio / LPASS clock provider.
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
obj-m += gcc-sdm439-oot.o
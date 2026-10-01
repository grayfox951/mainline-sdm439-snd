#!/bin/sh
# Build the audio modules for Xiaomi Redmi 7A (pine), SDM439.
#
# Built on the mainline Linux PC, deployed to the phone over SSH/SCP.
# No ADB anywhere - the phone runs a full Linux mainline system.
#
# No kernel rebuild is required: CONFIG_MODVERSIONS is off in 7.1.3-msm89x7,
# so an out-of-tree module only needs `make modules_prepare` to have been run
# once in the kernel tree.
#
# SPDX-License-Identifier: GPL-2.0
#
# Edited by: grayfox951 <admin@dnr.qzz.io>

set -e

# ---- configuration ------------------------------------------------------
KERNEL_SRC="${KERNEL_SRC:-/path/to/msm89x7-linux}"     # <-- EDIT ME
CROSS="${CROSS_COMPILE:-aarch64-linux-gnu-}"
ARCH=arm64
HERE=$(cd "$(dirname "$0")" && pwd)

# ОСТОРОЖНО С ПАМЯТЬЮ.
# make по умолчанию берёт все ядра (здесь их 8) и вешает телефон от нехватки
# RAM. Ставим 2. Для сборки одного .c модуля этого хватает с запасом, а
# modules_prepare на 70k-файловом дереве переживает уверенно.
# Повышай только если памяти реально хватает: free -m перед запуском.
JOBS="${JOBS:-2}"

# target phone
PHONE_HOST="${PHONE_HOST:-192.168.1.15}"
PHONE_USER="${PHONE_USER:-alex}"
PHONE_PORT="${PHONE_PORT:-22}"
PHONE_TMP=/data/local/tmp

die() { echo "ОШИБКА: $*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "нет команды: $1"; }

echo "=== проверки ==="
[ -d "$KERNEL_SRC" ] || die "нет дерева ядра: $KERNEL_SRC"
[ -f "$KERNEL_SRC/Makefile" ] || die "это не дерево ядра: $KERNEL_SRC"
[ -f "$KERNEL_SRC/include/generated/autoconf.h" ] \
  || die "не выполнен make modules_prepare в дереве ядра"
need "${CROSS}gcc"
need ssh
need scp

ver=$(sed -n 's/^VERSION = //p;s/^PATCHLEVEL = //p;s/^SUBLEVEL = //p' \
      "$KERNEL_SRC/Makefile" 2>/dev/null | tr '\n' '.')
modv=$(grep -c '^CONFIG_MODVERSIONS=y' "$KERNEL_SRC/.config" 2>/dev/null || echo 0)
echo "    ядро:        $KERNEL_SRC  ($ver)"
echo "    тулчейн:     ${CROSS}gcc"
echo "    MODVERSIONS: $modv  $([ "$modv" = 0 ] && echo '(ок, пересборка не нужна)' || echo '(ВНИМАНИЕ: нужен Module.symvers)')"

# проверка памяти перед сборкой - телефон уже ребутался от нехватки RAM
free_m=$(awk '/MemAvailable/{print $2}' /proc/meminfo 2>/dev/null || echo 0)
echo "    свободно RAM: $((free_m / 1024)) MB   (потоков: $JOBS)"
[ "$free_m" -lt 600000 ] && echo "    ВНИМАНИЕ: мало памяти, уменьши JOBS" >&2"

# Если kernel ещё не подготовлен, один раз выполни (низкопотоково!):
#   make -C "$KERNEL_SRC" ARCH=arm64 CROSS_COMPILE="$CROSS" -j"$JOBS" modules_prepare

# ---- build, in dependency order -----------------------------------------
# clk first: the LPAIF DAI hard-fails on the 7 AON audio clocks.
for m in clk lpaif aw87329; do
  echo
  echo "=== сборка $m ==="
  make -C "$KERNEL_SRC" M="$HERE/$m" ARCH="$ARCH" CROSS_COMPILE="$CROSS" \
       -j"$JOBS" modules
done

# ---- check --------------------------------------------------------------
echo
echo "=== результат ==="
MISSING=0
for m in clk lpaif aw87329; do
  ko=$(find "$HERE/$m" -name '*.ko' 2>/dev/null | head -1)
  if [ -n "$ko" ]; then
    echo "  OK        $(basename "$ko")  ($(stat -c%s "$ko") байт)"
  else
    echo "  ОТСУТСТВУЕТ  $m/*.ko"; MISSING=1
  fi
done
[ "$MISSING" = 0 ] || die "не все модули собрались, смотрите ошибки выше"

# ---- deploy over ssh ----------------------------------------------------
echo
echo "=== копирование на телефон $PHONE_USER@$PHONE_HOST ==="
for m in clk lpaif aw87329; do
  ko=$(find "$HERE/$m" -name '*.ko' | head -1)
  scp -P "$PHONE_PORT" "$ko" "$PHONE_USER@$PHONE_HOST:$PHONE_TMP/" || die "scp не удался"
  echo "  отправлен  $(basename "$ko")"
done

cat <<EOF

Загрузить на телефоне (порядок важен - сначала часы):

  ssh -p $PHONE_PORT $PHONE_USER@$PHONE_HOST
  sudo insmod $PHONE_TMP/gcc-sdm439-oot.ko
  sudo insmod $PHONE_TMP/sdm439-lpaif.ko
  sudo insmod $PHONE_TMP/aw87329.ko

Проверка:

  cat /sys/bus/i2c/devices/2-0058/aw87329/chipid      # ожидаем 0x39
  dmesg | grep -iE "lpass|wcd|apq8016"
  ls /dev/snd/
  aplay -l ; arecord -l

Откат:

  sudo rmmod aw87329 sdm439_lpaif gcc_sdm439_oot

ВАЖНО: база LPASS пока НЕИЗВЕСТНА, в dts-фрагменте стоит заглушка msm8916.
При write_en=0 модуль только читает и зависнуть не может.
EOF
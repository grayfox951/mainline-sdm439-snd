#!/bin/sh
# $1 = адрес hex -> читает 8 слов подряд от него
A="$1"
off=0
while [ $off -lt 32 ]; do
  a=$(printf "%x" $(( 0x$A + off )))
  r=$(devmem2 "0x$a" 2>&1 | tail -1)
  case "$r" in *rror*|*ail*|*apped*) r="DEAD";; *0x*) ;; *) r="?$r";; esac
  printf "  0x%s (+%2d) = %s\n" "$a" "$off" "$r"
  off=$((off+4))
done

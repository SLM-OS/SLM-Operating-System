#!/bin/bash
# usage: textcmp.sh <elfA> <elfB> ; per-section byte compare
for s in .text .rodata .data; do
  for e in "$1" "$2"; do aarch64-none-elf-objcopy -O binary -j $s "$e" /dev/stdout | sha256sum | cut -c1-16; done | paste -sd' ' | sed "s/^/$s /"
  n=$(cmp -l <(aarch64-none-elf-objcopy -O binary -j $s "$1" /dev/stdout) <(aarch64-none-elf-objcopy -O binary -j $s "$2" /dev/stdout) 2>&1 | wc -l)
  echo "$s differing bytes: $n (sizes $(aarch64-none-elf-objcopy -O binary -j $s "$1" /dev/stdout | wc -c) / $(aarch64-none-elf-objcopy -O binary -j $s "$2" /dev/stdout | wc -c))"
done

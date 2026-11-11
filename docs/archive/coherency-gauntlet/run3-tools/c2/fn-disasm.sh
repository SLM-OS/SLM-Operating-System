#!/bin/bash
# Disassemble one function from an ELF with addresses normalised, so two
# builds can be diffed. Usage: fn-disasm.sh <elf> <function> <out>
set -u
export PATH=/opt/arm-gnu-toolchain/bin:$PATH
aarch64-none-elf-objdump -d --no-show-raw-insn "$1" \
    | awk -v f="<$2>:" '$0 ~ f, /^$/' \
    | sed 's/^ *[0-9a-f]*:[[:space:]]*//; s/[0-9a-f]\{8,\} <[^>]*>/ADDR/' > "$3"
echo "$3: $(wc -l < "$3") lines, wfi=$(grep -c '^wfi' "$3") wfe=$(grep -c '^wfe' "$3")"

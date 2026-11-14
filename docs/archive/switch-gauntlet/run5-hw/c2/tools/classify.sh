#!/bin/bash
# Classify C2 QEMU run files: first event, iterations before it.
# COMPLETE = the filter summary printed; ASSERT = first [ITER] [FAIL];
# FAULT = an exception/panic dump; HANG = timeout with neither.
for f in "$@"; do
  b=$(basename "$f")
  # passes before the first non-pass
  first_fail_iter=$(grep -a -o '\[ITER [0-9]*/[0-9]*\] *\[FAIL\]' "$f" | head -1 | grep -o '[0-9]*' | head -1)
  npass_before=$(awk '/\[ITER [0-9]+\/[0-9]+\] *\[FAIL\]/{exit} /\[ITER [0-9]+\/[0-9]+\] *\[PASS\]/{n++} END{print n+0}' "$f")
  dump=0; grep -a -q 'KERNEL PANIC\|Synchronous Exception\|Task Name:\|EC: ' "$f" && dump=1
  done_=0; grep -a -q '^\[FILTER\] Summary' "$f" && done_=1
  if [ -n "$first_fail_iter" ]; then ev=ASSERT; at=$first_fail_iter; npass_before=$((first_fail_iter-1))
  elif [ $dump = 1 ]; then ev=FAULT; at=$((npass_before+1))
  elif [ $done_ = 1 ]; then ev=COMPLETE; at=-
  else ev=HANG; at=$((npass_before+1)); fi
  [ $dump = 1 ] && [ "$ev" = ASSERT ] && ev=ASSERT+FAULT
  load=$(sed -n 5p "$f" | grep -o 'load [0-9.]*' | head -1)
  echo "$b $ev at_iter=$at passes_before=$npass_before $load"
done

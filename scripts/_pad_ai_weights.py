#!/usr/bin/env python3
"""Pad an AI weight .c file's output layer to AI_MLP_LAYER3_MAX_ROWS.

Helper for `import_ai_weights.sh` — the sibling-repo `_real` export
emits the output layer (`ai_<model>_w3` / `ai_<model>_b3`) sized to
the platform's actual `n_actions`, e.g. 24 for Pi 5. The kernel's
header (`kernel/sched/ai/ai_weights.h`) sizes the extern declarations
for `AI_MLP_LAYER3_MAX_ROWS = 42` (Jetson's space, the maximum across
all supported platforms) so the same header serves every platform.
Pad the trailing rows with zeros — only the first `AI_SCHED_N_ACTIONS`
rows are read by the kernel at inference time, so the padding is
never consumed and the per-platform model behaviour is unchanged.

Usage: _pad_ai_weights.py <file.c> <model> <max_rows> <layer3_in>

  <model>     "mlp" or "ppo" (selects symbol prefix)
  <max_rows>  Row count to pad to (typically 42 = AI_MLP_LAYER3_MAX_ROWS)
  <layer3_in> Layer 3 input dim (typically 128)
"""
import re
import sys
from pathlib import Path


def pad_array(src: str, name: str, want_rows: int, row_stride: int) -> str:
    pat = re.compile(
        rf'(const float {re.escape(name)}\[)(\d+)((?:\s*\*\s*\d+)?)(\] = \{{)(.*?)(\n\}};)',
        re.DOTALL,
    )
    m = pat.search(src)
    if not m:
        raise RuntimeError(f"could not locate definition of {name}")
    values = [v.strip() for v in m.group(5).replace('\n', ' ').split(',') if v.strip()]
    want_len = want_rows * row_stride
    have = len(values)
    if have == want_len:
        return src  # already correct
    if have > want_len:
        raise RuntimeError(f"{name} has {have} > {want_len}; refusing to truncate")
    values += ['0x0p+0'] * (want_len - have)
    rows = []
    for i in range(0, len(values), 6):
        chunk = values[i:i + 6]
        comma = ',' if i + 6 < len(values) else ''
        rows.append('    ' + ', '.join(chunk) + comma)
    new_size = (
        f"{want_rows}{m.group(3)}" if m.group(3) else str(want_rows)
    )
    new_def = m.group(1) + new_size + '] = {\n' + '\n'.join(rows) + m.group(6)
    return src[:m.start()] + new_def + src[m.end():]


if __name__ == "__main__":
    if len(sys.argv) != 5:
        sys.exit(__doc__.strip().splitlines()[-1])
    path = Path(sys.argv[1])
    model = sys.argv[2]
    max_rows = int(sys.argv[3])
    layer3_in = int(sys.argv[4])
    src = path.read_text()
    src = pad_array(src, f'ai_{model}_w3', max_rows, layer3_in)
    src = pad_array(src, f'ai_{model}_b3', max_rows, 1)
    path.write_text(src)

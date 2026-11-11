# Tips of run-3 branches deleted at close-out

Each patch is `git format-patch -1` of the named commit. To rebuild a branch, apply in order on its base.

| Branch (deleted) | Head | Base | Patches, in order |
|---|---|---|---|
| `c2/cell-a-ncoff` | 6608cbd0 | cbab2fc6 (run 2's NC_AXIS=OFF cell, archived as `run2-tools/deleted-branch-tips/c2_steal-instrument-1203-ncoff-cbab2fc6.patch` on 10f8688e) | `c2_cell-a-ncoff-6608cbd0.patch` |
| `c2/cell-b-ncoff-v1` | 579b155a | 6608cbd0 | A's patch, then `c2_cell-b-ncoff-v1-579b155a.patch` |
| `c2/cell-de-removal` | 8fc7c88a | 79c10261 (`coherency-remove-nc-axis`) | `c2_cell-de-removal-parent-3da0dd9d.patch` (983bddc9 cherry-picked), `c2_cell-de-removal-8fc7c88a.patch` |
| `c2/cell-g-removal-v1` | 0dfaae0c | 8fc7c88a | D/E's two patches, then `c2_cell-g-removal-v1-0dfaae0c.patch` |

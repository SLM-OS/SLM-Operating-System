# Tips of run-3 branches deleted at close-out

Each patch is `git format-patch -1` of the named commit. To rebuild a branch, apply in order on its base.

| Branch (deleted) | Head | Base | Patches, in order |
|---|---|---|---|
| `c2/cell-a-ncoff` | 6608cbd0 | cbab2fc6 (run 2's NC_AXIS=OFF cell, archived as `run2-tools/deleted-branch-tips/c2_steal-instrument-1203-ncoff-cbab2fc6.patch` on 10f8688e) | `c2_cell-a-ncoff-6608cbd0.patch` |
| `c2/cell-b-ncoff-v1` | 579b155a | 6608cbd0 | A's patch, then `c2_cell-b-ncoff-v1-579b155a.patch` |
| `c2/cell-de-removal` | 8fc7c88a | 79c10261 (`coherency-remove-nc-axis`) | `c2_cell-de-removal-parent-3da0dd9d.patch` (983bddc9 cherry-picked), `c2_cell-de-removal-8fc7c88a.patch` |
| `c2/cell-g-removal-v1` | 0dfaae0c | 8fc7c88a | D/E's two patches, then `c2_cell-g-removal-v1-0dfaae0c.patch` |

## Deleted at the close-out (2026-11-11)

| Branch (deleted) | Head | Base | Archived |
|---|---|---|---|
| `c2/fixc-removal` | c77156b2 | 5d70fdbd (`coherency-remove-nc-axis`) | `c2_fixc-removal-c77156b2.patch` (1 commit: the `--assert-shares` commit) |
| `c2/fixc-removal-fix` | 5fe8c70b | 5d70fdbd | `c2_fixc-removal-fix-5fe8c70b.patch` (2 commits: the same plus idle WFE) |
| `c1/boot-path-plus-helper` | 638e297d | 59a85926 | not archived: its content (#1209 + #1215) is on main |
| `fix-boot-1194-1197-1198` | 58cc6832 | main 85f51ffd | not archived: squash-merged as 8a27711d (#1208); tip kept by GitHub as the PR head |
| `fix/boot-path-1193` | b849e2ee | 8a27711d | squash-merged as cf34214d (#1209) |
| `c1/test-wait-helper` | a03fc75b | 85f51ffd | squash-merged as 73d8deb7 (#1215) |
| `c2/steal-instrument-1203` | 1b27eea4 | 85f51ffd | squash-merged as 88b28c15 (#1217) |
| `docs/labctl-serializes-calls` | 90f0d9e3 | 85f51ffd | squash-merged as e4a33790 (#1218) |

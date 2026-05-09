# SLM-OS Interface Contracts

Stable contracts at the boundary between SLM-OS and external producers
or consumers — on-disk file layouts, blob wire formats, and similar
interface definitions that host tools and runtime activation paths
must agree on.

**Not** fact sheets ([`../fact-sheets/`](../fact-sheets/)) and not
design proposals ([`../design/`](../design/)). A contract here is the
canonical source of truth for the format itself: any change to a
contract is a coordinated update across producers (host tools) and
consumers (runtime / kernel parsers), and lands together in one PR.

If a doc describes "what bytes go on the wire / what files live where",
it belongs here. If it describes "how a feature works across
platforms", it belongs in `../fact-sheets/`. If it describes a planned
change, it belongs in `../design/` until the change ships and the
contract here is updated.

## Current contracts

| Doc | Subject |
|---|---|
| [device-file-contract.md](device-file-contract.md) | Standard writable layout under `/mnt/files` for host tooling, runtime blob activation, and boot-managed copies. |
| [runtime-blob-formats.md](runtime-blob-formats.md) | First-cut runtime blob formats used by dynamic eviction and scheduler model loading (`SEMB` outer wrapper, FNV-1a checksum, schema versioning). |

## See also

- [`../fact-sheets/`](../fact-sheets/) — descriptive per-capability cross-platform coverage.
- [`../design/`](../design/) — design specs and engineering proposals.

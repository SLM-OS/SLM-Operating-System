#!/usr/bin/env python3
"""Configuration matrix for the report-only CI build (.github/workflows/build-matrix.yml).

Every PLATFORM gets:
  * its default `make kernel` build,
  * the test kernel (`make kernel-test`, build only, never run),
  * the "wide" configuration that `make check-braces` configures (but never
    compiles) -- its arguments are read from the Makefile's TIDY_WIDE_* variables
    at build time, so this file does not copy them,
  * one cell per build option flipped from that platform's default, one at a
    time (not the cross product).
X86_64 also gets the kexec (multiboot2) and bzImage variants.

Cells that are invalid by construction (the option only applies to another
platform, or is an alias/duplicate of another knob) are listed as skipped with
a reason. Knobs that need files outside the repository are listed as not
buildable in CI.

Usage:
  build_matrix.py github    -> one line `matrix=<json>` for $GITHUB_OUTPUT
  build_matrix.py markdown  -> inventory, cell list and skipped cells (Markdown)
  build_matrix.py json      -> {"cells": [...], "skipped": [...], "not_buildable": [...]}

Defaults were read from Makefile and CMakeLists.txt at origin/main 8ec0e330.
When an option's default or platform gate changes there, change it here too.
"""

import json
import sys

PLATFORMS = ["QEMU_VIRT", "RASPI5", "JETSON_ORIN_NANO", "X86_64"]
ARM = ["QEMU_VIRT", "RASPI5", "JETSON_ORIN_NANO"]
HW_ARM = ["RASPI5", "JETSON_ORIN_NANO"]
SHORT = {"QEMU_VIRT": "qemu", "RASPI5": "pi5", "JETSON_ORIN_NANO": "jetson", "X86_64": "x86"}


def per(**kw):
    """Per-platform default map; keys are SHORT names."""
    inv = {v: k for k, v in SHORT.items()}
    return {inv[k]: v for k, v in kw.items()}


def same(v):
    return {p: v for p in PLATFORMS}


# Each option:
#   name      knob name as the user types it
#   via       "make"  -> passed as NAME=value on the make command line
#             "cmake" -> passed as -DNAME=value through EXTRA_KERNEL_CMAKE_ARGS
#   where     Makefile / CMakeLists.txt line(s) that define it
#   default   {PLATFORM: default value}
#   flip      {PLATFORM: value to build} or None for the boolean opposite
#   applies   platforms where the cell is built; others are skipped with `skip`
#   prereq    extra make args needed so the flip has any effect
#   target    make target for the cell (default "kernel")
OPTIONS = [
    # ---- Makefile knobs (?=) that change what compiles -----------------------
    dict(name="BUILD_TYPE", via="make", where="Makefile:13",
         default=same("Release"), flip=same("Debug"), applies=PLATFORMS),
    dict(name="AI_SCHED", via="make", where="Makefile:23-27 -> ENABLE_AI_SCHEDULER (CMakeLists.txt:2310)",
         default=per(qemu="OFF", pi5="ON", jetson="ON", x86="OFF"), applies=PLATFORMS),
    dict(name="AI_WEIGHTS", via="make", where="Makefile:36 -> CMakeLists.txt:2321",
         default=same("synthetic"), flip=same("real"), applies=PLATFORMS,
         prereq={"QEMU_VIRT": "AI_SCHED=ON", "X86_64": "AI_SCHED=ON"},
         note="only read when AI_SCHED=ON; QEMU/x86 cells add AI_SCHED=ON"),
    dict(name="HAILO_WIRE_DEBUG", via="make", where="Makefile:44, CMakeLists.txt:229",
         default=same("OFF"), applies=ARM,
         skip={"X86_64": "Hailo sources are not compiled on X86_64 (CMakeLists.txt:839-851)"}),
    dict(name="HAILO_TRACE_PHASE_DEFAULT", via="make", where="Makefile:54, CMakeLists.txt:240",
         default=same("0"), flip=same("0x07"), applies=ARM,
         skip={"X86_64": "Hailo sources are not compiled on X86_64"}),
    dict(name="HAILO_TRACE_MECH_DEFAULT", via="make", where="Makefile:55, CMakeLists.txt:243",
         default=same("0"), flip=same("0x03"), applies=ARM,
         skip={"X86_64": "Hailo sources are not compiled on X86_64"}),
    dict(name="WORK_STEALING", via="make", where="Makefile:64 -> ENABLE_WORK_STEALING (CMakeLists.txt:2360-2365, defined twice)",
         default=per(qemu="OFF", pi5="ON", jetson="ON", x86="ON"), applies=PLATFORMS),
    dict(name="JETSON_HW_TICK", via="make", where="Makefile:82-86, CMakeLists.txt:201",
         default=per(qemu="OFF", pi5="OFF", jetson="ON", x86="OFF"), applies=["JETSON_ORIN_NANO"],
         skip={p: "Jetson-only (CMakeLists.txt ignores it elsewhere)" for p in ["QEMU_VIRT", "RASPI5", "X86_64"]},
         note="OFF also turns SECONDARY_PREEMPT's Makefile default OFF (stock-BL31 config)"),
    dict(name="SECONDARY_PREEMPT", via="make", where="Makefile:96-102, CMakeLists.txt:152",
         default=per(qemu="OFF", pi5="ON", jetson="ON", x86="OFF"), applies=HW_ARM,
         skip={p: "only Pi 5 and Jetson define it (CMakeLists.txt ignores it elsewhere)" for p in ["QEMU_VIRT", "X86_64"]}),
    dict(name="SETWAY_INVAL", via="make", where="Makefile:117, CMakeLists.txt:173",
         default=same("ON"), applies=HW_ARM,
         skip={p: "only Pi 5 and Jetson run the set/way loop" for p in ["QEMU_VIRT", "X86_64"]}),
    dict(name="STAGE25_TRACE_PI5", via="make", where="Makefile:301 (no ?=), CMakeLists.txt:309",
         default=same("OFF"), applies=["RASPI5"],
         skip={p: "Pi 5-only (CMakeLists.txt gates on PLATFORM=RASPI5)" for p in ["QEMU_VIRT", "JETSON_ORIN_NANO", "X86_64"]}),
    dict(name="SLOT_TRACE", via="make", where="Makefile:302 (no ?=), CMakeLists.txt:301",
         default=same("OFF"), applies=ARM,
         skip={"X86_64": "ARM64-only: CMakeLists.txt:303 stops with FATAL_ERROR on X86_64"}),
    dict(name="DISABLE_EVICTION", via="make", where="Makefile:136, CMakeLists.txt:2672; cargo feature ai_eviction",
         default=same("OFF"), applies=PLATFORMS),
    dict(name="EVICTION_MODELS", via="make", where="Makefile:137 -> ENABLE_EVICTION_MODELS (CMakeLists.txt:2673); cargo feature ai_eviction_models",
         default=same("OFF"), applies=PLATFORMS),
    dict(name="EVICTION_DEFAULT_POLICY", via="make", where="Makefile:138, CMakeLists.txt:2674; Rust option_env!",
         default=same("lru"), flip=same("xgboost"), applies=PLATFORMS,
         note="string knob read through option_env! at compile time; one non-default value built"),
    dict(name="EMBED_DEMO_SCRIPTS", via="make", where="Makefile:158, CMakeLists.txt:385",
         default=same("ON"), applies=PLATFORMS),
    dict(name="NET_SSHD", via="make", where="Makefile:166, CMakeLists.txt:495",
         default=same("OFF"), applies=ARM,
         skip={"X86_64": "sshd sources are not compiled on X86_64 (CMakeLists.txt:2116); check-braces' wide config leaves it off there too"}),
    dict(name="NET_SSHD_AUTOSTART", via="make", where="Makefile:167, CMakeLists.txt:518",
         default=per(qemu="OFF", pi5="ON", jetson="ON", x86="OFF"), applies=ARM,
         prereq=same("NET_SSHD=ON"), note="needs NET_SSHD=ON to have any effect; cells add it",
         skip={"X86_64": "sshd sources are not compiled on X86_64"}),
    dict(name="NET_SSHD_DEMO_ALLOW_ALL", via="make", where="Makefile:168, CMakeLists.txt:534",
         default=same("OFF"), applies=ARM, prereq=same("NET_SSHD=ON"),
         note="needs NET_SSHD=ON to have any effect; cells add it",
         skip={"X86_64": "sshd sources are not compiled on X86_64"}),
    dict(name="JETSON_EL1_SMOKE", via="make", where="Makefile:313 (no ?=), CMakeLists.txt:416",
         default=same("OFF"), applies=["JETSON_ORIN_NANO"],
         skip={p: "Jetson-only diagnostic (name and KERNEL_BLOB_CMAKE_ARGS)" for p in ["QEMU_VIRT", "RASPI5", "X86_64"]}),

    # ---- CMake option()s without a Makefile variable --------------------------
    dict(name="COOP_PREEMPT", via="cmake", where="CMakeLists.txt:349-358",
         default=per(qemu="ON", pi5="OFF", jetson="OFF", x86="ON"), applies=HW_ARM,
         extra_cmake={"RASPI5": "-DPI5_COOP_PREEMPT=ON", "JETSON_ORIN_NANO": "-DPI5_COOP_PREEMPT=ON"},
         note="the PI5_COOP_PREEMPT alias defaults OFF with it and forces COOP_PREEMPT OFF, so cells pass both (as check-braces' wide config does)",
         skip={p: "only Pi 5 and Jetson define COOP_PREEMPT (CMakeLists.txt:355)" for p in ["QEMU_VIRT", "X86_64"]}),
    dict(name="PI5_FIQ_TIMER", via="cmake", where="CMakeLists.txt:215",
         default=same("OFF"), applies=["RASPI5"],
         skip={p: "Pi 5-only (CMakeLists.txt gates on PLATFORM=RASPI5)" for p in ["QEMU_VIRT", "JETSON_ORIN_NANO", "X86_64"]}),
    dict(name="PI5_IRQ_DIAG", via="cmake", where="CMakeLists.txt:286",
         default=same("ON"), applies=["RASPI5"],
         skip={p: "Pi 5-only (CMakeLists.txt gates on PLATFORM=RASPI5)" for p in ["QEMU_VIRT", "JETSON_ORIN_NANO", "X86_64"]}),
    dict(name="HAILO_D3HOT_AT_BOOT", via="cmake", where="CMakeLists.txt:261",
         default=same("ON"), applies=ARM, skip={"X86_64": "Hailo sources are not compiled on X86_64"}),
    dict(name="HAILO_IRQ_CYCLE_AT_BOOT", via="cmake", where="CMakeLists.txt:276",
         default=same("ON"), applies=ARM, skip={"X86_64": "Hailo sources are not compiled on X86_64"}),
    dict(name="BOOT_TESTS_HW_SET", via="cmake", where="CMakeLists.txt:374",
         default=same("OFF"), applies=PLATFORMS, target="kernel-test",
         note="only meaningful with ENABLE_BOOT_TESTS=ON, so built as the test kernel"),
    dict(name="ENABLE_NETWORKING", via="cmake", where="CMakeLists.txt:402-407 (defined twice)",
         default=same("ON"), applies=PLATFORMS),
    dict(name="JETSON_XHCI_REBOOT_ON_NOOP", via="cmake", where="CMakeLists.txt:425",
         default=same("OFF"), applies=["JETSON_ORIN_NANO"],
         skip={p: "Jetson-only diagnostic (built on every platform by the wide cell)" for p in ["QEMU_VIRT", "RASPI5", "X86_64"]}),
    dict(name="NET_DHCP_AT_BOOT", via="cmake", where="CMakeLists.txt:439",
         default=same("ON"), applies=PLATFORMS),
    dict(name="NET_TELNETD_AUTOSTART", via="cmake", where="CMakeLists.txt:459-467 (cache BOOL on Pi 5/Jetson, option() elsewhere)",
         default=per(qemu="OFF", pi5="ON", jetson="ON", x86="OFF"), applies=PLATFORMS),
    dict(name="NET_TELEMETRYD_AUTOSTART", via="cmake", where="CMakeLists.txt:475",
         default=same("OFF"), applies=PLATFORMS),
    dict(name="SLM_GPU_GOLDEN", via="cmake", where="CMakeLists.txt:1461",
         default=same("OFF"), applies=PLATFORMS),
    dict(name="SLM_REAL_GOLDEN", via="cmake", where="CMakeLists.txt:1477",
         default=same("OFF"), applies=PLATFORMS),
    dict(name="MOCK_CAMERA_FRAME", via="cmake", where="CMakeLists.txt:1568",
         default=same("ON"), applies=PLATFORMS),
    dict(name="ENABLE_GSP_FIRMWARE", via="cmake", where="CMakeLists.txt:2388-2400",
         default=per(qemu="OFF", pi5="OFF", jetson="OFF", x86="ON"), applies=["JETSON_ORIN_NANO", "X86_64"],
         note="ON needs /lib/firmware/nvidia/<chip>/gsp on the build host; CMake turns it off with a warning when absent",
         skip={p: "GSP_CHIP is empty on this platform, so the option has no effect" for p in ["QEMU_VIRT", "RASPI5"]}),
]

# Knobs that are aliases or duplicates of a knob already in the matrix.
ALIASES = [
    ("PI5_SECONDARY_PREEMPT", "CMake option + Makefile var: deprecated alias of SECONDARY_PREEMPT (same build)"),
    ("PI5_COOP_PREEMPT", "CMake option: deprecated alias of COOP_PREEMPT; set together with it in the COOP_PREEMPT cells"),
    ("ENABLE_AI_SCHEDULER", "CMake option behind the AI_SCHED Makefile knob"),
    ("ENABLE_WORK_STEALING", "CMake option behind the WORK_STEALING Makefile knob"),
    ("ENABLE_EVICTION_MODELS", "CMake option behind the EVICTION_MODELS Makefile knob"),
    ("ENABLE_BOOT_TESTS", "CMake option set by `make kernel-test`; built as each platform's test-kernel cell"),
    ("KEXEC_BUILD / BZIMAGE_BUILD", "CMake variables set by `make kernel-kexec` / `make kernel-bzimage` (X86_64 variant cells)"),
    ("AI_EVICTION / AI_EVICTION_MODELS", "deprecated Makefile aliases of DISABLE_EVICTION / EVICTION_MODELS"),
]

# Knobs that need files that are not in the repository, or do not change
# what compiles.
NOT_BUILDABLE = [
    ("HAILO_FW_BLOB", "path to hailo8_fw.bin (vendor firmware, not in repo; auto-detected from /usr/lib/firmware/hailo)"),
    ("SCHEDULER_HEF_BLOB", "path to a compiled scheduler .hef (Hailo DFC output, not in repo)"),
    ("USER_HEF_BLOB", "path to a user .hef (auto-detected from build/hailo/mnist.hef, not in repo)"),
    ("OPLIB_BLOB", "pre-built operator library (needs CUDA/nvcc output, not in repo)"),
    ("MNIST_DIGITS_DIR", "directory of fp32 digit fixtures outside the repo"),
    ("GA10B_FIRMWARE_DIR / GA10B_FW_DIR", "NVIDIA ga10b ucode fetched by scripts/tools/fetch-ga10b-firmware.sh (not redistributable)"),
    ("ENABLE_GSP_FIRMWARE=ON (contents)", "the GSP blobs come from the build host's /lib/firmware; a runner without them builds the auto-disabled path"),
    ("Jetson operator-library SASS", "scripts/cuda/ensure-oplib-shaders.sh needs nvcc (CUDA 12.0); without it Jetson cells build CPU-only GPU paths and print a WARNING"),
    ("SLM_GOLDEN_MODEL", "a ~1 GB GGUF model under ~/models, only used by host golden targets"),
    ("MOCK_CAMERA_FRAME_DIGIT", "alternate digit path; the in-repo default is used"),
    ("KEXEC_MODE, KEXEC_HOST, KEXEC_NO_EXEC", "deploy-only, do not change what compiles"),
    ("BRACES_BASE, BRACES_ARGS, TIDY_VARIANT", "lint-only (check-braces)"),
    ("EXTRA_KERNEL_CMAKE_ARGS", "escape hatch; the matrix uses it to pass the CMake-only options above"),
]


def bool_flip(v):
    return {"ON": "OFF", "OFF": "ON"}[v]


def cells():
    out, skipped = [], []

    def add(platform, target, label, make_args="", extra_cmake="", option="", value="", default="", note=""):
        cid = f"{SHORT[platform]}-{label}"
        out.append(dict(id=cid, platform=platform, target=target, make_args=make_args.strip(),
                        extra_cmake=extra_cmake.strip(), option=option, value=value,
                        default=default, note=note))

    for p in PLATFORMS:
        add(p, "kernel", "default", note="baseline for new-warning comparison")
        add(p, "kernel-test", "test-kernel", note="ENABLE_BOOT_TESTS=ON, build only")
        add(p, "wide", "wide", note="check-braces wide configuration (TIDY_WIDE_* from the Makefile)")
        if p == "X86_64":
            add(p, "kernel-kexec", "kexec", note="multiboot2 kexec variant, linked at 0x20000000")
            add(p, "kernel-bzimage", "bzimage", note="Linux bzImage wrapper variant")
        for o in OPTIONS:
            name = o["name"]
            if p not in o["applies"]:
                skipped.append(dict(id=f"{SHORT[p]}-{name}", platform=p, option=name,
                                    reason=o.get("skip", {}).get(p, "not applicable")))
                continue
            d = o["default"][p]
            v = (o.get("flip") or {}).get(p) or bool_flip(d)
            prereq = (o.get("prereq") or {}).get(p, "")
            extra = (o.get("extra_cmake") or {}).get(p, "")
            if o["via"] == "make":
                margs, cargs = f"{prereq} {name}={v}", extra
            else:
                margs, cargs = prereq, f"-D{name}={v} {extra}"
            add(p, o.get("target", "kernel"), f"{name}={v}", margs, cargs, name, v, d, o.get("note", ""))
    return out, skipped


def markdown():
    cs, sk = cells()
    lines = ["### Platforms", "", ", ".join(f"`{p}`" for p in PLATFORMS), "",
             "### Build options and per-platform defaults", "",
             "| Option | Via | Defined at | " + " | ".join(SHORT[p] for p in PLATFORMS) + " | Notes |",
             "|---|---|---|" + "---|" * len(PLATFORMS) + "---|"]
    for o in OPTIONS:
        row = []
        for p in PLATFORMS:
            d = o["default"][p]
            row.append(d if p in o["applies"] else f"({d})")
        lines.append(f"| `{o['name']}` | {o['via']} | {o['where']} | " + " | ".join(row) + f" | {o.get('note', '')} |")
    lines += ["", "Values in parentheses: the option has no effect on that platform (cell skipped).", "",
              "### Aliases and duplicates (no separate cell)", ""]
    lines += [f"- `{n}`: {r}" for n, r in ALIASES]
    lines += ["", "### Not buildable in CI / not build-affecting", ""]
    lines += [f"- `{n}`: {r}" for n, r in NOT_BUILDABLE]
    lines += ["", f"### Cells ({len(cs)} built, {len(sk)} skipped)", "",
              "| Cell | Target | make args | extra CMake args |", "|---|---|---|---|"]
    for c in cs:
        lines.append(f"| `{c['id']}` | {c['target']} | `{c['make_args']}` | `{c['extra_cmake']}` |")
    lines += ["", "### Skipped cells", "", "| Cell | Reason |", "|---|---|"]
    lines += [f"| `{s['id']}` | {s['reason']} |" for s in sk]
    return "\n".join(lines) + "\n"


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "json"
    cs, sk = cells()
    if mode == "github":
        print("matrix=" + json.dumps({"include": cs}, separators=(",", ":")))
    elif mode == "markdown":
        sys.stdout.write(markdown())
    else:
        print(json.dumps({"cells": cs, "skipped": sk,
                          "not_buildable": [dict(knob=n, reason=r) for n, r in NOT_BUILDABLE]}, indent=1))


if __name__ == "__main__":
    main()

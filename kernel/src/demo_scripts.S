/*
 * demo_scripts.S — Embed Lua demo scripts from scripts/ via .incbin.
 *
 * scripts/demo.lua and scripts/demo_menu.lua are the single source of
 * truth; this file embeds them verbatim into the kernel's read-only
 * data section at link time. demo_init.c uses the _start / _end
 * symbols (and computes length via pointer arithmetic) to write each
 * script into the LittleFS RAM disk at boot.
 *
 * Only compiled and linked when EMBED_DEMO_SCRIPTS is ON in CMake
 * (#14). DEMO_LUA_PATH and DEMO_MENU_LUA_PATH are supplied by
 * CMakeLists.txt via set_source_files_properties COMPILE_DEFINITIONS.
 *
 * Resolves issues #13 (embedded script content outside demo_init.c)
 * and #14 (conditional embedding via build flag).
 */

#if !defined(DEMO_LUA_PATH) || !defined(DEMO_MENU_LUA_PATH) || \
    !defined(DEMO_AUTO_LUA_PATH) || !defined(DEMO_MULTIPROC_LUA_PATH) || \
    !defined(DEMO_HAILO_LUA_PATH) || !defined(DEMO_ADMIN_LUA_PATH) || \
    !defined(DEMO_IMX219_MNIST_LUA_PATH) || \
    !defined(DEMO_CAMERA_HEATMAP_LUA_PATH) || \
    !defined(DEMO_CAMERA_SWEEP_LUA_PATH)
# error "DEMO_LUA_PATH, DEMO_MENU_LUA_PATH, DEMO_AUTO_LUA_PATH, "\
        "DEMO_MULTIPROC_LUA_PATH, DEMO_HAILO_LUA_PATH, "\
        "DEMO_ADMIN_LUA_PATH, DEMO_IMX219_MNIST_LUA_PATH, "\
        "DEMO_CAMERA_HEATMAP_LUA_PATH, and "\
        "DEMO_CAMERA_SWEEP_LUA_PATH must be supplied by the build system"
#endif

#define xstr(s) str(s)
#define str(s) #s

    .section .rodata

    .global demo_lua_start
    .global demo_lua_end
    .align 4
demo_lua_start:
    .incbin xstr(DEMO_LUA_PATH)
demo_lua_end:

    .global demo_menu_lua_start
    .global demo_menu_lua_end
    .align 4
demo_menu_lua_start:
    .incbin xstr(DEMO_MENU_LUA_PATH)
demo_menu_lua_end:

    /* #315: demo_auto.lua previously lived as a hand-transcribed
     * C-string literal in demo_init.c and had already drifted from
     * scripts/demo_auto.lua ("Demo aborted by user." vs "Demo aborted.").
     * Now embedded from the on-disk file like the other demo scripts. */
    .global demo_auto_lua_start
    .global demo_auto_lua_end
    .align 4
demo_auto_lua_start:
    .incbin xstr(DEMO_AUTO_LUA_PATH)
demo_auto_lua_end:

    /* #323: multiproc_demo.lua was orphaned — sitting in scripts/ with
     * no build-side reference and no way to reach /mnt/files at runtime.
     * Now embedded alongside the other demo scripts. */
    .global demo_multiproc_lua_start
    .global demo_multiproc_lua_end
    .align 4
demo_multiproc_lua_start:
    .incbin xstr(DEMO_MULTIPROC_LUA_PATH)
demo_multiproc_lua_end:

    /* Phase 7: Hailo NPU demo. Walks through slm.hailo.status/load/infer
     * and degrades gracefully when the Hailo backend is absent (QEMU,
     * non-Pi 5 platforms). See docs/demo.md §Hailo NPU Demo. */
    .global demo_hailo_lua_start
    .global demo_hailo_lua_end
    .align 4
demo_hailo_lua_start:
    .incbin xstr(DEMO_HAILO_LUA_PATH)
demo_hailo_lua_end:

    /* M6: scripts/admin.lua — the seven-page admin TUI launched by the
     * `admin` shell command. Embedded into the kernel image so it's
     * available the moment the shell starts; demo_init.c writes it to
     * /scripts/admin.lua at boot for `lua /scripts/admin.lua` access. */
    .global demo_admin_lua_start
    .global demo_admin_lua_end
    .align 4
demo_admin_lua_start:
    .incbin xstr(DEMO_ADMIN_LUA_PATH)
demo_admin_lua_end:

    /* #396: IMX219 -> MNIST capstone demo. Pipes a real camera frame
     * through slm.camera.preprocess_mnist + slm.model_infer_bytes
     * from a single Lua entry point. Degrades to a clean "camera not
     * available" message on platforms without IMX219 hardware. */
    .global demo_imx219_mnist_lua_start
    .global demo_imx219_mnist_lua_end
    .align 4
demo_imx219_mnist_lua_start:
    .incbin xstr(DEMO_IMX219_MNIST_LUA_PATH)
demo_imx219_mnist_lua_end:

    /* IMX219 debug aid: renders the post-preprocess 28x28 tensor as
     * an ASCII heatmap so the operator can see what the model
     * actually receives. Optional admin-side inference summary. */
    .global demo_camera_heatmap_lua_start
    .global demo_camera_heatmap_lua_end
    .align 4
demo_camera_heatmap_lua_start:
    .incbin xstr(DEMO_CAMERA_HEATMAP_LUA_PATH)
demo_camera_heatmap_lua_end:

    /* IMX219 gain/exposure sweep — iterate a small grid of
     * (analog, digital, exposure) combos and report pixel range +
     * MNIST argmax + top-1 margin for each. Operator picks the
     * row that looks best, applies it from the REPL. */
    .global demo_camera_sweep_lua_start
    .global demo_camera_sweep_lua_end
    .align 4
demo_camera_sweep_lua_start:
    .incbin xstr(DEMO_CAMERA_SWEEP_LUA_PATH)
demo_camera_sweep_lua_end:

    /* IMX219 flat-field calibration — capture a uniform background as
     * a per-block reference, then sweep the per-mode normalize options
     * on a digit to see which one classifies best. */
    .global demo_camera_flatfield_lua_start
    .global demo_camera_flatfield_lua_end
    .align 4
demo_camera_flatfield_lua_start:
    .incbin xstr(DEMO_CAMERA_FLATFIELD_LUA_PATH)
demo_camera_flatfield_lua_end:

#if defined(__ELF__) && (defined(__x86_64__) || defined(__i386__))
    .section .note.GNU-stack, "", @progbits
#endif

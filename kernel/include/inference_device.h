/*
 * inference_device.h — Pluggable inference-device abstraction for
 * SLM-OS.
 *
 * Purpose
 * -------
 * The kernel runs AI forward-passes for scheduler policies today
 * (NEON MLP / PPO on CPU) and is about to gain a Hailo-8 NPU
 * backend on the Pi 5 AI HAT+. Rather than let every call-site
 * branch on "CPU vs. NPU", the scheduler (and, later, page
 * eviction, Lua bindings, etc.) goes through a single
 * `struct inference_device` vtable. Each backend implements
 * init, load_model, run, free_model, shutdown — the common
 * surface of loading a model and feeding it tensors.
 *
 * Design follows the `gpu_driver` pattern in kernel/gpu/gpu.h
 * and the `gsp_platform_ops` pattern in kernel/gpu/nvidia/gsp.h
 * (one shared core, multiple platform/device implementations).
 *
 * Backends registered today:
 *   - "cpu-mlp" — wraps `ai_schedule_mlp()` on NEON/SSE so the
 *     abstraction is exercised end-to-end before any Hailo code
 *     lands. load_model is a no-op; the MLP weights are compiled
 *     in via ai_weights_mlp.c.
 *
 * Backends added later:
 *   - "hailo-8" (Phase 3/5) — loads a .hef, submits descriptors
 *     over the PCIe VDMA channel.
 *
 * Thread safety
 * -------------
 * The registry is written only at boot (single-threaded). After
 * boot, `inference_device_find` and friends are read-only on
 * the registry; individual device ops must handle their own
 * concurrency (the CPU backend saves/restores FP state and takes
 * no locks; the Hailo backend will serialise on a per-device
 * lock when live).
 */

#ifndef INFERENCE_DEVICE_H
#define INFERENCE_DEVICE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------- */
/* Error codes                                                                */
/* -------------------------------------------------------------------------- */

#define INF_OK                    0
#define INF_ERR_INVAL           (-1)
#define INF_ERR_NODEV           (-2)   /* no backend registered */
#define INF_ERR_NOSUPPORT       (-3)   /* op not implemented by this backend */
#define INF_ERR_NOMEM           (-4)   /* allocation failure */
#define INF_ERR_BAD_MODEL       (-5)   /* model blob malformed / unsupported */
#define INF_ERR_BAD_TENSOR      (-6)   /* tensor shape/dtype mismatch */
#define INF_ERR_TIMEOUT         (-7)   /* device poll exceeded budget */
#define INF_ERR_FULL            (-8)   /* registry or model table full */
#define INF_ERR_BUSY            (-9)   /* slot in use; operation refused */

/* -------------------------------------------------------------------------- */
/* Tensors                                                                     */
/* -------------------------------------------------------------------------- */

/* dtype encoding — matches the common set we'll need for both the
 * FP32 MLP and INT8 Hailo-compiled models. */
enum inference_dtype {
    INF_DTYPE_FP32  = 1,
    INF_DTYPE_FP16  = 2,
    INF_DTYPE_INT8  = 3,
    INF_DTYPE_UINT8 = 4,
    INF_DTYPE_INT32 = 5,
};

#define INF_TENSOR_MAX_RANK 4

/*
 * Lightweight tensor descriptor. Callers own the data buffer; the
 * inference layer never copies (backends may DMA in/out, which is
 * why the buffer needs to live through the run call).
 *
 * `n_elems` is redundant with the shape but precomputed for quick
 * bounds checks by backends. shape[i] = 0 means "dimension not
 * used" for ranks < INF_TENSOR_MAX_RANK.
 */
typedef struct inference_tensor {
    void    *data;
    uint32_t n_elems;
    uint8_t  dtype;        /* enum inference_dtype */
    uint8_t  rank;
    uint16_t shape[INF_TENSOR_MAX_RANK];
} inference_tensor_t;

/* -------------------------------------------------------------------------- */
/* Model handle                                                                */
/* -------------------------------------------------------------------------- */

typedef int32_t inference_model_handle_t;
#define INF_INVALID_HANDLE       ((inference_model_handle_t)-1)

/*
 * Special handle for backends that only know about one hard-coded
 * model (the CPU MLP wrapper uses this — the scheduler weights are
 * compiled in, so "loading" is trivial). Backends that support real
 * model loading (Hailo) hand out sequential positive handles.
 */
#define INF_BUILTIN_HANDLE       ((inference_model_handle_t)0)

/* -------------------------------------------------------------------------- */
/* Device capabilities                                                         */
/* -------------------------------------------------------------------------- */

#define INF_CAP_FP32             (1u << 0)
#define INF_CAP_FP16             (1u << 1)
#define INF_CAP_INT8             (1u << 2)
#define INF_CAP_LOAD_MODEL       (1u << 3)  /* load_model really parses bytes */
#define INF_CAP_DMA              (1u << 4)  /* I/O can be in DMA buffers */

/* -------------------------------------------------------------------------- */
/* Forward decls                                                               */
/* -------------------------------------------------------------------------- */

struct inference_device;

/* -------------------------------------------------------------------------- */
/* Vtable                                                                       */
/* -------------------------------------------------------------------------- */

struct inference_device_ops {
    const char *name;            /* short id: "cpu-mlp", "hailo-8", ... */
    uint32_t    caps;            /* INF_CAP_* bitmask */

    /*
     * One-time init. Called by inference_device_register for each
     * newly registered device. Returns INF_OK to accept.
     */
    int (*init)(struct inference_device *dev);

    /* Optional — symmetric shutdown. May be NULL. */
    void (*shutdown)(struct inference_device *dev);

    /*
     * Load a model from an opaque byte blob. Semantics per backend:
     *   - CPU-MLP: ignores `model` / `size`, returns INF_BUILTIN_HANDLE.
     *   - Hailo: parses `.hef`, DMAs weights, programs the device,
     *     returns a sequential handle.
     * Returns INF_OK and writes *out on success.
     */
    int (*load_model)(struct inference_device *dev,
                      const void *model, size_t size,
                      inference_model_handle_t *out);

    /*
     * Run one forward pass. `in` and `out` shapes/dtypes must match
     * the loaded model's boundaries. Backends validate and return
     * INF_ERR_BAD_TENSOR on mismatch.
     *
     * Called from task context (never from an IRQ handler). On
     * platforms where this runs on the CPU's FP registers, the
     * backend is responsible for saving/restoring FP state.
     */
    int (*run)(struct inference_device *dev,
               inference_model_handle_t h,
               const inference_tensor_t *in,
               inference_tensor_t *out);

    /*
     * Release a model handle. CPU-MLP is a no-op. Hailo frees the
     * device-side descriptor table and weight buffers.
     */
    int (*free_model)(struct inference_device *dev,
                      inference_model_handle_t h);
};

/* -------------------------------------------------------------------------- */
/* Device instance                                                             */
/* -------------------------------------------------------------------------- */

struct inference_device {
    const struct inference_device_ops *ops;
    void    *priv;                 /* backend-private state */
    uint32_t id;                   /* assigned at register time */
    bool     initialised;
};

/* -------------------------------------------------------------------------- */
/* Registry                                                                    */
/* -------------------------------------------------------------------------- */

#define INFERENCE_MAX_DEVICES    4

/*
 * Register a device. Calls dev->ops->init(). On success the device
 * is added to the registry and, if no default is set, becomes the
 * default. Returns INF_OK or a negative error.
 */
int inference_device_register(struct inference_device *dev);

/* Look up by name (case-sensitive). Returns NULL if not present. */
struct inference_device *inference_device_find(const char *name);

/* Indexed access. index in [0, inference_device_count()). */
struct inference_device *inference_device_get(uint32_t index);
uint32_t                 inference_device_count(void);

/*
 * The default device — what policy code uses when it doesn't care
 * which backend runs the model. Set to the first-registered device
 * by default; can be overridden at runtime with
 * inference_device_set_default(). Returns NULL if nothing registered.
 */
struct inference_device *inference_device_default(void);
int                      inference_device_set_default(struct inference_device *dev);

/* Convenience wrappers around ops. Always check `dev != NULL` first
 * via inference_device_default() — calling with NULL returns
 * INF_ERR_NODEV. */
int inference_load_model(struct inference_device *dev,
                         const void *model, size_t size,
                         inference_model_handle_t *out);
int inference_run(struct inference_device *dev,
                  inference_model_handle_t h,
                  const inference_tensor_t *in,
                  inference_tensor_t *out);
int inference_free_model(struct inference_device *dev,
                         inference_model_handle_t h);

#endif /* INFERENCE_DEVICE_H */

/*
 * inference_device.c — Dispatcher / registry for inference_device.h.
 *
 * Pure glue code: holds the registry, forwards calls through the
 * vtable. No hardware access; no FP operations. Backends live in
 * sibling files (inference_cpu.c today, inference_hailo.c later).
 */

#include "inference_device.h"
#include "debug.h"
#include "smp.h"
#include "string.h"
#include <stddef.h>

/* -------------------------------------------------------------------------- */
/* Registry                                                                    */
/* -------------------------------------------------------------------------- */

static struct inference_device *devices[INFERENCE_MAX_DEVICES];
static uint32_t                 device_count;
static struct inference_device *default_device;

int inference_device_register(struct inference_device *dev)
{
    /* INVARIANT: registration is single-CPU-only. Every call to this
     * function runs from boot-time module init on CPU 0 before
     * secondaries come online. There is no lock here because adding
     * one would only matter if a second CPU ever called register()
     * concurrently, which would itself be a bug — `devices[]` and
     * `device_count` would already be inconsistent past that point.
     * Trip an assert so the violation is loud rather than corrupting
     * state silently. */
    ASSERT(cpu_id() == 0);

    if (!dev || !dev->ops || !dev->ops->name
     || !dev->ops->load_model || !dev->ops->run || !dev->ops->free_model) {
        return INF_ERR_INVAL;
    }
    if (device_count >= INFERENCE_MAX_DEVICES) {
        return INF_ERR_FULL;
    }

    dev->id = device_count;

    if (dev->ops->init) {
        int rc = dev->ops->init(dev);
        if (rc != INF_OK) {
            WARN("inference: %s init failed (%d)", dev->ops->name, rc);
            return rc;
        }
    }
    dev->initialised = true;

    devices[device_count++] = dev;
    if (!default_device) {
        default_device = dev;
    }

    INFO("inference: registered '%s' (caps=0x%x, id=%u%s)",
         dev->ops->name, dev->ops->caps, dev->id,
         (default_device == dev) ? ", default" : "");
    return INF_OK;
}

struct inference_device *inference_device_find(const char *name)
{
    if (!name) return NULL;
    for (uint32_t i = 0; i < device_count; i++) {
        if (strcmp(name, devices[i]->ops->name) == 0) {
            return devices[i];
        }
    }
    return NULL;
}

struct inference_device *inference_device_get(uint32_t index)
{
    return (index < device_count) ? devices[index] : NULL;
}

uint32_t inference_device_count(void)
{
    return device_count;
}

struct inference_device *inference_device_default(void)
{
    return default_device;
}

int inference_device_set_default(struct inference_device *dev)
{
    if (!dev) return INF_ERR_INVAL;
    for (uint32_t i = 0; i < device_count; i++) {
        if (devices[i] == dev) {
            default_device = dev;
            return INF_OK;
        }
    }
    return INF_ERR_NODEV;
}

/* -------------------------------------------------------------------------- */
/* Vtable forwarders                                                           */
/* -------------------------------------------------------------------------- */

int inference_load_model(struct inference_device *dev,
                         const void *model, size_t size,
                         inference_model_handle_t *out)
{
    if (!dev || !out) return INF_ERR_INVAL;
    if (!dev->initialised) return INF_ERR_NODEV;
    return dev->ops->load_model(dev, model, size, out);
}

int inference_run(struct inference_device *dev, inference_model_handle_t h,
                  const inference_tensor_t *in, inference_tensor_t *out)
{
    if (!dev || !in || !out) return INF_ERR_INVAL;
    if (!dev->initialised) return INF_ERR_NODEV;
    return dev->ops->run(dev, h, in, out);
}

int inference_free_model(struct inference_device *dev,
                         inference_model_handle_t h)
{
    if (!dev) return INF_ERR_INVAL;
    if (!dev->initialised) return INF_ERR_NODEV;
    return dev->ops->free_model(dev, h);
}

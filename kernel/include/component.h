/**
 * @file component.h
 * @brief Component System Interface
 *
 * Provides the C API for the Rust component system.
 * Components are self-contained units of functionality that can be:
 * - Loaded and unloaded at runtime
 * - Hot-swapped without system restart
 * - Interconnected via message passing
 *
 * See docs/components.md for full documentation.
 */

#ifndef COMPONENT_H
#define COMPONENT_H

#include <stdint.h>

/**
 * Maximum component name length (including null terminator)
 */
#define COMPONENT_MAX_NAME 32

/**
 * Maximum component version length
 */
#define COMPONENT_MAX_VERSION 16

/**
 * Maximum component description length
 */
#define COMPONENT_MAX_DESC 64

/**
 * Maximum number of components that can be loaded simultaneously
 */
#define COMPONENT_MAX_COUNT 16

/**
 * Component lifecycle states.
 */
typedef enum {
    COMPONENT_LOADED      = 0,  /**< Binary loaded into memory */
    COMPONENT_INITIALIZING = 1, /**< Init function running */
    COMPONENT_RUNNING     = 2,  /**< Active and processing */
    COMPONENT_SUSPENDED   = 3,  /**< Temporarily paused */
    COMPONENT_UPDATING    = 4,  /**< Being replaced by new version */
    COMPONENT_TERMINATING = 5,  /**< Shutdown in progress */
    COMPONENT_UNLOADED    = 6,  /**< Resources freed */
} component_state_t;

/**
 * Component type classification.
 */
typedef enum {
    COMPONENT_TYPE_SERVICE     = 0, /**< Background service */
    COMPONENT_TYPE_DRIVER      = 1, /**< Hardware interface */
    COMPONENT_TYPE_APPLICATION = 2, /**< User-facing application */
} component_type_t;

/**
 * Component priority levels.
 */
typedef enum {
    COMPONENT_PRIORITY_IDLE     = 0,
    COMPONENT_PRIORITY_LOW      = 2,
    COMPONENT_PRIORITY_NORMAL   = 4,
    COMPONENT_PRIORITY_HIGH     = 6,
    COMPONENT_PRIORITY_CRITICAL = 7,
} component_priority_t;

/**
 * Component metadata and runtime state.
 *
 * This structure matches the Rust ComponentInfo struct exactly
 * for FFI compatibility.
 */
typedef struct {
    uint8_t name[COMPONENT_MAX_NAME];        /**< Null-terminated name */
    uint8_t version[COMPONENT_MAX_VERSION];  /**< Null-terminated version */
    uint8_t description[COMPONENT_MAX_DESC]; /**< Null-terminated description */
    uint8_t component_type;                  /**< component_type_t */
    uint8_t priority;                        /**< component_priority_t */
    uint8_t state;                           /**< component_state_t */
    uint8_t _pad;                            /**< Alignment padding */
    uint32_t task_id;                        /**< Associated task ID (0 = none) */
    uint32_t memory_kb;                      /**< Memory usage in KB */
    uint64_t switches;                       /**< Context switch count */
} component_info_t;

// =============================================================================
// System Lifecycle
// =============================================================================

/**
 * Initialize the component system.
 *
 * Must be called once during kernel boot before using component functions.
 *
 * @return 0 on success, -1 on error
 */
int component_system_init(void);

// =============================================================================
// Component Registration
// =============================================================================

/**
 * Register a new component.
 *
 * @param name Component name (null-terminated)
 * @param version Version string (null-terminated)
 * @param type Component type (0=service, 1=driver, 2=application)
 * @param priority Priority level (0-7)
 * @return Component index on success, -1 on error
 */
int component_register(const char *name, const char *version,
                       uint8_t type, uint8_t priority);

/**
 * Unregister a component by index.
 *
 * @param index Component index from component_register()
 * @return 0 on success, -1 on error
 */
int component_unregister(uint32_t index);

// =============================================================================
// Component Query
// =============================================================================

/**
 * Get the number of registered components.
 *
 * @return Number of active components
 */
uint32_t component_count(void);

/**
 * Get component info by index.
 *
 * @param index Component index (0 to component_count()-1)
 * @param info Output parameter for component info
 * @return 0 on success, -1 on error
 */
int component_get_info(uint32_t index, component_info_t *info);

/**
 * Find a component by name.
 *
 * @param name Component name to search for
 * @return Component index if found, -1 if not found
 */
int component_find(const char *name);

// =============================================================================
// Runtime Operations
// =============================================================================

/**
 * Run a built-in component by name.
 *
 * Registers it in the component system, creates a task, and links them.
 *
 * @param name Built-in component name (e.g., "counter", "echo", "listener")
 * @return Component index on success, -1 on error
 */
int component_run(const char *name);

/**
 * Hot-swap a running component with a new version.
 *
 * Saves the old component's topic subscriptions, unregisters the old
 * component, runs the new one, and re-subscribes to the saved topics.
 *
 * @param old_name Name of the component to replace
 * @param new_name Name of the replacement component (built-in)
 * @return New component index on success, -1 on error
 */
int component_hot_swap(const char *old_name, const char *new_name);

// =============================================================================
// Stateful Hot-Swap
// =============================================================================

/**
 * Maximum state transfer buffer size for stateful hot-swap.
 */
#define COMPONENT_STATE_MAX 256

/**
 * State transfer buffer for stateful hot-swap.
 *
 * During component_hot_swap_stateful(), the old component's state_export
 * callback fills this buffer. The new component reads it via
 * component_get_swap_state() during initialization.
 */
typedef struct {
    uint8_t data[COMPONENT_STATE_MAX];  /**< Serialized component state */
    uint32_t size;                       /**< Bytes written (0 = no state) */
    int valid;                           /**< 1 if state was exported */
} component_swap_state_t;

/**
 * State export callback type.
 * Called on the old component before teardown.
 * Should write serialized state into buf (up to COMPONENT_STATE_MAX bytes).
 * Returns number of bytes written, or -1 on error.
 */
typedef int (*component_state_export_fn)(uint8_t *buf, uint32_t max_size);

/**
 * Hot-swap with state transfer.
 *
 * Calls the export callback on the old component, saves state + subscriptions,
 * tears down old, starts new, restores subscriptions. New component retrieves
 * state via component_get_swap_state().
 *
 * @param old_name Name of the component to replace
 * @param new_name Name of the replacement component
 * @param export_fn Callback to export old component's state (may be NULL)
 * @return New component index on success, -1 on error
 */
int component_hot_swap_stateful(const char *old_name, const char *new_name,
                                component_state_export_fn export_fn);

/**
 * Get the state buffer from the most recent stateful hot-swap.
 * Called by the new component during initialization to import state.
 *
 * @param buf Output buffer to copy state into
 * @param max_size Size of output buffer
 * @return Number of bytes copied, or 0 if no state available
 */
uint32_t component_get_swap_state(uint8_t *buf, uint32_t max_size);

// =============================================================================
// State Management
// =============================================================================

/**
 * Set component state.
 *
 * Valid state transitions:
 *   Loaded -> Initializing -> Running
 *   Running <-> Suspended
 *   Running -> Updating -> Running
 *   Any -> Terminating -> Unloaded
 *
 * @param index Component index
 * @param state New state (component_state_t value)
 * @return 0 on success, -1 on error
 */
int component_set_state(uint32_t index, uint8_t state);

/**
 * Get state name as string.
 *
 * @param state Component state value
 * @return Human-readable state name
 */
const char *component_state_name(component_state_t state);

/**
 * Get component type name as string.
 *
 * @param type Component type value
 * @return Human-readable type name
 */
const char *component_type_name(component_type_t type);

#endif /* COMPONENT_H */

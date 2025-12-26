/**
 * @file component.c
 * @brief Component System C Helpers
 *
 * Provides C helper functions for the component system.
 * The core functionality is implemented in Rust (runtime/src/component/).
 */

#include "component.h"

/**
 * Get state name as string.
 */
const char *component_state_name(component_state_t state)
{
    switch (state) {
    case COMPONENT_LOADED:      return "loaded";
    case COMPONENT_INITIALIZING: return "initializing";
    case COMPONENT_RUNNING:     return "running";
    case COMPONENT_SUSPENDED:   return "suspended";
    case COMPONENT_UPDATING:    return "updating";
    case COMPONENT_TERMINATING: return "terminating";
    case COMPONENT_UNLOADED:    return "unloaded";
    default:                    return "unknown";
    }
}

/**
 * Get component type name as string.
 */
const char *component_type_name(component_type_t type)
{
    switch (type) {
    case COMPONENT_TYPE_SERVICE:     return "service";
    case COMPONENT_TYPE_DRIVER:      return "driver";
    case COMPONENT_TYPE_APPLICATION: return "application";
    default:                         return "unknown";
    }
}

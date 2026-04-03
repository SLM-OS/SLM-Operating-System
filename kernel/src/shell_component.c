/*
 * shell_component.c - Component system commands for SLM-OS shell
 *
 * Commands: component (list, register, unregister, status)
 */

#include "shell_internal.h"
#include "uart.h"
#include "component.h"
#include "string.h"
#include <stdint.h>

/*
 * cmd_component - Component system management.
 */
int cmd_component(int argc, char *argv[])
{
    if (argc < 2) {
        /* Show help */
        uart_puts("Component System Commands:\r\n");
        uart_puts("  component list      - List all registered components\r\n");
        uart_puts("  component register <name> <version> <type> [priority]\r\n");
        uart_puts("                      - Register a component\r\n");
        uart_puts("                        type: service|driver|application\r\n");
        uart_puts("                        priority: idle|low|normal|high|critical\r\n");
        uart_puts("  component unregister <idx> - Unregister component by index\r\n");
        uart_puts("  component status <name|idx> - Show component details\r\n");
        return 0;
    }

    const char *subcmd = argv[1];

    /* component list */
    if (strcmp(subcmd, "list") == 0) {
        uint32_t count = component_count();
        uart_printf("Registered Components: %u\r\n", count);

        if (count == 0) {
            uart_puts("  (none)\r\n");
            return 0;
        }

        uart_puts("  Idx  Name                 Version   Type        State       Pri\r\n");
        uart_puts("  ---  ----                 -------   ----        -----       ---\r\n");

        for (uint32_t i = 0; i < COMPONENT_MAX_COUNT; i++) {
            component_info_t info;
            if (component_get_info(i, &info) == 0) {
                uart_printf("  %3u  %-20s %-9s %-11s %-11s %s\r\n",
                    i,
                    (const char *)info.name,
                    (const char *)info.version,
                    component_type_name(info.component_type),
                    component_state_name(info.state),
                    info.priority == COMPONENT_PRIORITY_CRITICAL ? "crit" :
                    info.priority == COMPONENT_PRIORITY_HIGH ? "high" :
                    info.priority == COMPONENT_PRIORITY_LOW ? "low" :
                    info.priority == COMPONENT_PRIORITY_IDLE ? "idle" : "norm");
            }
        }
        return 0;
    }

    /* component register <name> <version> <type> [priority] */
    if (strcmp(subcmd, "register") == 0) {
        if (argc < 5) {
            uart_puts("Usage: component register <name> <version> <type> [priority]\r\n");
            return -1;
        }

        const char *name = argv[2];
        const char *version = argv[3];
        const char *type_str = argv[4];
        const char *prio_str = (argc > 5) ? argv[5] : "normal";

        /* Parse type */
        uint8_t type;
        if (strcmp(type_str, "service") == 0) {
            type = COMPONENT_TYPE_SERVICE;
        } else if (strcmp(type_str, "driver") == 0) {
            type = COMPONENT_TYPE_DRIVER;
        } else if (strcmp(type_str, "application") == 0) {
            type = COMPONENT_TYPE_APPLICATION;
        } else {
            uart_printf("Unknown type: %s\r\n", type_str);
            return -1;
        }

        /* Parse priority */
        uint8_t priority;
        if (strcmp(prio_str, "idle") == 0) {
            priority = COMPONENT_PRIORITY_IDLE;
        } else if (strcmp(prio_str, "low") == 0) {
            priority = COMPONENT_PRIORITY_LOW;
        } else if (strcmp(prio_str, "high") == 0) {
            priority = COMPONENT_PRIORITY_HIGH;
        } else if (strcmp(prio_str, "critical") == 0) {
            priority = COMPONENT_PRIORITY_CRITICAL;
        } else {
            priority = COMPONENT_PRIORITY_NORMAL;
        }

        int idx = component_register(name, version, type, priority);
        if (idx < 0) {
            uart_puts("Failed to register component\r\n");
            return -1;
        }

        uart_printf("Registered component '%s' at index %d\r\n", name, idx);
        return 0;
    }

    /* component unregister <idx> */
    if (strcmp(subcmd, "unregister") == 0) {
        if (argc < 3) {
            uart_puts("Usage: component unregister <idx>\r\n");
            return -1;
        }

        uint32_t idx;
        if (shell_parse_uint(argv[2], &idx) != 0) {
            uart_puts("Invalid index\r\n");
            return -1;
        }
        if (component_unregister(idx) != 0) {
            uart_printf("Failed to unregister component %u\r\n", idx);
            return -1;
        }

        uart_printf("Unregistered component %u\r\n", idx);
        return 0;
    }

    /* component status <name|idx> */
    if (strcmp(subcmd, "status") == 0) {
        if (argc < 3) {
            uart_puts("Usage: component status <name|idx>\r\n");
            return -1;
        }

        const char *arg = argv[2];
        int idx;

        /* Check if numeric */
        if (arg[0] >= '0' && arg[0] <= '9') {
            uint32_t parsed;
            if (shell_parse_uint(arg, &parsed) != 0) {
                uart_puts("Invalid index\r\n");
                return -1;
            }
            idx = (int)parsed;
        } else {
            idx = component_find(arg);
            if (idx < 0) {
                uart_printf("Component '%s' not found\r\n", arg);
                return -1;
            }
        }

        component_info_t info;
        if (component_get_info((uint32_t)idx, &info) != 0) {
            uart_printf("Failed to get info for component %d\r\n", idx);
            return -1;
        }

        uart_printf("Component %d:\r\n", idx);
        uart_printf("  Name:        %s\r\n", (const char *)info.name);
        uart_printf("  Version:     %s\r\n", (const char *)info.version);
        uart_printf("  Type:        %s\r\n", component_type_name(info.component_type));
        uart_printf("  State:       %s\r\n", component_state_name(info.state));
        uart_printf("  Priority:    %u\r\n", info.priority);
        uart_printf("  Task ID:     %u\r\n", info.task_id);
        uart_printf("  Memory:      %u KB\r\n", info.memory_kb);
        uart_printf("  Switches:    %llu\r\n", (unsigned long long)info.switches);
        return 0;
    }

    uart_printf("Unknown subcommand: %s\r\n", subcmd);
    uart_puts("Use 'component' for help.\r\n");
    return -1;
}

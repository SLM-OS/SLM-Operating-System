/*
 * shell_component.c - Component system and message router commands
 *
 * Commands: component (list, register, unregister, status)
 *           msg (send, list, subscribe)
 */

#include "shell.h"
#include "shell_internal.h"
#include "uart.h"
#include "component.h"
#include "string.h"
#include "slm_ffi.h"
#include <stdint.h>

/* Component runtime (component_runtime.c) */
extern int component_run(const char *name);
extern int component_send_echo(const char *message);
extern int component_hot_swap(const char *old_name, const char *new_name);
extern void component_list_builtins(void);

/* Message router (msg_router.c) */
extern void msg_router_init(void);
extern int msg_router_subscribe(const char *topic_name, int component_idx);
/* msg_router_publish declared in slm_ffi.h */
extern const char *msg_router_receive(int component_idx, char *topic_out);
extern void msg_router_ack(int component_idx);
extern void msg_router_list(void);

int cmd_component(int argc, char *argv[])
{
    if (argc < 2) {
        /* Show help */
        shell_puts("Component System Commands:\r\n");
        shell_puts("  component list            - List registered components\r\n");
        shell_puts("  component builtins        - List available built-in components\r\n");
        shell_puts("  component run <name>      - Run a built-in component\r\n");
        shell_puts("  component swap <old> <new> - Hot-swap: replace old with new\r\n");
        shell_puts("  component send <msg>      - Send message to echo service\r\n");
        shell_puts("  component register <name> <version> <type> [priority]\r\n");
        shell_puts("  component unregister <idx>\r\n");
        shell_puts("  component status <name|idx>\r\n");
        return 0;
    }

    const char *subcmd = argv[1];

    /* component list */
    if (strcmp(subcmd, "list") == 0) {
        uint32_t count = component_count();
        shell_printf("Registered Components: %u\r\n", count);

        if (count == 0) {
            shell_puts("  (none)\r\n");
            return 0;
        }

        shell_puts("  Idx  Name                 Version   Type        State       Pri\r\n");
        shell_puts("  ---  ----                 -------   ----        -----       ---\r\n");

        for (uint32_t i = 0; i < COMPONENT_MAX_COUNT; i++) {
            component_info_t info;
            if (component_get_info(i, &info) == 0) {
                shell_printf("  %3u  %-20s %-9s %-11s %-11s %s\r\n",
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

    /* component builtins */
    if (strcmp(subcmd, "builtins") == 0) {
        component_list_builtins();
        return 0;
    }

    /* component run <name> */
    if (strcmp(subcmd, "run") == 0) {
        if (argc < 3) {
            shell_puts("Usage: component run <name>\r\n");
            component_list_builtins();
            return -1;
        }
        int idx = component_run(argv[2]);
        return (idx >= 0) ? 0 : -1;
    }

    /* component swap <old_name> <new_name> */
    if (strcmp(subcmd, "swap") == 0) {
        if (argc < 4) {
            shell_puts("Usage: component swap <old_name> <new_name>\r\n");
            return -1;
        }
        int idx = component_hot_swap(argv[2], argv[3]);
        return (idx >= 0) ? 0 : -1;
    }

    /* component send <message> */
    if (strcmp(subcmd, "send") == 0) {
        if (argc < 3) {
            shell_puts("Usage: component send <message>\r\n");
            return -1;
        }
        /* Join remaining args into a single message */
        char msg[64];
        int pos = 0;
        for (int i = 2; i < argc && pos < 62; i++) {
            if (i > 2 && pos < 62) msg[pos++] = ' ';
            for (int j = 0; argv[i][j] && pos < 62; j++) {
                msg[pos++] = argv[i][j];
            }
        }
        msg[pos] = '\0';
        return component_send_echo(msg);
    }

    /* component register <name> <version> <type> [priority] */
    if (strcmp(subcmd, "register") == 0) {
        if (argc < 5) {
            shell_puts("Usage: component register <name> <version> <type> [priority]\r\n");
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
            shell_printf("Unknown type: %s\r\n", type_str);
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
            shell_puts("Failed to register component\r\n");
            return -1;
        }

        shell_printf("Registered component '%s' at index %d\r\n", name, idx);
        return 0;
    }

    /* component unregister <idx> */
    if (strcmp(subcmd, "unregister") == 0) {
        if (argc < 3) {
            shell_puts("Usage: component unregister <idx>\r\n");
            return -1;
        }

        uint32_t idx;
        if (shell_parse_uint(argv[2], &idx) != 0) {
            shell_puts("Invalid index\r\n");
            return -1;
        }
        if (component_unregister(idx) != 0) {
            shell_printf("Failed to unregister component %u\r\n", idx);
            return -1;
        }

        shell_printf("Unregistered component %u\r\n", idx);
        return 0;
    }

    /* component status <name|idx> */
    if (strcmp(subcmd, "status") == 0) {
        if (argc < 3) {
            shell_puts("Usage: component status <name|idx>\r\n");
            return -1;
        }

        const char *arg = argv[2];
        int idx;

        /* Check if numeric */
        if (arg[0] >= '0' && arg[0] <= '9') {
            uint32_t parsed;
            if (shell_parse_uint(arg, &parsed) != 0) {
                shell_puts("Invalid index\r\n");
                return -1;
            }
            idx = (int)parsed;
        } else {
            idx = component_find(arg);
            if (idx < 0) {
                shell_printf("Component '%s' not found\r\n", arg);
                return -1;
            }
        }

        component_info_t info;
        if (component_get_info((uint32_t)idx, &info) != 0) {
            shell_printf("Failed to get info for component %d\r\n", idx);
            return -1;
        }

        shell_printf("Component %d:\r\n", idx);
        shell_printf("  Name:        %s\r\n", (const char *)info.name);
        shell_printf("  Version:     %s\r\n", (const char *)info.version);
        shell_printf("  Type:        %s\r\n", component_type_name(info.component_type));
        shell_printf("  State:       %s\r\n", component_state_name(info.state));
        shell_printf("  Priority:    %u\r\n", info.priority);
        shell_printf("  Task ID:     %u\r\n", info.task_id);
        shell_printf("  Memory:      %u KB\r\n", info.memory_kb);
        shell_printf("  Switches:    %llu\r\n", (unsigned long long)info.switches);
        return 0;
    }

    shell_printf("Unknown subcommand: %s\r\n", subcmd);
    shell_puts("Use 'component' for help.\r\n");
    return -1;
}

/*
 * cmd_msg - Message router commands.
 *
 * Subcommands:
 *   msg send <topic> <data>     - Publish a message to a topic
 *   msg list                    - List topics and subscribers
 *   msg subscribe <topic> <idx> - Subscribe a component to a topic
 */
int cmd_msg(int argc, char *argv[])
{
    if (argc < 2) {
        shell_puts("Message Router Commands:\r\n");
        shell_puts("  msg send <topic> <data>     - Publish message to topic\r\n");
        shell_puts("  msg list                    - List topics and subscribers\r\n");
        shell_puts("  msg subscribe <topic> <idx> - Subscribe component to topic\r\n");
        return 0;
    }

    const char *subcmd = argv[1];

    /* msg list */
    if (strcmp(subcmd, "list") == 0) {
        msg_router_list();
        return 0;
    }

    /* msg subscribe <topic> <component_idx> */
    if (strcmp(subcmd, "subscribe") == 0) {
        if (argc < 4) {
            shell_puts("Usage: msg subscribe <topic> <component_idx>\r\n");
            return -1;
        }
        uint32_t idx;
        if (shell_parse_uint(argv[3], &idx) != 0) {
            shell_puts("Invalid component index\r\n");
            return -1;
        }
        int ret = msg_router_subscribe(argv[2], (int)idx);
        if (ret == 0) {
            shell_printf("Subscribed component %u to topic '%s'\r\n", idx, argv[2]);
        }
        return ret;
    }

    /* msg send <topic> <data...> */
    if (strcmp(subcmd, "send") == 0) {
        if (argc < 4) {
            shell_puts("Usage: msg send <topic> <data...>\r\n");
            return -1;
        }
        const char *topic = argv[2];

        /* Join remaining args into message */
        char msg[64];
        int pos = 0;
        for (int i = 3; i < argc && pos < 62; i++) {
            if (i > 3 && pos < 62) msg[pos++] = ' ';
            for (int j = 0; argv[i][j] && pos < 62; j++) {
                msg[pos++] = argv[i][j];
            }
        }
        msg[pos] = '\0';

        int delivered = msg_router_publish((const uint8_t *)topic, (const uint8_t *)msg);
        shell_printf("Message delivered to %d subscriber(s)\r\n", delivered);
        return (delivered > 0) ? 0 : -1;
    }

    shell_printf("Unknown subcommand: %s\r\n", subcmd);
    shell_puts("Use 'msg' for help.\r\n");
    return -1;
}

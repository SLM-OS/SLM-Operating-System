/*
 * msg_router.c - Topic-based message routing for component IPC
 *
 * Provides publish/subscribe messaging between components:
 *   - Components subscribe to topics (string-based)
 *   - msg_route_publish(topic, data) sends to all subscribers
 *   - Each subscriber has a shared mailbox for message delivery
 *
 * Uses the same yield-based polling as the echo service mailbox,
 * with atomic operations for cross-task visibility.
 */

#include "component.h"
#include "uart.h"
#include "sched.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ============================================================================
 * Message Router Data Structures
 * ============================================================================ */

#define MSG_ROUTER_MAX_TOPICS       8
#define MSG_ROUTER_MAX_SUBSCRIBERS  4    /* per topic */
#define MSG_ROUTER_MAX_MSG_LEN     60
#define MSG_ROUTER_TOPIC_LEN       16

/* Per-subscriber mailbox for message delivery */
struct msg_mailbox {
    volatile uint32_t ready;     /* 1 = message available */
    volatile uint32_t ack;       /* 1 = message consumed */
    char data[MSG_ROUTER_MAX_MSG_LEN];
    char topic[MSG_ROUTER_TOPIC_LEN];
};

/* A topic subscription */
struct msg_subscription {
    int component_idx;                   /* -1 = unused slot */
    struct msg_mailbox mailbox;
};

/* A topic with its subscribers */
struct msg_topic {
    char name[MSG_ROUTER_TOPIC_LEN];     /* Empty string = unused */
    struct msg_subscription subs[MSG_ROUTER_MAX_SUBSCRIBERS];
    int sub_count;
};

static struct msg_topic topics[MSG_ROUTER_MAX_TOPICS];
static int topic_count;

/* ============================================================================
 * String helpers (no libc)
 * ============================================================================ */

static int str_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

static void str_copy(char *dst, const char *src, int max)
{
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* ============================================================================
 * Public API
 * ============================================================================ */

/*
 * Initialize the message router.
 */
void msg_router_init(void)
{
    topic_count = 0;
    for (int i = 0; i < MSG_ROUTER_MAX_TOPICS; i++) {
        topics[i].name[0] = '\0';
        topics[i].sub_count = 0;
        for (int j = 0; j < MSG_ROUTER_MAX_SUBSCRIBERS; j++) {
            topics[i].subs[j].component_idx = -1;
            topics[i].subs[j].mailbox.ready = 0;
            topics[i].subs[j].mailbox.ack = 0;
        }
    }
}

/*
 * Subscribe a component to a topic.
 * Creates the topic if it doesn't exist.
 * Returns 0 on success, -1 on error.
 */
int msg_router_subscribe(const char *topic_name, int component_idx)
{
    /* Find or create topic */
    struct msg_topic *topic = NULL;
    for (int i = 0; i < MSG_ROUTER_MAX_TOPICS; i++) {
        if (topics[i].name[0] && str_eq(topics[i].name, topic_name)) {
            topic = &topics[i];
            break;
        }
    }

    if (!topic) {
        /* Create new topic */
        for (int i = 0; i < MSG_ROUTER_MAX_TOPICS; i++) {
            if (!topics[i].name[0]) {
                str_copy(topics[i].name, topic_name, MSG_ROUTER_TOPIC_LEN);
                topics[i].sub_count = 0;
                topic = &topics[i];
                topic_count++;
                break;
            }
        }
    }

    if (!topic) {
        uart_printf("[msg] No free topic slots\n");
        return -1;
    }

    /* Add subscriber */
    for (int j = 0; j < MSG_ROUTER_MAX_SUBSCRIBERS; j++) {
        if (topic->subs[j].component_idx == -1) {
            topic->subs[j].component_idx = component_idx;
            topic->subs[j].mailbox.ready = 0;
            topic->subs[j].mailbox.ack = 0;
            topic->sub_count++;
            return 0;
        }
    }

    uart_printf("[msg] Topic '%s' full (%d subscribers)\n",
                topic_name, MSG_ROUTER_MAX_SUBSCRIBERS);
    return -1;
}

/*
 * Publish a message to a topic.
 * Delivers to all subscribers via their mailboxes.
 * Waits for each subscriber to acknowledge (yield-based).
 * Returns number of subscribers that received the message.
 */
int msg_router_publish(const char *topic_name, const char *data)
{
    /* Find topic */
    struct msg_topic *topic = NULL;
    for (int i = 0; i < MSG_ROUTER_MAX_TOPICS; i++) {
        if (topics[i].name[0] && str_eq(topics[i].name, topic_name)) {
            topic = &topics[i];
            break;
        }
    }

    if (!topic) {
        uart_printf("[msg] Topic '%s' not found\n", topic_name);
        return 0;
    }

    int delivered = 0;
    extern volatile uint64_t pit_ticks;

    for (int j = 0; j < MSG_ROUTER_MAX_SUBSCRIBERS; j++) {
        if (topic->subs[j].component_idx == -1) continue;

        struct msg_mailbox *mb = &topic->subs[j].mailbox;

        /* Copy message to subscriber's mailbox */
        str_copy((char *)mb->data, data, MSG_ROUTER_MAX_MSG_LEN);
        str_copy((char *)mb->topic, topic_name, MSG_ROUTER_TOPIC_LEN);
        __atomic_store_n(&mb->ack, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&mb->ready, 1, __ATOMIC_RELEASE);

        /* Wait for ack (5 second timeout) */
        uint64_t timeout = pit_ticks + 500;
        while (pit_ticks < timeout) {
            if (__atomic_load_n(&mb->ack, __ATOMIC_ACQUIRE)) {
                delivered++;
                break;
            }
            yield();
        }
    }

    return delivered;
}

/*
 * Check if a subscriber has a pending message.
 * Returns pointer to the mailbox data, or NULL if no message.
 * Caller must call msg_router_ack() after processing.
 */
const char *msg_router_receive(int component_idx, char *topic_out)
{
    for (int i = 0; i < MSG_ROUTER_MAX_TOPICS; i++) {
        if (!topics[i].name[0]) continue;
        for (int j = 0; j < MSG_ROUTER_MAX_SUBSCRIBERS; j++) {
            if (topics[i].subs[j].component_idx != component_idx) continue;
            struct msg_mailbox *mb = &topics[i].subs[j].mailbox;
            if (__atomic_load_n(&mb->ready, __ATOMIC_ACQUIRE)) {
                if (topic_out)
                    str_copy(topic_out, (const char *)mb->topic, MSG_ROUTER_TOPIC_LEN);
                return (const char *)mb->data;
            }
        }
    }
    return NULL;
}

/*
 * Acknowledge receipt of a message.
 */
void msg_router_ack(int component_idx)
{
    for (int i = 0; i < MSG_ROUTER_MAX_TOPICS; i++) {
        if (!topics[i].name[0]) continue;
        for (int j = 0; j < MSG_ROUTER_MAX_SUBSCRIBERS; j++) {
            if (topics[i].subs[j].component_idx != component_idx) continue;
            struct msg_mailbox *mb = &topics[i].subs[j].mailbox;
            if (__atomic_load_n(&mb->ready, __ATOMIC_ACQUIRE)) {
                __atomic_store_n(&mb->ready, 0, __ATOMIC_RELEASE);
                __atomic_store_n(&mb->ack, 1, __ATOMIC_RELEASE);
                return;
            }
        }
    }
}

/*
 * List all topics and their subscribers.
 */
void msg_router_list(void)
{
    uart_printf("Message Router (%d topics):\n", topic_count);
    if (topic_count == 0) {
        uart_printf("  (no topics)\n");
        return;
    }

    for (int i = 0; i < MSG_ROUTER_MAX_TOPICS; i++) {
        if (!topics[i].name[0]) continue;
        uart_printf("  Topic '%s' (%d subscribers):\n",
                    topics[i].name, topics[i].sub_count);
        for (int j = 0; j < MSG_ROUTER_MAX_SUBSCRIBERS; j++) {
            if (topics[i].subs[j].component_idx == -1) continue;
            component_info_t info;
            if (component_get_info((uint32_t)topics[i].subs[j].component_idx, &info) == 0) {
                uart_printf("    → component '%s' (idx %d)\n",
                            (const char *)info.name, topics[i].subs[j].component_idx);
            } else {
                uart_printf("    → component idx %d\n", topics[i].subs[j].component_idx);
            }
        }
    }
}

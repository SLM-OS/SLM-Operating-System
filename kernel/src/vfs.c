/*
 * vfs.c - Virtual Filesystem Implementation for SLM-OS
 *
 * In-memory virtual filesystem providing hierarchical access to system resources.
 */

#include "vfs.h"
#include "pmm.h"
#include "task.h"
#include "sched.h"
#include "smp.h"
#include "ipc.h"
#include "uart.h"
#include "slm_ffi.h"
#include <stddef.h>

/* Maximum number of VFS nodes */
#define VFS_MAX_NODES 64

/* Static node pool */
static struct vfs_node node_pool[VFS_MAX_NODES];
static int next_node = 0;

/* Standard mount points */
static struct vfs_node *root_node = NULL;
static struct vfs_node *sys_node = NULL;
static struct vfs_node *proc_node = NULL;
static struct vfs_node *components_node = NULL;

/* ============================================================================
 * Helper functions
 * ============================================================================ */

/*
 * String comparison.
 */
static int vfs_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a - *b;
}

/*
 * String copy with length limit.
 */
static void vfs_strncpy(char *dst, const char *src, size_t max)
{
    size_t i;
    for (i = 0; i < max - 1 && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/*
 * String length.
 */
static size_t vfs_strlen(const char *s)
{
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

/*
 * Allocate a new node from the pool.
 */
static struct vfs_node *alloc_node(void)
{
    if (next_node >= VFS_MAX_NODES) {
        return NULL;
    }

    struct vfs_node *node = &node_pool[next_node++];

    /* Zero initialize */
    node->name[0] = '\0';
    node->type = VFS_NODE_DIR;
    node->read = NULL;
    node->read_ctx = NULL;
    node->num_children = 0;
    node->parent = NULL;

    for (int i = 0; i < VFS_MAX_CHILDREN; i++) {
        node->children[i] = NULL;
    }

    return node;
}

/*
 * Add a child to a directory.
 */
static int add_child(struct vfs_node *parent, struct vfs_node *child)
{
    if (parent->type != VFS_NODE_DIR) {
        return -1;
    }

    if (parent->num_children >= VFS_MAX_CHILDREN) {
        return -1;
    }

    parent->children[parent->num_children++] = child;
    child->parent = parent;

    return 0;
}

/*
 * Find a child by name.
 */
static struct vfs_node *find_child(struct vfs_node *parent, const char *name)
{
    if (parent->type != VFS_NODE_DIR) {
        return NULL;
    }

    for (int i = 0; i < parent->num_children; i++) {
        if (vfs_strcmp(parent->children[i]->name, name) == 0) {
            return parent->children[i];
        }
    }

    return NULL;
}

/* ============================================================================
 * /sys/ virtual file callbacks
 * ============================================================================ */

/*
 * /sys/memory - Memory statistics
 */
static int read_sys_memory(char *buf, size_t size, void *ctx)
{
    (void)ctx;

    size_t total_pages = pmm_get_total_pages();
    size_t free_pages = pmm_get_free_pages();
    size_t used_pages = total_pages - free_pages;

    size_t page_size = 4096;
    size_t total_kb = (total_pages * page_size) / 1024;
    size_t free_kb = (free_pages * page_size) / 1024;
    size_t used_kb = (used_pages * page_size) / 1024;

    /* Format into buffer with proper bounds checking */
    int pos = 0;
    size_t remain = size;
    int wrote;

    wrote = uart_snprintf(buf + pos, remain, "total_kb: %lu\n", total_kb);
    if (wrote > 0) { pos += wrote; remain = (size_t)pos < size ? size - pos : 0; }

    wrote = uart_snprintf(buf + pos, remain, "used_kb: %lu\n", used_kb);
    if (wrote > 0) { pos += wrote; remain = (size_t)pos < size ? size - pos : 0; }

    wrote = uart_snprintf(buf + pos, remain, "free_kb: %lu\n", free_kb);
    if (wrote > 0) { pos += wrote; remain = (size_t)pos < size ? size - pos : 0; }

    wrote = uart_snprintf(buf + pos, remain, "total_pages: %lu\n", total_pages);
    if (wrote > 0) { pos += wrote; remain = (size_t)pos < size ? size - pos : 0; }

    wrote = uart_snprintf(buf + pos, remain, "free_pages: %lu\n", free_pages);
    if (wrote > 0) { pos += wrote; }

    return pos;
}

/*
 * /sys/uptime - System uptime
 */
static int read_sys_uptime(char *buf, size_t size, void *ctx)
{
    (void)ctx;

    extern uint64_t timer_get_count(void);
    extern uint64_t timer_get_frequency(void);

    uint64_t count = timer_get_count();
    uint64_t freq = timer_get_frequency();
    if (freq == 0) freq = 1;

    uint64_t total_seconds = count / freq;
    uint64_t ms = (count * 1000 / freq) % 1000;

    return uart_snprintf(buf, size, "uptime_seconds: %lu\nuptime_ms: %lu\n",
                         (unsigned long)total_seconds, (unsigned long)ms);
}

/*
 * /sys/cpus - CPU information
 */
static int read_sys_cpus(char *buf, size_t size, void *ctx)
{
    (void)ctx;

    int pos = 0;
    size_t remain = size;
    int wrote;

    wrote = uart_snprintf(buf + pos, remain, "count: %lu\n", (unsigned long)cpu_count);
    if (wrote > 0) { pos += wrote; remain = (size_t)pos < size ? size - pos : 0; }

    wrote = uart_snprintf(buf + pos, remain, "online: %lu\n", (unsigned long)cpus_online);
    if (wrote > 0) { pos += wrote; }

    return pos;
}

/*
 * /sys/version - OS version
 */
static int read_sys_version(char *buf, size_t size, void *ctx)
{
    (void)ctx;

    return uart_snprintf(buf, size, "name: SLM-OS\nversion: 0.4.0\nphase: 4\n");
}

/*
 * /sys/ipc - IPC statistics
 */
static int read_sys_ipc(char *buf, size_t size, void *ctx)
{
    (void)ctx;

    struct ipc_stats stats;
    ipc_get_stats(&stats);

    int pos = 0;
    size_t remain = size;
    int wrote;

    wrote = uart_snprintf(buf + pos, remain, "queues: %lu\n", (unsigned long)stats.queue_count);
    if (wrote > 0) { pos += wrote; remain = (size_t)pos < size ? size - pos : 0; }

    wrote = uart_snprintf(buf + pos, remain, "buffers: %lu\n", (unsigned long)stats.buffer_count);
    if (wrote > 0) { pos += wrote; remain = (size_t)pos < size ? size - pos : 0; }

    wrote = uart_snprintf(buf + pos, remain, "msgs_sent: %lu\n", (unsigned long)stats.total_msgs_sent);
    if (wrote > 0) { pos += wrote; remain = (size_t)pos < size ? size - pos : 0; }

    wrote = uart_snprintf(buf + pos, remain, "msgs_recv: %lu\n", (unsigned long)stats.total_msgs_recv);
    if (wrote > 0) { pos += wrote; }

    return pos;
}

/*
 * /sys/model - Model memory pool stats
 */
static int read_sys_model(char *buf, size_t size, void *ctx)
{
    (void)ctx;

    RustPoolStats weight_stats = rust_weight_pool_stats();
    RustPoolStats workspace_stats = rust_workspace_pool_stats();

    int pos = 0;
    size_t remain = size;
    int wrote;

    wrote = uart_snprintf(buf + pos, remain, "[weight_pool]\n");
    if (wrote > 0) { pos += wrote; remain = (size_t)pos < size ? size - pos : 0; }

    wrote = uart_snprintf(buf + pos, remain, "total: %lu\n", (unsigned long)weight_stats.total_blocks);
    if (wrote > 0) { pos += wrote; remain = (size_t)pos < size ? size - pos : 0; }

    wrote = uart_snprintf(buf + pos, remain, "free: %lu\n", (unsigned long)weight_stats.free_blocks);
    if (wrote > 0) { pos += wrote; remain = (size_t)pos < size ? size - pos : 0; }

    wrote = uart_snprintf(buf + pos, remain, "allocated: %lu\n", (unsigned long)weight_stats.allocated_blocks);
    if (wrote > 0) { pos += wrote; remain = (size_t)pos < size ? size - pos : 0; }

    wrote = uart_snprintf(buf + pos, remain, "[workspace_pool]\n");
    if (wrote > 0) { pos += wrote; remain = (size_t)pos < size ? size - pos : 0; }

    wrote = uart_snprintf(buf + pos, remain, "total: %lu\n", (unsigned long)workspace_stats.total_blocks);
    if (wrote > 0) { pos += wrote; remain = (size_t)pos < size ? size - pos : 0; }

    wrote = uart_snprintf(buf + pos, remain, "free: %lu\n", (unsigned long)workspace_stats.free_blocks);
    if (wrote > 0) { pos += wrote; remain = (size_t)pos < size ? size - pos : 0; }

    wrote = uart_snprintf(buf + pos, remain, "allocated: %lu\n", (unsigned long)workspace_stats.allocated_blocks);
    if (wrote > 0) { pos += wrote; }

    return pos;
}

/* ============================================================================
 * /proc/ virtual file callbacks
 * ============================================================================ */

/*
 * State name lookup for tasks
 */
static const char *proc_state_name(task_state_t state)
{
    switch (state) {
        case TASK_READY:      return "ready";
        case TASK_RUNNING:    return "running";
        case TASK_BLOCKED:    return "blocked";
        case TASK_TERMINATED: return "terminated";
        default:              return "unknown";
    }
}

/*
 * /proc/tasks - List of all tasks
 */
static int read_proc_tasks(char *buf, size_t size, void *ctx)
{
    (void)ctx;

    int pos = 0;
    size_t remain = size;
    int wrote;

    for (uint32_t id = 0; id < MAX_TASKS; id++) {
        struct task *t = task_get(id);
        if (t != NULL) {
            wrote = uart_snprintf(buf + pos, remain,
                                 "%lu %s %s %u %lu\n",
                                 (unsigned long)t->id,
                                 t->name,
                                 proc_state_name(t->state),
                                 t->effective_priority,
                                 (unsigned long)t->assigned_cpu);
            if (wrote > 0) {
                pos += wrote;
                remain = (size_t)pos < size ? size - pos : 0;
            }
            if (remain == 0) break;
        }
    }

    return pos;
}

/*
 * /proc/self - Current task info
 */
static int read_proc_self(char *buf, size_t size, void *ctx)
{
    (void)ctx;

    struct task *t = task_current();
    if (!t) {
        return uart_snprintf(buf, size, "error: no current task\n");
    }

    int pos = 0;
    size_t remain = size;
    int wrote;

    wrote = uart_snprintf(buf + pos, remain, "id: %lu\n", (unsigned long)t->id);
    if (wrote > 0) { pos += wrote; remain = (size_t)pos < size ? size - pos : 0; }

    wrote = uart_snprintf(buf + pos, remain, "name: %s\n", t->name);
    if (wrote > 0) { pos += wrote; remain = (size_t)pos < size ? size - pos : 0; }

    wrote = uart_snprintf(buf + pos, remain, "state: %s\n", proc_state_name(t->state));
    if (wrote > 0) { pos += wrote; remain = (size_t)pos < size ? size - pos : 0; }

    wrote = uart_snprintf(buf + pos, remain, "priority: %u\n", t->priority);
    if (wrote > 0) { pos += wrote; remain = (size_t)pos < size ? size - pos : 0; }

    wrote = uart_snprintf(buf + pos, remain, "effective_priority: %u\n", t->effective_priority);
    if (wrote > 0) { pos += wrote; remain = (size_t)pos < size ? size - pos : 0; }

    wrote = uart_snprintf(buf + pos, remain, "cpu: %lu\n", (unsigned long)t->assigned_cpu);
    if (wrote > 0) { pos += wrote; remain = (size_t)pos < size ? size - pos : 0; }

    wrote = uart_snprintf(buf + pos, remain, "switches: %lu\n", (unsigned long)t->switches);
    if (wrote > 0) { pos += wrote; }

    return pos;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

void vfs_init(void)
{
    /* Create root directory */
    root_node = alloc_node();
    vfs_strncpy(root_node->name, "/", VFS_MAX_NAME);
    root_node->type = VFS_NODE_DIR;

    /* Create /sys/ directory */
    sys_node = vfs_create_dir(root_node, "sys");

    /* Populate /sys/ with virtual files */
    vfs_create_file(sys_node, "memory", read_sys_memory, NULL);
    vfs_create_file(sys_node, "uptime", read_sys_uptime, NULL);
    vfs_create_file(sys_node, "cpus", read_sys_cpus, NULL);
    vfs_create_file(sys_node, "version", read_sys_version, NULL);
    vfs_create_file(sys_node, "ipc", read_sys_ipc, NULL);
    vfs_create_file(sys_node, "model", read_sys_model, NULL);

    /* Create /proc/ directory */
    proc_node = vfs_create_dir(root_node, "proc");

    /* Populate /proc/ with virtual files */
    vfs_create_file(proc_node, "tasks", read_proc_tasks, NULL);
    vfs_create_file(proc_node, "self", read_proc_self, NULL);

    /* Create /components/ directory (empty for now, M6 will populate) */
    components_node = vfs_create_dir(root_node, "components");
}

struct vfs_node *vfs_lookup(const char *path)
{
    if (!path || path[0] != '/') {
        return NULL;
    }

    /* Handle root */
    if (path[0] == '/' && (path[1] == '\0' || (path[1] == '/' && path[2] == '\0'))) {
        return root_node;
    }

    struct vfs_node *node = root_node;
    const char *p = path + 1;  /* Skip leading '/' */

    char component[VFS_MAX_NAME];

    while (*p) {
        /* Skip leading slashes */
        while (*p == '/') p++;

        if (*p == '\0') break;

        /* Extract path component */
        int i = 0;
        while (*p && *p != '/' && i < VFS_MAX_NAME - 1) {
            component[i++] = *p++;
        }
        component[i] = '\0';

        /* Look up in current directory */
        node = find_child(node, component);
        if (!node) {
            return NULL;  /* Not found */
        }
    }

    return node;
}

struct vfs_node *vfs_create_file(struct vfs_node *parent, const char *name,
                                  vfs_read_fn read, void *ctx)
{
    if (!parent || parent->type != VFS_NODE_DIR) {
        return NULL;
    }

    struct vfs_node *node = alloc_node();
    if (!node) {
        return NULL;
    }

    vfs_strncpy(node->name, name, VFS_MAX_NAME);
    node->type = VFS_NODE_FILE;
    node->read = read;
    node->read_ctx = ctx;

    if (add_child(parent, node) < 0) {
        /* Can't add, but we've allocated - just leave orphaned for now */
        return NULL;
    }

    return node;
}

struct vfs_node *vfs_create_dir(struct vfs_node *parent, const char *name)
{
    if (!parent || parent->type != VFS_NODE_DIR) {
        return NULL;
    }

    struct vfs_node *node = alloc_node();
    if (!node) {
        return NULL;
    }

    vfs_strncpy(node->name, name, VFS_MAX_NAME);
    node->type = VFS_NODE_DIR;

    if (add_child(parent, node) < 0) {
        return NULL;
    }

    return node;
}

int vfs_read(struct vfs_node *node, char *buf, size_t size)
{
    if (!node || node->type != VFS_NODE_FILE) {
        return -1;
    }

    if (!node->read) {
        return 0;  /* No content */
    }

    return node->read(buf, size, node->read_ctx);
}

void vfs_list(struct vfs_node *node, vfs_list_fn callback, void *ctx)
{
    if (!node || node->type != VFS_NODE_DIR || !callback) {
        return;
    }

    for (int i = 0; i < node->num_children; i++) {
        callback(node->children[i], ctx);
    }
}

struct vfs_node *vfs_root(void)
{
    return root_node;
}

struct vfs_node *vfs_get_sys(void)
{
    return sys_node;
}

struct vfs_node *vfs_get_proc(void)
{
    return proc_node;
}

struct vfs_node *vfs_get_components(void)
{
    return components_node;
}

char *vfs_get_path(struct vfs_node *node, char *buf, size_t size)
{
    if (!node || !buf || size == 0) {
        return NULL;
    }

    /* Build path by traversing up to root */
    char temp[VFS_MAX_PATH];
    int pos = VFS_MAX_PATH - 1;
    temp[pos] = '\0';

    struct vfs_node *n = node;
    while (n && n->parent) {
        size_t len = vfs_strlen(n->name);
        pos -= len;
        if (pos < 1) {
            return NULL;  /* Path too long */
        }
        for (size_t i = 0; i < len; i++) {
            temp[pos + i] = n->name[i];
        }
        pos--;
        temp[pos] = '/';
        n = n->parent;
    }

    /* Handle root */
    if (node == root_node) {
        if (size < 2) return NULL;
        buf[0] = '/';
        buf[1] = '\0';
        return buf;
    }

    /* Copy to output buffer */
    size_t path_len = VFS_MAX_PATH - 1 - pos;
    if (path_len >= size) {
        return NULL;
    }
    for (size_t i = 0; i <= path_len; i++) {
        buf[i] = temp[pos + i];
    }

    return buf;
}

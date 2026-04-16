/*
 * help.c - File-Driven Help System for SLM-OS
 *
 * Centralizes all command help text in one file. Help is written to the
 * filesystem at boot and read on demand, keeping RAM usage minimal.
 *
 * To add help for a new command:
 * 1. Add a HELP_TEXT entry in the help_entries[] array
 * 2. The text will be written to /help/<command>.txt at boot
 */

#include "help.h"
#include "uart.h"
#include "vfs.h"
#include "littlefs_slm.h"
#include "string.h"
#include <stddef.h>

/* ============================================================================
 * Help Text Definitions
 *
 * Each entry defines help for one command. The text is written to
 * /help/<name>.txt at boot time.
 * ============================================================================ */

struct help_entry {
    const char *name;    /* Command name (also filename without .txt) */
    const char *text;    /* Full help text */
};

#define HELP_TEXT(cmd, content) { cmd, content }

static const struct help_entry help_entries[] = {
    HELP_TEXT("help",
        "help - Display command help\n"
        "\n"
        "Usage:\n"
        "  help          List all available commands\n"
        "  help <cmd>    Show detailed help for a command\n"
        "\n"
        "Examples:\n"
        "  help          Show command list\n"
        "  help cp       Show help for the cp command\n"
        "  help grep     Show help for grep\n"
    ),

    HELP_TEXT("ls",
        "ls - List directory contents\n"
        "\n"
        "Usage:\n"
        "  ls [path]     List directory (default: current directory)\n"
        "\n"
        "Output format for mounted filesystems:\n"
        "  name    [f]    size    - Regular file with size in bytes\n"
        "  name    [d]    0       - Directory\n"
        "\n"
        "Examples:\n"
        "  ls            List current directory\n"
        "  ls /          List root directory\n"
        "  ls /sys       List system info directory\n"
        "  ls ..         List parent directory\n"
    ),

    HELP_TEXT("cd",
        "cd - Change current directory\n"
        "\n"
        "Usage:\n"
        "  cd [path]     Change to path (default: /)\n"
        "\n"
        "Special paths:\n"
        "  .             Current directory (no change)\n"
        "  ..            Parent directory\n"
        "  /             Root directory\n"
        "\n"
        "Examples:\n"
        "  cd /mnt/files     Change to files directory\n"
        "  cd ..             Go up one level\n"
        "  cd                Return to root\n"
    ),

    HELP_TEXT("pwd",
        "pwd - Print working directory\n"
        "\n"
        "Usage:\n"
        "  pwd           Display current directory path\n"
        "\n"
        "Example:\n"
        "  pwd\n"
        "  /mnt/files\n"
    ),

    HELP_TEXT("cat",
        "cat - Display file contents\n"
        "\n"
        "Usage:\n"
        "  cat <path> [offset] [length]\n"
        "\n"
        "Arguments:\n"
        "  path          File to display\n"
        "  offset        Start position in bytes (optional)\n"
        "  length        Number of bytes to read (optional)\n"
        "\n"
        "Examples:\n"
        "  cat /sys/memory           Show memory info\n"
        "  cat hello.txt             Show file contents\n"
        "  cat large.bin 0 64        Show first 64 bytes\n"
        "  cat large.bin 64 64       Show next 64 bytes\n"
    ),

    HELP_TEXT("cp",
        "cp - Copy files\n"
        "\n"
        "Usage:\n"
        "  cp <source> <destination>\n"
        "\n"
        "Copies file contents from source to destination. Works across\n"
        "mount points. Destination is overwritten if it exists.\n"
        "\n"
        "Examples:\n"
        "  cp hello.txt backup.txt           Copy in current dir\n"
        "  cp /mnt/files/a.txt ./b.txt       Copy with paths\n"
    ),

    HELP_TEXT("mv",
        "mv - Move or rename files\n"
        "\n"
        "Usage:\n"
        "  mv <source> <destination>\n"
        "\n"
        "Moves or renames a file or directory. Both paths must be\n"
        "in the same mounted filesystem.\n"
        "\n"
        "Examples:\n"
        "  mv old.txt new.txt       Rename file\n"
        "  mv file.txt subdir/      Move to subdirectory\n"
    ),

    HELP_TEXT("rm",
        "rm - Remove files or directories\n"
        "\n"
        "Usage:\n"
        "  rm <path>\n"
        "\n"
        "Removes a file or empty directory. Directories must be\n"
        "empty before removal.\n"
        "\n"
        "Examples:\n"
        "  rm temp.txt      Remove a file\n"
        "  rm emptydir      Remove empty directory\n"
    ),

    HELP_TEXT("mkdir",
        "mkdir - Create directory\n"
        "\n"
        "Usage:\n"
        "  mkdir <path>\n"
        "\n"
        "Creates a new directory in a mounted filesystem.\n"
        "\n"
        "Examples:\n"
        "  mkdir logs              Create in current dir\n"
        "  mkdir /mnt/files/data   Create with full path\n"
    ),

    HELP_TEXT("touch",
        "touch - Create empty file\n"
        "\n"
        "Usage:\n"
        "  touch <path>\n"
        "\n"
        "Creates an empty file if it doesn't exist. If the file\n"
        "already exists, it is NOT modified or truncated.\n"
        "\n"
        "Examples:\n"
        "  touch newfile.txt       Create empty file\n"
        "  touch log.txt           Create log file\n"
    ),

    HELP_TEXT("write",
        "write - Write content to file\n"
        "\n"
        "Usage:\n"
        "  write <path> <content...>\n"
        "\n"
        "Creates or overwrites a file with the given content.\n"
        "All arguments after path are joined with spaces.\n"
        "\n"
        "Examples:\n"
        "  write test.txt Hello World      Write 'Hello World'\n"
        "  write log.txt Started at boot   Write log entry\n"
    ),

    HELP_TEXT("append",
        "append - Append content to file\n"
        "\n"
        "Usage:\n"
        "  append <path> <content...>\n"
        "\n"
        "Appends content to an existing file. Creates the file\n"
        "if it doesn't exist.\n"
        "\n"
        "Examples:\n"
        "  append log.txt New entry        Add to log file\n"
    ),

    HELP_TEXT("truncate",
        "truncate - Truncate file to size\n"
        "\n"
        "Usage:\n"
        "  truncate <path> <size>\n"
        "\n"
        "Truncates a file to the specified size in bytes.\n"
        "Use size 0 to empty a file.\n"
        "\n"
        "Examples:\n"
        "  truncate log.txt 0       Clear log file\n"
        "  truncate data.bin 1024   Truncate to 1KB\n"
    ),

    HELP_TEXT("stat",
        "stat - Show file information\n"
        "\n"
        "Usage:\n"
        "  stat <path>\n"
        "\n"
        "Displays file or directory information including:\n"
        "  - Full path\n"
        "  - Type (regular file, directory, virtual)\n"
        "  - Size in bytes (for files)\n"
        "\n"
        "Examples:\n"
        "  stat hello.txt       Show file info\n"
        "  stat /mnt/files      Show directory info\n"
        "  stat /sys/memory     Show virtual file info\n"
    ),

    HELP_TEXT("tree",
        "tree - Recursive directory listing\n"
        "\n"
        "Usage:\n"
        "  tree [path] [depth]\n"
        "\n"
        "Recursively lists directory contents with indentation.\n"
        "Default path is current directory, default depth is 5.\n"
        "\n"
        "Examples:\n"
        "  tree                  List current dir recursively\n"
        "  tree /mnt/files       List files directory\n"
        "  tree / 2              List root with depth 2\n"
    ),

    HELP_TEXT("wc",
        "wc - Count lines, words, bytes\n"
        "\n"
        "Usage:\n"
        "  wc <path>\n"
        "\n"
        "Counts and displays:\n"
        "  - Number of lines (newline characters)\n"
        "  - Number of words (whitespace-separated)\n"
        "  - Number of bytes (file size)\n"
        "\n"
        "Output format:  lines  words  bytes  path\n"
        "\n"
        "Example:\n"
        "  wc readme.txt\n"
        "        4       12       74  readme.txt\n"
    ),

    HELP_TEXT("hexdump",
        "hexdump - Display file in hexadecimal\n"
        "\n"
        "Usage:\n"
        "  hexdump <path> [offset] [length]\n"
        "\n"
        "Displays file contents in hex with ASCII sidebar.\n"
        "Default length is 256 bytes.\n"
        "\n"
        "Output format:\n"
        "  OFFSET  HEX BYTES (16 per line)  |ASCII|\n"
        "\n"
        "Examples:\n"
        "  hexdump hello.txt             First 256 bytes\n"
        "  hexdump data.bin 0 32         First 32 bytes\n"
        "  hexdump data.bin 256 64       64 bytes at offset 256\n"
    ),

    HELP_TEXT("grep",
        "grep - Search for pattern in file\n"
        "\n"
        "Usage:\n"
        "  grep <pattern> <path>\n"
        "\n"
        "Searches for a substring pattern in the file and prints\n"
        "matching lines with line numbers. Case-sensitive.\n"
        "\n"
        "Output format:\n"
        "  line_number: matching line content\n"
        "  (N matches)\n"
        "\n"
        "Examples:\n"
        "  grep error log.txt       Find lines with 'error'\n"
        "  grep Hello hello.txt     Find 'Hello'\n"
    ),

    HELP_TEXT("find",
        "find - Find files by name pattern\n"
        "\n"
        "Usage:\n"
        "  find <path> <pattern>\n"
        "\n"
        "Recursively searches for files matching the pattern.\n"
        "Works only on mounted filesystems (not virtual dirs).\n"
        "\n"
        "Wildcards:\n"
        "  *     Match any sequence of characters\n"
        "  ?     Match any single character\n"
        "\n"
        "Examples:\n"
        "  find /mnt/files *.txt       All .txt files\n"
        "  find /mnt/files log*        Files starting with 'log'\n"
        "  find /mnt/files test?.c     test1.c, test2.c, etc.\n"
    ),

    HELP_TEXT("df",
        "df - Show filesystem statistics\n"
        "\n"
        "Usage:\n"
        "  df [path]\n"
        "\n"
        "Shows disk usage for mounted filesystems.\n"
        "Default path is /mnt/files.\n"
        "\n"
        "Output:\n"
        "  Filesystem      Blocks     Used     Free   Use%\n"
        "  /mnt/files         256        4      252     1%\n"
        "                   1024K     16K    1008K\n"
    ),

    HELP_TEXT("mem",
        "mem - Show memory statistics\n"
        "\n"
        "Usage:\n"
        "  mem\n"
        "\n"
        "Displays physical memory manager (PMM) statistics:\n"
        "  - Total and free pages\n"
        "  - Memory in KB/MB\n"
        "  - Buddy allocator state\n"
    ),

    HELP_TEXT("tasks",
        "tasks - List all tasks\n"
        "\n"
        "Usage:\n"
        "  tasks\n"
        "\n"
        "Displays all tasks with:\n"
        "  - Task ID\n"
        "  - State (ready, running, blocked, terminated)\n"
        "  - CPU affinity\n"
        "  - Priority\n"
        "  - Task name\n"
    ),

    HELP_TEXT("cpu",
        "cpu - Show CPU status\n"
        "\n"
        "Usage:\n"
        "  cpu\n"
        "\n"
        "Displays per-core information:\n"
        "  - Online status\n"
        "  - Currently running task\n"
        "  - Isolation state\n"
    ),

    HELP_TEXT("kill",
        "kill - Terminate a task\n"
        "\n"
        "Usage:\n"
        "  kill <pid>\n"
        "\n"
        "Terminates the task with the given process ID.\n"
        "Use 'tasks' to see task IDs.\n"
        "\n"
        "Example:\n"
        "  kill 5        Terminate task with ID 5\n"
    ),

    HELP_TEXT("run",
        "run - Run a program\n"
        "\n"
        "Usage:\n"
        "  run <name>\n"
        "\n"
        "Loads and executes an ELF program from the built-in\n"
        "program table. Use 'elftest' to see available programs.\n"
        "\n"
        "Example:\n"
        "  run hello     Run the hello program\n"
    ),

    HELP_TEXT("component",
        "component - Component system management\n"
        "\n"
        "Usage:\n"
        "  component list                           List all components\n"
        "  component register <name> <ver> <type> [pri]\n"
        "                                           Register new component\n"
        "  component unregister <idx>               Unregister by index\n"
        "  component status <name|idx>              Show component details\n"
        "\n"
        "Types: service, driver, application\n"
        "Priorities: idle, low, normal, high, critical\n"
        "\n"
        "Examples:\n"
        "  component register my-svc 1.0.0 service high\n"
        "  component list\n"
        "  component status 0\n"
    ),

    HELP_TEXT("uptime",
        "uptime - Show system uptime\n"
        "\n"
        "Usage:\n"
        "  uptime\n"
        "\n"
        "Displays time since boot in seconds and milliseconds.\n"
    ),

    HELP_TEXT("vmm",
        "vmm - Show virtual memory info\n"
        "\n"
        "Usage:\n"
        "  vmm\n"
        "\n"
        "Displays VMM page table information and memory mappings.\n"
    ),

    HELP_TEXT("ipc",
        "ipc - Show IPC statistics\n"
        "\n"
        "Usage:\n"
        "  ipc\n"
        "\n"
        "Displays message queue and shared buffer statistics.\n"
    ),

    HELP_TEXT("model",
        "model - Show model memory pools\n"
        "\n"
        "Usage:\n"
        "  model\n"
        "\n"
        "Displays model memory allocator status:\n"
        "  - Weight pool (for model parameters)\n"
        "  - Workspace pool (for inference)\n"
    ),

    HELP_TEXT("dtb",
        "dtb - Show device tree info\n"
        "\n"
        "Usage:\n"
        "  dtb\n"
        "\n"
        "Displays parsed Device Tree information or platform defaults\n"
        "if no DTB was provided by the bootloader.\n"
    ),

    HELP_TEXT("clear",
        "clear - Clear the screen\n"
        "\n"
        "Usage:\n"
        "  clear\n"
        "\n"
        "Sends ANSI escape sequence to clear terminal.\n"
    ),

    HELP_TEXT("reboot",
        "reboot - Restart the system\n"
        "\n"
        "Usage:\n"
        "  reboot\n"
        "\n"
        "Restarts the system via PSCI. In QEMU, this exits the emulator.\n"
    ),

    HELP_TEXT("elftest",
        "elftest - Test ELF loader\n"
        "\n"
        "Usage:\n"
        "  elftest\n"
        "\n"
        "Runs ELF loader validation tests including header parsing\n"
        "and architecture verification.\n"
    ),

    HELP_TEXT("net",
        "net - Network control\n"
        "\n"
        "Usage:\n"
        "  net init      Initialize network subsystem\n"
        "  net status    Show network status\n"
        "\n"
        "The network must be initialized before using ping, ifconfig,\n"
        "or netstat commands.\n"
        "\n"
        "Example:\n"
        "  net init\n"
        "  Initializing network...\n"
        "  Network initialized successfully\n"
    ),

    HELP_TEXT("ping",
        "ping - Send ICMP echo request\n"
        "\n"
        "Usage:\n"
        "  ping <ip_address> [count]\n"
        "\n"
        "Sends ICMP echo requests to the specified IP address.\n"
        "Default count is 4 packets.\n"
        "\n"
        "Examples:\n"
        "  ping 10.0.2.2           Ping QEMU gateway (4 packets)\n"
        "  ping 10.0.2.2 10        Send 10 ping packets\n"
        "\n"
        "Note: Requires 'net init' first.\n"
    ),

    HELP_TEXT("ifconfig",
        "ifconfig - Network interface configuration\n"
        "\n"
        "Usage:\n"
        "  ifconfig                          Show current config\n"
        "  ifconfig dhcp                     Enable DHCP\n"
        "  ifconfig <ip> <netmask> <gateway> Set static IP\n"
        "\n"
        "Examples:\n"
        "  ifconfig\n"
        "  sl0: flags=UP,STATIC\n"
        "       ether 52:54:00:12:34:56\n"
        "       inet 10.0.2.15  netmask 255.255.255.0\n"
        "       gateway 10.0.2.2\n"
        "\n"
        "  ifconfig 192.168.1.100 255.255.255.0 192.168.1.1\n"
        "  IP set to 192.168.1.100\n"
    ),

    HELP_TEXT("netstat",
        "netstat - Show network statistics\n"
        "\n"
        "Usage:\n"
        "  netstat\n"
        "\n"
        "Displays network interface statistics:\n"
        "  - RX/TX packet counts\n"
        "  - RX/TX byte counts\n"
        "  - Error and dropped packet counts\n"
        "\n"
        "Example:\n"
        "  netstat\n"
        "  Network Statistics:\n"
        "    RX packets: 42  bytes: 6048\n"
        "    TX packets: 38  bytes: 3192\n"
        "    RX errors:  0  dropped: 0\n"
        "    TX errors:  0\n"
    ),

    HELP_TEXT("lua",
        "lua - Lua scripting environment\n"
        "\n"
        "Usage:\n"
        "  lua              Enter interactive REPL\n"
        "  lua -e \"code\"    Execute Lua code directly\n"
        "  lua <file>       Run script from filesystem\n"
        "\n"
        "REPL Commands:\n"
        "  exit             Exit the REPL\n"
        "  Ctrl+D           Exit the REPL\n"
        "  Ctrl+C           Cancel current line\n"
        "\n"
        "SLM-OS Bindings (slm module):\n"
        "\n"
        "  System:\n"
        "  slm.print(...)              Print to console\n"
        "  slm.uptime()                Uptime in milliseconds\n"
        "  slm.mem_stats()             {total_kb, free_kb, used_kb}\n"
        "  slm.tasks()                 [{id, name, state, cpu, priority}]\n"
        "  slm.sleep(ms)               Sleep for milliseconds\n"
        "  slm.yield()                 Yield CPU to scheduler\n"
        "  slm.version()               SLM-OS version string\n"
        "  slm.cpu_count()             Number of CPUs\n"
        "  slm.cpu_id()                Current CPU ID\n"
        "\n"
        "  Components:\n"
        "  slm.component_count()       Number of registered components\n"
        "  slm.component_list()        [{name, version, type, state, ...}]\n"
        "  slm.component_find(name)    Find by name (index or nil)\n"
        "  slm.component_run(name)     Run built-in component\n"
        "  slm.component_hot_swap(o,n) Replace component, keep subscriptions\n"
        "  slm.component_hot_swap_stateful(o,n) Replace + transfer state\n"
        "\n"
        "  CPU / Memory / IPC:\n"
        "  slm.cpu_info()              Per-CPU state + current_cpu + online_count\n"
        "  slm.vmm_stats()             {l1_tables, l2_tables, blocks_mapped, ...} (ARM64)\n"
        "  slm.ipc_stats()             {queue_count, buffer_count, msgs_sent, recv}\n"
        "\n"
        "  Model Memory + Inference:\n"
        "  slm.model_stats()           {weights={...}, workspace={...}}\n"
        "  slm.model_load_mnist()      Load built-in MNIST model, returns index\n"
        "  slm.model_list()            [{index, name, format, params, ...}]\n"
        "  slm.model_info(index)       Detailed info for one model (or nil)\n"
        "  slm.model_find(name)        Find model by name, returns index or -1\n"
        "  slm.model_infer(index)      Run inference, returns predicted class\n"
        "  slm.model_bench(idx, iters) Run inference benchmark (prints to UART)\n"
        "  slm.infer_stats()           {total, min_ns, max_ns, last_ns, errors}\n"
        "  slm.gpu_status()            {available, name, device, compute_ready}\n"
        "\n"
        "  Message Routing:\n"
        "  slm.msg_publish(topic, data) Publish to topic, returns subscriber count\n"
        "  slm.msg_publish_priority(t, d, prio) Publish with priority 0-255\n"
        "\n"
        "  Scheduler:\n"
        "  slm.sched_policy()          Current policy name (e.g., 'heuristic')\n"
        "  slm.sched_stats()           {task_count, ready_count, ctx_switches, ...}\n"
        "  slm.sched_set_policy(name)  Switch policy at runtime, returns bool\n"
        "  slm.sched_policy_list()     [{name, active}] for all registered policies\n"
        "  slm.ai_sched_stats()        {decisions, fallbacks, ...} or nil\n"
        "\n"
        "  Eviction (requires AI_EVICTION=ON):\n"
        "  slm.eviction_policy()       Current policy name or nil\n"
        "  slm.eviction_set_policy(name) Switch policy, returns bool\n"
        "  slm.eviction_stats()        Detailed stats table or nil\n"
        "\n"
        "  Shell:\n"
        "  slm.read_line()             Read one line from UART (blocks)\n"
        "  slm.shell_exec(cmd)         Run a shell command, returns exit code\n"
        "\n"
        "Examples:\n"
        "  lua /mnt/files/demo.lua      Run the industrial IoT demo\n"
        "  lua /mnt/files/demo_menu.lua Interactive feature-tour menu\n"
        "\n"
        "  lua\n"
        "  >>> slm.print(slm.version())\n"
        "  >>> slm.component_run('sensor_monitor')\n"
        "  >>> slm.msg_publish('/sensors/data', '75')\n"
        "  >>> exit\n"
        "\n"
        "  lua -e \"print(slm.sched_policy())\"\n"
    ),
};

#define NUM_HELP_ENTRIES (sizeof(help_entries) / sizeof(help_entries[0]))

/* ============================================================================
 * Implementation
 * ============================================================================ */

/*
 * Initialize the help system.
 * Creates /help/ directory and writes all help files.
 */
int help_init(void)
{
    /* Get mount context for /mnt/files */
    const char *subpath = NULL;
    struct lfs_mount *mnt = (struct lfs_mount *)vfs_get_mount_ctx("/mnt/files", &subpath);
    if (!mnt) {
        return -1;  /* No filesystem mounted */
    }

    /* Create /help directory */
    int ret = littlefs_mkdir(mnt, "/help");
    if (ret < 0 && ret != LFS_ERR_EXIST) {
        return -1;
    }

    /* Write each help file */
    for (size_t i = 0; i < NUM_HELP_ENTRIES; i++) {
        const struct help_entry *e = &help_entries[i];

        /* Build path: /help/<name>.txt */
        char path[64];
        path[0] = '/'; path[1] = 'h'; path[2] = 'e'; path[3] = 'l';
        path[4] = 'p'; path[5] = '/';

        size_t pos = 6;
        const char *n = e->name;
        while (*n && pos < 55) {
            path[pos++] = *n++;
        }
        path[pos++] = '.'; path[pos++] = 't'; path[pos++] = 'x'; path[pos++] = 't';
        path[pos] = '\0';

        /* Write help text */
        int f = littlefs_file_open(mnt, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
        if (f >= 0) {
            littlefs_file_write(mnt, f, e->text, strlen(e->text));
            littlefs_file_close(mnt, f);
        }
    }

    return 0;
}

/*
 * Display help for a specific command.
 */
int help_show(const char *command)
{
    if (!command || !*command) {
        return -1;
    }

    /* Build path: /mnt/files/help/<command>.txt */
    char path[VFS_MAX_PATH];
    size_t pos = 0;
    const char *prefix = "/mnt/files/help/";
    while (*prefix) path[pos++] = *prefix++;

    const char *c = command;
    while (*c && pos < VFS_MAX_PATH - 5) {
        path[pos++] = *c++;
    }
    path[pos++] = '.'; path[pos++] = 't'; path[pos++] = 'x'; path[pos++] = 't';
    path[pos] = '\0';

    /* Read and display */
    char buf[512];
    int total = 0;
    size_t offset = 0;

    while (1) {
        int bytes = vfs_read_path(path, buf, sizeof(buf) - 1, offset);
        if (bytes <= 0) break;

        buf[bytes] = '\0';
        uart_puts(buf);
        total += bytes;
        offset += bytes;

        if ((size_t)bytes < sizeof(buf) - 1) break;
    }

    if (total == 0) {
        uart_printf("No help available for '%s'\n", command);
        return -1;
    }

    return 0;
}

/*
 * Check if help exists for a command.
 */
int help_exists(const char *command)
{
    if (!command || !*command) {
        return 0;
    }

    /* Check if command is in our help entries */
    for (size_t i = 0; i < NUM_HELP_ENTRIES; i++) {
        if (strcmp(help_entries[i].name, command) == 0) {
            return 1;
        }
    }
    return 0;
}

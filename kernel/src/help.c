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
#include "shell.h"
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

    HELP_TEXT("put",
        "put - Write binary hex to file\n"
        "\n"
        "Usage:\n"
        "  put <path> <hex...>\n"
        "  put -a <path> <hex...>\n"
        "\n"
        "Decodes hexadecimal bytes and writes them to a file. The default\n"
        "mode overwrites the destination; use -a to append another chunk.\n"
        "All hex arguments are concatenated without spaces before decoding.\n"
        "\n"
        "Examples:\n"
        "  put blob.bin 000102ff           Write 4 raw bytes\n"
        "  put -a blob.bin aabbccdd        Append another 4 bytes\n"
    ),

    HELP_TEXT("xput",
        "xput - Framed upload session for binary files\n"
        "\n"
        "Usage:\n"
        "  xput begin <path> <size>\n"
        "  xput chunk <offset> <hex...>\n"
        "  xput status\n"
        "  xput finish\n"
        "  xput abort\n"
        "\n"
        "Starts an upload session, appends offset-checked chunks, and\n"
        "finishes only when the received byte count matches the declared\n"
        "size. This is intended for host upload tools.\n"
        "\n"
        "For higher throughput on large files, prefer `xput-bin` (#597\n"
        "Option B) which streams raw bytes without hex 2x expansion.\n"
        "\n"
        "Examples:\n"
        "  xput begin blob.bin 1024\n"
        "  xput chunk 0 000102ff\n"
        "  xput chunk 4 aabbccdd\n"
        "  xput finish\n"
    ),

    HELP_TEXT("xput-bin",
        "xput-bin - Direct binary upload (#597 Option B)\n"
        "\n"
        "Usage:\n"
        "  xput-bin <path> <total>\n"
        "\n"
        "Streams `total` raw bytes straight into the named file via the\n"
        "current shell session's TCP transport — no hex encoding, no\n"
        "per-line shell parse. Sustains 1+ MB/s on hardware where the\n"
        "framed `xput chunk` protocol caps at ~125 KB/s due to hex 2x\n"
        "expansion + line-edit per-byte overhead.\n"
        "\n"
        "Wire format: telnet IAC byte-stuffed (0xFF in payload doubles\n"
        "to 0xFF 0xFF on the wire per RFC 854). Resume: if the file\n"
        "already exists with size <= total, the kernel reports the\n"
        "existing offset in its `XPUT-BIN ready offset=N` response and\n"
        "the client streams from N onwards.\n"
        "\n"
        "Use `slm-put.py --protocol binary` to drive this from the\n"
        "host side.\n"
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

    HELP_TEXT("canary",
        "canary - Check task stack canaries (#601 Bug B diagnostic)\n"
        "\n"
        "Usage:\n"
        "  canary\n"
        "\n"
        "Iterates every live task and verifies the 64-byte canary\n"
        "pattern at stack_base. Reports the stack inventory\n"
        "([stack_base..stack_top) for each task) and any tasks whose\n"
        "canary has been smashed by a stack overflow or wild write.\n"
        "Diagnostic only — no side effects.\n"
        "\n"
        "Also runs automatically inside the panic handler to surface\n"
        "stack-corruption symptoms at crash time.\n"
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
        "  component list                           List registered components\n"
        "  component builtins                       List built-in components available to run\n"
        "  component run <name>                     Run a built-in component\n"
        "  component swap <old> <new>               Hot-swap: replace old with new\n"
        "  component send <message>                 Send to the echo demo service\n"
        "  component register <name> <ver> <type> [pri]\n"
        "                                           Register a new component\n"
        "  component unregister <idx>               Unregister by index\n"
        "  component status <name|idx>              Show component details\n"
        "\n"
        "Types: service, driver, application\n"
        "Priorities: idle, low, normal, high, critical\n"
        "\n"
        "Examples:\n"
        "  component builtins\n"
        "  component run counter\n"
        "  component swap counter counter_v2\n"
        "  component register my-svc 1.0.0 service high\n"
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
        "model - Model registry and inference control\n"
        "\n"
        "Usage:\n"
        "  model                              Show pools + loaded-model summary\n"
        "  model load <path|mnist>            Load a model from VFS or built-in MNIST\n"
        "  model list                         List loaded models\n"
        "  model info <name|idx>              Detailed info for one model\n"
        "  model unload <name|idx>            Free a model slot\n"
        "  model pin <name|idx>               Lock against LRU eviction\n"
        "  model unpin <name|idx>             Allow LRU eviction\n"
        "  model preload                      Run /mnt/files/preload.conf\n"
        "  model preload-status               Show preload progress\n"
        "  model infer <name|idx>             Inference with zero input (validation only)\n"
        "  model infer-file <name|idx> <path> Inference with fp32 input from VFS\n"
        "  model use-gpu <name|idx> <on|off>  Per-model GPU-dispatch toggle\n"
        "  model bench <name|idx> [iters]     Inference latency benchmark\n"
        "  model stats                        Inference rate / latency stats\n"
        "  model pools                        Weight + workspace pool status\n"
        "  model gpu                          Per-model GPU status (on Jetson)\n"
        "  model engines                      Loaded model engines (mnist, raw, etc.)\n"
        "\n"
        "GPU dispatch decision: `model infer-file` routes through the GPU\n"
        "fastpath when the master `gpu use inference` flag AND the per-model\n"
        "`model use-gpu` flag are BOTH on AND a v6 channel handoff is present\n"
        "(Jetson with --no-gpu-suspend kexec). Otherwise CPU NEON path. See\n"
        "`gpu use status` for the master flag, `model info` for per-model.\n"
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

    HELP_TEXT("dtb-dump",
        "dtb-dump - Dump the firmware-passed DTB as hex\n"
        "\n"
        "Usage:\n"
        "  dtb-dump\n"
        "\n"
        "Prints the runtime DTB (post firmware fix-ups) as a single hex\n"
        "stream between 'DTB-START' and 'DTB-END' markers. Used to diff\n"
        "what the firmware patches into the DTB at handoff vs the on-disk\n"
        ".dtb file.\n"
        "\n"
        "Recovery on the host:\n"
        "  awk '/^DTB-START/{f=1;next} /^DTB-END/{f=0} f' capture.txt \\\n"
        "      | tr -d ' \\r\\n' | xxd -r -p > runtime.dtb\n"
        "  dtc -I dtb -O dts runtime.dtb\n"
        "\n"
        "Originally added during the issue #414 EMMC2 investigation\n"
        "to confirm whether firmware was tagging the DTB differently\n"
        "for Linux vs non-Linux kernel handoff. Kept in tree because\n"
        "DTB triage on Pi/Jetson recurs.\n"
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

    HELP_TEXT("http",
        "http - Minimal HTTP client\n"
        "\n"
        "Usage:\n"
        "  http get <url> <dest> [sha256]\n"
        "\n"
        "Downloads one file over plain HTTP into a mounted filesystem path.\n"
        "Only http:// URLs are supported in this first cut. When a SHA-256\n"
        "hex digest is provided, the download is rejected unless it matches.\n"
        "\n"
        "Examples:\n"
        "  http get http://10.0.2.2:8080/blob.bin /mnt/files/blob.bin\n"
        "  http get http://example.com/policies/a.blob /mnt/files/a.blob\n"
        "  http get http://10.0.2.2:8080/blob.bin /mnt/files/blob.bin \\\n"
        "    0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
        "\n"
        "Note: Requires 'net init' first.\n"
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
        "  slm.component_count()       Number of components\n"
        "  slm.component_list()        [{name, version, type, state, ...}]\n"
        "  slm.component_find(name)    Find by name\n"
        "  slm.component_run(name)     Admin only: run built-in component\n"
        "  slm.component_hot_swap(o,n) Admin only: replace component, keep subscriptions\n"
        "  slm.component_hot_swap_stateful(o,n) Admin only: replace + transfer state\n"
        "\n"
        "  CPU / Memory / IPC:\n"
        "  slm.cpu_info()              Per-CPU state\n"
        "  slm.vmm_stats()             {l1_tables, l2_tables, blocks_mapped, ...} (ARM64)\n"
        "  slm.ipc_stats()             {queue_count, buffer_count, msgs_sent, recv}\n"
        "\n"
        "  Model Memory + Inference:\n"
        "  slm.model_stats()           {weights={...}, workspace={...}}\n"
        "  slm.model_load_mnist()      Admin only: load built-in MNIST model\n"
        "  slm.model_list()            [{index, name, format, params, ...}]\n"
        "  slm.model_info(index)       Detailed info (or nil)\n"
        "  slm.model_find(name)        Find model by name, returns index or -1\n"
        "  slm.model_infer(index)      Admin for now: run inference\n"
        "  slm.model_bench(idx, iters) Admin only: inference benchmark\n"
        "  slm.model_load(path[, n])   Admin only: load ONNX from VFS\n"
        "  slm.infer_stats()           {total, min_ns, max_ns, last_ns, errors}\n"
        "  slm.gpu_status()            GPU status table\n"
        "\n"
        "  Message Routing:\n"
        "  slm.msg_publish(topic, data) Publish to topic, returns subscriber count\n"
        "  slm.msg_publish_priority(t, d, prio) Publish with priority 0-255\n"
        "  slm.msg_subscribe(topic, fn) Register Lua callback; returns handle or nil\n"
        "  slm.msg_unsubscribe(handle)  Remove subscription, returns bool\n"
        "  slm.msg_drain()              Dispatch pending callbacks (auto on yield)\n"
        "\n"
        "  Scheduler:\n"
        "  slm.sched_policy()          Current policy name (e.g., 'heuristic')\n"
        "  slm.sched_stats()           {task_count, ready_count, ctx_switches, ...}\n"
        "  slm.sched_set_policy(name)  Admin only: switch policy at runtime\n"
        "  slm.sched_policy_list()     [{name, active}] for all registered policies\n"
        "  slm.ai_sched_stats()        {decisions, fallbacks, ...} or nil\n"
        "  slm.ai_sched_decision(id)   Last AI decision or nil\n"
        "  slm.task_migrate(id, cpu)   Admin only: move a non-running task\n"
        "  slm.task_create(name, fn)   Admin only: spawn Lua task from bytecode\n"
        "  slm.task_kill(id)           Admin only: terminate task\n"
        "  slm.task_set_priority(id,p) Admin only: change priority 0..7\n"
        "  slm.task_pin(id, cpu)       Admin only: pin task (cpu<0 clears)\n"
        "\n"
        "  Eviction (disabled only with DISABLE_EVICTION=ON):\n"
        "  slm.eviction_policy()       Current policy name or nil\n"
        "  slm.eviction_set_policy(name) Admin only: switch policy\n"
        "  slm.eviction_stats()        Detailed stats table or nil\n"
        "  slm.eviction_model_status(kind)   Admin only: blob state\n"
        "  slm.eviction_model_load(k,p)      Admin only: stage blob\n"
        "  slm.eviction_model_activate(kind) Admin only: activate blob\n"
        "  slm.eviction_model_rollback(kind) Admin only: roll back blob\n"
        "  slm.eviction_model_clear(kind)    Admin only: clear blob slots\n"
        "\n"
        "  Shell:\n"
        "  slm.read_line()             Read one line from UART (blocks)\n"
        "  slm.shell_exec(cmd)         Admin only: run a shell command\n"
        "\n"
        "Examples:\n"
        "  lua /mnt/files/demo.lua      Run the industrial IoT demo\n"
        "  lua\n"
        "  >>> slm.print(slm.version())\n"
        "  >>> slm.msg_publish('/sensors/data', '75')\n"
        "  >>> exit\n"
        "\n"
        "  lua -e \"print(slm.sched_policy())\"\n"
    ),

    /* ====================================================================
     * Entries below are kept alphabetized. The
     * test_every_command_has_help_entry regression test in
     * kernel/tests/test_shell.c walks the live builtin_commands[] +
     * external_commands[] tables and fails if any registered command
     * lacks an entry here, so this list is mechanically synchronised
     * with what `help` enumerates.
     * ==================================================================== */

    HELP_TEXT("admin",
        "admin - Seven-page operator TUI for live observability\n"
        "\n"
        "Usage:\n"
        "  admin\n"
        "\n"
        "Launches a 1 Hz read-only dashboard backed by /mnt/files/admin.lua\n"
        "(embedded in the kernel image and written at boot). Pages 1-7 walk\n"
        "Overview, Tasks, Sched, Eviction, Models, Telemetry, REPL. Press a\n"
        "digit 1-7 to switch pages, 'r' to force a repaint, 'q' or Ctrl-C\n"
        "to exit.\n"
        "\n"
        "Uses the safe `slm.*` bindings only. For control-plane mutation\n"
        "(policy swap, gpu_use_set, model_launch) drop into `lua-admin`.\n"
    ),

    HELP_TEXT("bench",
        "bench - Performance benchmarks\n"
        "\n"
        "Usage:\n"
        "  bench <subcommand> [args...]\n"
        "\n"
        "Subcommands:\n"
        "  context        Context-switch latency (create/switch/destroy)\n"
        "  irq            Interrupt latency (timer tick interval accuracy)\n"
        "  ipc            IPC message round-trip latency\n"
        "  eviction       Eviction-policy fault-rate comparison (#117)\n"
        "  deadline       Deadline-accuracy benchmark\n"
        "  isolate        Core-isolation benchmark\n"
        "  shared         Shared-buffer throughput\n"
        "  smp            Cross-CPU dispatch test (one task per CPU)\n"
        "  stealing       Work-stealing scheduler exercise\n"
        "  matmul [N]     NEON FP32 matmul (default 20 iters)\n"
        "  conv           NEON FP32 conv2d\n"
        "  quant          int8 quantization throughput\n"
        "  gpu            GPU offload latency (where available)\n"
        "  stats          Scheduler statistics dump\n"
        "  all            Run every benchmark above\n"
        "\n"
        "Examples:\n"
        "  bench context\n"
        "  bench matmul 100\n"
        "  bench all\n"
    ),

    HELP_TEXT("bpmp",
        "bpmp - BPMP IPC smoke test (Jetson Orin Nano only)\n"
        "\n"
        "Usage:\n"
        "  bpmp\n"
        "\n"
        "Sends an MRQ_PING to the BPMP via HSP doorbell + IVC ring buffers,\n"
        "then queries a known clock id and prints the result. Verifies the\n"
        "post-kexec BPMP path is alive — required for downstream features\n"
        "(camera clocks, PCIe-C8 bring-up, USB networking).\n"
    ),

    HELP_TEXT("diag",
        "diag - Pi 5 IRQ-delivery diagnostics (PI5_IRQ_DIAG build only)\n"
        "\n"
        "Usage:\n"
        "  diag <el2|vec|fiq|all>\n"
        "\n"
        "Probes which path can deliver hardware timer IRQs to NS EL1/EL2\n"
        "on Pi 5. See `docs/archive/investigations/pi5-preemption-resolution.md` for the\n"
        "interpretation of each subprobe. The `fiq` mode writes\n"
        "ICC_IGRPEN0 — do NOT run on Jetson, it crashes the EL3 handler.\n"
    ),

    HELP_TEXT("eviction",
        "eviction - AI page-eviction policy and statistics\n"
        "\n"
        "Usage:\n"
        "  eviction                                 Show summary + per-pool policy\n"
        "  eviction policy                          List policies + per-pool config\n"
        "  eviction policy <name>                   Set both pools to <name>\n"
        "  eviction policy weight <name>            Set weight pool only (#120)\n"
        "  eviction policy workspace <name>         Set workspace pool only (#120)\n"
        "  eviction stats                           Decisions, fallbacks, latency\n"
        "  eviction trajectory [N]                  Last N CACHEUS weight snapshots\n"
        "  eviction features                        Feature-name introspection\n"
        "  eviction model status                    Runtime blob status (per kind)\n"
        "  eviction model load <kind> <path>        Stage a runtime blob from VFS\n"
        "  eviction model activate <kind>           Activate the staged blob\n"
        "  eviction model rollback <kind>           Roll back to the previous payload\n"
        "  eviction model clear <kind>              Clear staged + active for a kind\n"
        "  eviction model autoload status           Show autoload config (per kind)\n"
        "  eviction model autoload set <kind> <path>\n"
        "                                           Set a kind's autoload path\n"
        "  eviction model autoload clear <kind>     Clear autoload for a kind\n"
        "\n"
        "Policies include LRU, LFU, FirstCandidate, SlmHeuristic, XGBoost,\n"
        "MLP, and CACHEUS (ensemble). Build with DISABLE_EVICTION=ON to\n"
        "compile the framework out entirely.\n"
    ),

    HELP_TEXT("gpu",
        "gpu - GPU information and control\n"
        "\n"
        "Usage:\n"
        "  gpu                              Show GPU driver / aperture status\n"
        "  gpu read <hex-offset>            Dump a 32-bit BAR0 word (Jetson)\n"
        "  gpu qmd-selftest                 Byte-compare the QMD encoder\n"
        "                                   against a host-built reference\n"
        "                                   (Jetson — issue #558 Phase 4)\n"
        "  gpu use status                   Tabular view of all consumer toggles\n"
        "  gpu use sched <on|off>           Scheduler-policy GPU dispatch (scaffold)\n"
        "  gpu use eviction <on|off>        Eviction-policy GPU dispatch (scaffold)\n"
        "  gpu use inference <on|off>       Master GPU inference dispatch toggle\n"
        "\n"
        "The `gpu use` family flips per-consumer flags read by the engine\n"
        "before attempting GPU dispatch. `inference` is wired through the\n"
        "MNIST GA10B fastpath (Jetson, with --no-gpu-suspend kexec). `sched`\n"
        "and `eviction` accept on/off but currently have no GPU forward pass\n"
        "wired — flipping them ON records intent and emits a\n"
        "`note: scaffold only…` warning. See docs/specs/gpu-policy-models.md\n"
        "for the wiring plan.\n"
        "\n"
        "Usage on x86-64 (richer external command, registered via\n"
        "nvidia_gpu_register_shell_commands):\n"
        "  gpu init       Initialise the NVIDIA GSP path\n"
        "  gpu vram       Print VRAM region + WPR boundaries\n"
        "  gpu regs       Dump key bringup registers\n"
        "  gpu sec2       SEC2 / Booter-Load probe\n"
    ),

    HELP_TEXT("hailo",
        "hailo - Hailo NPU control\n"
        "\n"
        "Usage:\n"
        "  hailo                          Status\n"
        "  hailo probe                    Drive PCIe probe + bring-up\n"
        "  hailo boot                     Boot the firmware\n"
        "  hailo load <path>              Load a .hef into the inference engine\n"
        "  hailo fw [log|loghex]          Firmware control / log readback\n"
        "  hailo bist                     Run the on-chip ECC BIST\n"
        "  hailo peek <hex-off>           Read a 32-bit BAR0 register\n"
        "  hailo poke <hex-off> <hex-val> Write a 32-bit BAR0 register\n"
        "  hailo cfgstream <in|out> <ch>  Configure a context-switch stream\n"
        "  hailo cfgdump                  Dump captured CS wire bytes\n"
        "  hailo ctxsmoke [min|out|in|full] CS-translator smoke test\n"
        "\n"
        "Most subcommands mutate driver state. Available on Pi 5 + Jetson\n"
        "when the Hailo NPU is wired up; PCIe enumeration is required\n"
        "before `boot` / `load`.\n"
    ),

    HELP_TEXT("hspdiag",
        "hspdiag - HSP dimensioning + BPMP doorbell probe (Jetson only)\n"
        "\n"
        "Usage:\n"
        "  hspdiag\n"
        "\n"
        "Reads HSP_DIMENSIONING from the top-level HSP block to confirm\n"
        "shared-mailbox / doorbell / shared-semaphore counts, then\n"
        "exercises the BPMP_DOORBELL the BPMP IVC stack uses. Useful\n"
        "when investigating BPMP IPC failure modes after kexec.\n"
    ),

    HELP_TEXT("imx219",
        "imx219 - Read IMX219 sensor CHIP_ID via cam_i2c (Jetson only)\n"
        "\n"
        "Usage:\n"
        "  imx219\n"
        "\n"
        "Hardware Task 1 verification gate of the camera plan: drives the\n"
        "Tegra234 HSI2C controller against the IMX219 sensor at slave 0x10\n"
        "and reads CHIP_ID (registers 0x0000 high + 0x0001 low). Combined\n"
        "value should be 0x0219.\n"
        "\n"
        "Prerequisites:\n"
        "  1. IMX219 module attached to connector A (J17).\n"
        "  2. The IMX219-A DT overlay loaded by Linux pre-kexec.\n"
        "  3. Sensor powered on (XCLK + reset GPIO managed by Hardware\n"
        "     Task 2 sensor driver, or held across kexec by slmos-kexec).\n"
        "\n"
        "A NACK or RX-empty result with init=OK is the expected diagnostic\n"
        "for `sensor in reset / clock-gated`; not an HSI2C bug. See\n"
        "`docs/jetson-camera-imx219-plan.md`.\n"
    ),

    HELP_TEXT("kernel",
        "kernel - Manage staged / active boot kernel image (Pi 5 tryboot)\n"
        "\n"
        "Usage:\n"
        "  kernel status                    Show staged / active / previous state\n"
        "  kernel stage <vfs-path>          Copy a VFS file into the SD boot\n"
        "                                   partition as `tryboot.img` and\n"
        "                                   compute its SHA-256 sidecar.\n"
        "  kernel activate                  Set the Pi 5 tryboot flag and reboot\n"
        "  kernel promote                   Rename `tryboot.img` over\n"
        "                                   `kernel_2712.img` (commit current)\n"
        "  kernel rollback                  Delete `tryboot.img`, clear flag\n"
        "\n"
        "Admin-gated. Drives the dynamic kernel-replace state machine\n"
        "(empty -> staged -> armed -> promoted | rolled_back). The running\n"
        "kernel's identity is reported via the SLMOS_VERSION /\n"
        "SLMOS_BUILD_STAMP / SLMOS_BUILD_SHA triple from build_info.h —\n"
        "see also `cat /sys/version`. Pi 5 only today.\n"
    ),

    HELP_TEXT("lua-admin",
        "lua-admin - Lua REPL with admin/global-control bindings\n"
        "\n"
        "Usage:\n"
        "  lua-admin              Enter admin REPL\n"
        "  lua-admin -e \"code\"    Execute Lua code (admin surface)\n"
        "  lua-admin <file>       Run script with admin surface\n"
        "\n"
        "Identical to `lua` (see `help lua` for the full slm.* binding\n"
        "list) except the slm table also exposes mutating bindings:\n"
        "scheduler policy swap, component hot-swap, model load/unload,\n"
        "task lifecycle, telnetd start/stop, runtime blob activate, etc.\n"
        "Use this REPL when you need to mutate global state; otherwise\n"
        "prefer plain `lua` for safe observability.\n"
    ),

    HELP_TEXT("macbdiag",
        "macbdiag - MACB IRQ delivery diagnostic (Pi 5 + networking only)\n"
        "\n"
        "Usage:\n"
        "  macbdiag\n"
        "\n"
        "Probes whether the BCM2712 GENET MAC IRQ reaches the GIC at NS\n"
        "EL1, then dumps RX/TX counters. Companion to the broader timer\n"
        "IRQ work; Pi 5 cooperative-preemption issue (#99) made hardware\n"
        "IRQ delivery a recurring blocker.\n"
    ),

    HELP_TEXT("msg",
        "msg - Message router commands\n"
        "\n"
        "Usage:\n"
        "  msg                              Show usage (no subcommand)\n"
        "  msg list                         List topics and subscribers\n"
        "  msg send <topic> <data...>       Publish message to topic\n"
        "  msg subscribe <topic> <idx>      Subscribe component <idx> to topic\n"
        "\n"
        "Wildcard subscriptions: a topic ending in `/*` matches every\n"
        "topic with that prefix. The Lua surface (`slm.msg_*`) exposes\n"
        "the same router with per-session callbacks.\n"
        "\n"
        "Examples:\n"
        "  msg list\n"
        "  msg send /sensors/temp 75\n"
        "  msg subscribe /sensors/* 2\n"
    ),

    HELP_TEXT("nvgpu",
        "nvgpu - Jetson nvgpu bring-up (Jetson Orin Nano only)\n"
        "\n"
        "Usage:\n"
        "  nvgpu prepare    Prepare GA10B GPU state (clocks, power)\n"
        "  nvgpu run        Submit a smoke compute kernel\n"
        "  nvgpu info       Print GPU revision / aperture status\n"
        "\n"
        "This command mutates GPU clock/power state via BPMP. Pre-kexec\n"
        "GPU runtime-suspend (slmos-kexec helper) is required for the\n"
        "post-kexec inheritance path to find a sane device.\n"
    ),

    HELP_TEXT("pci",
        "pci - List PCI/PCIe devices (x86-64 only)\n"
        "\n"
        "Usage:\n"
        "  pci\n"
        "\n"
        "Walks the PCI configuration space and prints bus:device.function\n"
        "for every responding endpoint with vendor/device ids and the\n"
        "matched device-class string. Read-only.\n"
    ),

    HELP_TEXT("pcietrain",
        "pcietrain - Tegra PCIe C8 host init + link train + EP probe (Jetson only)\n"
        "\n"
        "Usage:\n"
        "  pcietrain\n"
        "\n"
        "Drives the Tegra PCIe-C8 controller through host-mode init,\n"
        "LTSSM training, and an iATU window probe of the attached\n"
        "endpoint (typically the RTL8168 NIC). Used by #25 for the\n"
        "Jetson networking path.\n"
    ),

    HELP_TEXT("peek",
        "peek - Read physical memory\n"
        "\n"
        "Usage:\n"
        "  peek <phys-hex> [count]\n"
        "\n"
        "Reads `count` 32-bit words (default 1) from physical address\n"
        "<phys-hex>. The address is mapped through the kernel identity\n"
        "map; reads to unmapped or CBB-firewalled regions can crash on\n"
        "Jetson — start with one-word probes when bring-up is uncertain.\n"
        "\n"
        "Examples:\n"
        "  peek 0x40000000          One word at start of RAM\n"
        "  peek 0x17000000 4        First 4 words of GPU BAR0 (Jetson)\n"
    ),

    HELP_TEXT("poke",
        "poke - Write a 32-bit word to physical memory\n"
        "\n"
        "Usage:\n"
        "  poke <phys-hex> <val-hex>\n"
        "\n"
        "Writes the 32-bit value at `<phys-hex>`. Use with care: the\n"
        "address is identity-mapped, so a wrong address can crash the\n"
        "kernel or mutate hardware state. Mutating: serialised against\n"
        "other shell sessions.\n"
        "\n"
        "Example:\n"
        "  poke 0x10000000 deadbeef\n"
    ),

    HELP_TEXT("rtldiag",
        "rtldiag - RTL8168 PCIe probe diagnostic (Jetson + networking)\n"
        "\n"
        "Usage:\n"
        "  rtldiag\n"
        "\n"
        "Walks the post-LTSSM PCIe config space for the RTL8168 NIC,\n"
        "prints BAR programming, link width/speed, and reports whether\n"
        "the device is responsive to MMIO. Companion to `pcietrain`.\n"
    ),

    HELP_TEXT("sched",
        "sched - Scheduler control\n"
        "\n"
        "Usage:\n"
        "  sched                              Show current policy\n"
        "  sched policy                       List all registered policies\n"
        "  sched policy <name>                Switch active policy at runtime\n"
        "  sched stats                        Per-CPU statistics + utilisation\n"
        "  sched model status                 Runtime blob status (CONFIG_AI_SCHEDULER)\n"
        "  sched model load <kind> <path>     Stage a runtime blob from VFS\n"
        "  sched model activate <kind>        Activate a staged blob\n"
        "  sched model rollback <kind>        Roll back to the previous payload\n"
        "  sched model clear <kind>           Clear staged + active for a kind\n"
        "  sched model autoload status        Show autoload config (per kind)\n"
        "  sched model autoload set <kind> <path>\n"
        "                                     Set a kind's autoload path\n"
        "  sched model autoload clear <kind>  Clear autoload for a kind\n"
        "\n"
        "The `model` subcommands are only present when the kernel is\n"
        "built with `CONFIG_AI_SCHEDULER` (i.e. `make kernel AI_SCHED=ON`).\n"
        "\n"
        "Examples:\n"
        "  sched policy                       List\n"
        "  sched policy heuristic             Switch\n"
        "  sched stats\n"
    ),

    HELP_TEXT("sleep",
        "sleep - Sleep for N milliseconds\n"
        "\n"
        "Usage:\n"
        "  sleep <ms>\n"
        "\n"
        "Suspends the shell task for the requested number of milliseconds\n"
        "via the cooperative-tick path. Reports the actually-elapsed time\n"
        "alongside the request — under heavy contention or coarse tick\n"
        "granularity the actual sleep can overshoot the request.\n"
        "\n"
        "Example:\n"
        "  sleep 250        Sleep 250 ms\n"
    ),

    HELP_TEXT("slm",
        "slm - Small language model control (Phase SLM, M7)\n"
        "\n"
        "Usage:\n"
        "  slm <verb> [args...]\n"
        "\n"
        "Verbs:\n"
        "  load   <vfs-path>                    Parse a GGUF model into the SLM registry\n"
        "  list                                 List loaded SLMs (handle, arch, vocab)\n"
        "  info   <handle>                      Show full metadata for a model\n"
        "  unload <handle>                      Remove a model from the registry\n"
        "  launch <handle> [opts]               Open a session over a loaded model\n"
        "                                       (--ctx N --sampler greedy|temp|topk|topp|topkp\n"
        "                                        --temp F --topk N --topp F --seed N)\n"
        "  prompt <session> <text...>           Run a prompt; tokens stream to UART\n"
        "  stream <session>                     Read prompt from UART line, then prompt\n"
        "  stop   <session>                     Cooperative cancel for an in-flight decode\n"
        "  reset  <session>                     Clear KV cache (keeps model loaded)\n"
        "  close  <session>                     Close a session, free its KV slot\n"
        "  status                               One-line summary (models / sessions)\n"
        "  stats  <session>                     Per-session telemetry (TTFT, decode, totals)\n"
        "  gpu                                  GPU dispatch state (M6.A-3 deferred)\n"
        "\n"
        "Note (M5.2 stub): the decoder runs end-to-end but the forward\n"
        "step emits zero-logit tokens until M5.3 wires real GGUF weights.\n"
        "Use `slm prompt 0 \"\"` (empty) to exercise the state machine\n"
        "without hitting the not-yet-plumbed tokenizer.\n"
        "\n"
        "Example:\n"
        "  slm load /mnt/files/qwen2.5-1.5b-instruct-q4_k_m.gguf\n"
        "  slm launch 0 --ctx 4096 --sampler topkp --temp 0.7 --topk 40 --topp 0.9\n"
        "  slm prompt 0 \"Hello, world!\"\n"
        "  slm stats 0\n"
    ),

    HELP_TEXT("tcpsh",
        "tcpsh - Deprecated alias for `telnetd`\n"
        "\n"
        "Usage:\n"
        "  tcpsh ...\n"
        "\n"
        "Identical behaviour to `telnetd`; argv[0] is preserved for\n"
        "self-consistent status output. New scripts should use\n"
        "`telnetd`. See `help telnetd` for the subcommand list.\n"
    ),

    HELP_TEXT("telemetry",
        "telemetry - Admin telemetry feed\n"
        "\n"
        "Usage:\n"
        "  telemetry                              Same as `telemetry stats`\n"
        "  telemetry stats                        Per-topic publish counters + total\n"
        "  telemetry list-topics                  Show the eviction + inference topic names\n"
        "  telemetry server start [port]          Start the network feed (default port 2325)\n"
        "  telemetry server stop                  Stop the network feed\n"
        "  telemetry server status                Listener state + per-server counters\n"
        "  telemetry server sessions              List connected peers\n"
        "  telemetry server kick <id>             Force-disconnect a session by id\n"
        "\n"
        "The kernel publishes structured events on message-router topics\n"
        "from the eviction decision site and the inference call site. Any\n"
        "Lua client can subscribe to the wildcard topic `tel.*` to\n"
        "consume the whole feed — see the Lua binding\n"
        "`slm.telemetry_subscribe(pattern, fn)`.\n"
        "\n"
        "`telemetry server` exposes the same `tel.*` events to a host-side\n"
        "monitor over TCP. Wire format: newline-terminated\n"
        "`<topic> seq=<n> ts=<ms> <payload>` records. Per-client glob filter\n"
        "(default `tel.*`) re-settable via `SUB <pattern>` on the same\n"
        "connection; `BYE` closes gracefully. Slow-client policy is\n"
        "per-client drop-oldest — the publisher is never blocked.\n"
        "\n"
        "Example:\n"
        "  telemetry list-topics\n"
        "  telemetry server start              # then `nc <host> 2325` from a workstation\n"
        "  lua -e 'slm.telemetry_subscribe(\"tel.*\", function(t,d) print(t,d) end)'\n"
    ),

    HELP_TEXT("telnetd",
        "telnetd - Telnet shell daemon\n"
        "\n"
        "Usage:\n"
        "  telnetd start [port]            Start listener (default port 2323)\n"
        "  telnetd stop                    Stop listener (existing sessions live on)\n"
        "  telnetd status                  Listener state, port, session counts\n"
        "  telnetd sessions                List active sessions (id, peer, age)\n"
        "  telnetd kick <id>               Force-disconnect a session by id\n"
        "\n"
        "Each accepted connection runs an independent shell session with\n"
        "its own line buffer and per-session cwd. The same Lua + admin\n"
        "split applies as on the UART console.\n"
        "\n"
        "Requires `net init` (or DHCP) to have completed first.\n"
    ),

    HELP_TEXT("timdiag",
        "timdiag - Timer / interrupt-delivery diagnostic (non-x86)\n"
        "\n"
        "Usage:\n"
        "  timdiag [fiq]\n"
        "\n"
        "Dumps live ARM generic-timer and GIC state — Group register\n"
        "configuration, CPU interface registers, SPI group bitmap,\n"
        "PPI/SGI status — to help diagnose hardware-timer IRQ delivery.\n"
        "Useful for investigating the cooperative-preemption blocker\n"
        "(#99 on Pi 5, equivalent on Jetson).\n"
        "\n"
        "**Do not** pass `fiq` on Jetson. The optional FIQ delivery\n"
        "test writes ICC_IGRPEN0; TF-A traps the access on Jetson and\n"
        "crashes the EL3 handler. Pi 5 / QEMU only.\n"
    ),

    HELP_TEXT("top",
        "top - Live system dashboard\n"
        "\n"
        "Usage:\n"
        "  top [-n <iter>] [refresh_secs]\n"
        "\n"
        "Refreshes a fixed-screen dashboard with task table + per-CPU\n"
        "load + memory + scheduler / eviction summary. Press `q` or\n"
        "Ctrl+C to exit.\n"
        "\n"
        "Arguments:\n"
        "  -n <iter>     Stop after N refreshes (default: run until quit)\n"
        "  refresh_secs  Refresh interval, 1..N seconds (default 1)\n"
        "\n"
        "Examples:\n"
        "  top              Dashboard, refresh once per second\n"
        "  top 2            Refresh every 2 seconds\n"
        "  top -n 10        Quit after 10 refreshes\n"
    ),

    HELP_TEXT("xhci",
        "xhci - Tegra XHCI controller info (Jetson only)\n"
        "\n"
        "Usage:\n"
        "  xhci\n"
        "\n"
        "Reads the Tegra XUSB host controller capability + operational\n"
        "registers and prints HCS/HCC params, port count, and current\n"
        "USBSTS. Read-only diagnostic for the post-kexec USB networking\n"
        "path (#266 / #309).\n"
    ),

    HELP_TEXT("xhcidiag",
        "xhcidiag - Tegra XHCI CBB-at-EL2 probe (Jetson + networking)\n"
        "\n"
        "Usage:\n"
        "  xhcidiag\n"
        "\n"
        "Like `xhci`, but specifically targets the Tegra XHCI MMIO/SMMU\n"
        "envelope to identify which sub-apertures CBB lets through at\n"
        "NS EL2 vs which ones still raise RAS. See\n"
        "`docs/jetson-cbb-report.md` and `docs/jetson-usb-networking-plan.md`.\n"
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
        /* shell_puts routes through the bound session's io backend;
         * UART gets `\n` -> `\r\n` translation in shell_io_uart, and
         * telnet gets the same translation in shell_io_tcp. Using
         * uart_puts here would dump the help text to the physical
         * UART even when the user is on a telnet session, leaving
         * the telnet client with no visible output. */
        shell_puts(buf);
        total += bytes;
        offset += bytes;

        if ((size_t)bytes < sizeof(buf) - 1) break;
    }

    if (total == 0) {
        shell_printf("No help available for '%s'\n", command);
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

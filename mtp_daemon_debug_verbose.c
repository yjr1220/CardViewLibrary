#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <stdarg.h>
#include <sys/time.h>
#include <stdint.h>
#include <pthread.h>

#define WATCH_DIR "/mnt/extsd"
#define MTP_FIFO_NAME "/tmp/.mtp_fifo"
#define PID_FILE "/var/run/mtp-daemon.pid"

#define EVENT_SIZE (sizeof(struct inotify_event))
#define EVENT_BUF_LEN (1024 * (EVENT_SIZE + 16))

#define MAX_PATH_LEN 1024
#define OPERATION_QUIET_TIME 2
#define EVENT_CHECK_INTERVAL 100

volatile sig_atomic_t daemon_running = 1;

/* -------------------- 调试日志函数 -------------------- */

static void debug_log(const char *format, ...) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
    
    printf("[%s] ", time_str);
    
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    
    printf("\n");
    fflush(stdout);
}

/* -------------------- MTP Command Structure -------------------- */

typedef struct {
    uint32_t action;
    uint32_t type;
    uint32_t srcPathLen;
    uint32_t destPathLen;
    char *path;
} mtp_command_t;

enum {
    MTP_TOOLS_FUNCTION_ADD = 0,
    MTP_TOOLS_FUNCTION_REMOVE,
    MTP_TOOLS_FUNCTION_UPDATE,
    MTP_TOOLS_FUNCTION_CUT,
    MTP_TOOLS_FUNCTION_COPY,
    MTP_TOOLS_FUNCTION_CONNECT = 100,
};

enum {
    MTP_TOOLS_TYPE_FILE = 0,
    MTP_TOOLS_TYPE_DIR,
};

/* -------------------- 状态跟踪 -------------------- */

static time_t last_activity = 0;
static int pending_update = 0;
static pthread_mutex_t simple_mutex = PTHREAD_MUTEX_INITIALIZER;

/* -------------------- Signals -------------------- */

static void signal_handler(int sig) {
    switch (sig) {
        case SIGHUP:
            debug_log("Received SIGHUP");
            break;
        case SIGTERM:
        case SIGINT:
            debug_log("Received SIGTERM/SIGINT, shutting down");
            daemon_running = 0;
            break;
    }
}

static void setup_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGCHLD, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    
    debug_log("Signal handlers setup completed");
}

/* -------------------- Utils -------------------- */

static int directory_exists(const char *path) {
    struct stat st;
    int result = (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
    debug_log("Directory exists check: %s = %s", path, result ? "YES" : "NO");
    return result;
}

static int ensure_fifo(const char *path) {
    debug_log("Checking FIFO: %s", path);
    
    if (access(path, F_OK) == 0) {
        debug_log("FIFO already exists: %s", path);
        
        // 检查FIFO属性
        struct stat st;
        if (stat(path, &st) == 0) {
            debug_log("FIFO mode: 0%o, is_fifo: %s", st.st_mode & 0777, S_ISFIFO(st.st_mode) ? "YES" : "NO");
        }
        return 0;
    }
    
    debug_log("Creating FIFO: %s", path);
    if (mkfifo(path, 0666) != 0) {
        debug_log("Failed to create FIFO: %s, error: %s", path, strerror(errno));
        return -1;
    }
    
    debug_log("FIFO created successfully: %s", path);
    return 0;
}

static int should_ignore_path(const char *path) {
    if (!path) return 1;
    
    if (strstr(path, "/.tmp/") || strstr(path, "\\.tmp\\")) {
        debug_log("Ignoring tmp directory path: %s", path);
        return 1;
    }
    
    const char *filename = strrchr(path, '/');
    if (filename) filename++; else filename = path;
    
    if (strstr(filename, ".tmp") || 
        strstr(filename, ".temp") ||
        strstr(filename, ".part") ||
        strstr(filename, "~") ||
        (strncmp(filename, ".", 1) == 0 && strcmp(filename, "..") != 0) ||
        (strlen(filename) > 10 && strspn(filename, "0123456789") == strlen(filename))) {
        debug_log("Ignoring temp file: %s", path);
        return 1;
    }
    
    return 0;
}

static const char* get_event_type_name(struct inotify_event *event) {
    if (event->mask & IN_CREATE) return "CREATE";
    if (event->mask & IN_DELETE) return "DELETE";
    if (event->mask & IN_DELETE_SELF) return "DELETE_SELF";
    if (event->mask & IN_MODIFY) return "MODIFY";
    if (event->mask & IN_MOVED_FROM) return "MOVED_FROM";
    if (event->mask & IN_MOVED_TO) return "MOVED_TO";
    if (event->mask & IN_CLOSE_WRITE) return "CLOSE_WRITE";
    return "UNKNOWN";
}

/* -------------------- MTP Tools Integration -------------------- */

static int mtp_tools_send_command(int fd, uint32_t action, uint32_t type, const char *spath, const char *dpath)
{
    debug_log("=== MTP Command Details ===");
    debug_log("Action: %u (%s)", action, 
        action == MTP_TOOLS_FUNCTION_ADD ? "ADD" :
        action == MTP_TOOLS_FUNCTION_REMOVE ? "REMOVE" :
        action == MTP_TOOLS_FUNCTION_UPDATE ? "UPDATE" : "UNKNOWN");
    debug_log("Type: %u (%s)", type, 
        type == MTP_TOOLS_TYPE_FILE ? "FILE" : 
        type == MTP_TOOLS_TYPE_DIR ? "DIR" : "UNKNOWN");
    debug_log("Source Path: %s", spath ? spath : "NULL");
    debug_log("Dest Path: %s", dpath ? dpath : "NULL");
    
    int ret;
    mtp_command_t *command;
    size_t spathLen = 0, dpathLen = 0, pathLen = 0;
    size_t command_size = 0;

    if (spath != NULL) {
        spathLen = strlen(spath) + 1;
        dpathLen = (dpath != NULL) ? (strlen(dpath) + 1) : 0;
        pathLen = spathLen + dpathLen;
    } else if (action != MTP_TOOLS_FUNCTION_CONNECT) {
        debug_log("ERROR: No source path provided for non-CONNECT action");
        return -1;
    }

    command_size = sizeof(mtp_command_t) + pathLen;
    debug_log("Command size: %zu bytes", command_size);

    command = calloc(1, command_size);
    if (!command) {
        debug_log("ERROR: Failed to allocate memory for command");
        return -1;
    }
    
    command->action = action;
    command->type = type;
    command->srcPathLen = spathLen;
    command->destPathLen = dpathLen;
    
    debug_log("Command structure: action=%u, type=%u, srcPathLen=%u, destPathLen=%u", 
              command->action, command->type, command->srcPathLen, command->destPathLen);
    
    if (spathLen > 1) {
        command->path = (char *)&command[1];
        strcpy(command->path, spath);
        debug_log("Path copied to command: %s", command->path);
    } else if (action != MTP_TOOLS_FUNCTION_CONNECT) {
        debug_log("ERROR: Invalid path length for non-CONNECT action");
        free(command);
        return -1;
    }
    
    if (dpathLen > 1) {
        strcpy(command->path + spathLen, dpath);
        debug_log("Destination path copied: %s", dpath);
    }

    debug_log("Writing %zu bytes to FIFO fd %d", command_size, fd);
    ret = write(fd, command, command_size);
    debug_log("Write result: %d bytes written", ret);
    
    if (ret < 0) {
        debug_log("Write error: %s", strerror(errno));
    } else if (ret != (int)command_size) {
        debug_log("WARNING: Partial write - expected %zu, wrote %d", command_size, ret);
    }
    
    free(command);
    debug_log("=== MTP Command End ===");
    
    return (ret == (int)command_size) ? 0 : -1;
}

static void send_mtp_update(void) {
    static int fifo_fd = -1;
    static int open_attempts = 0;
    
    debug_log(">>> Starting MTP update process");
    
    // 检查FIFO文件状态
    struct stat fifo_stat;
    if (stat(MTP_FIFO_NAME, &fifo_stat) == 0) {
        debug_log("FIFO file exists - mode: 0%o, size: %ld, is_fifo: %s", 
                  fifo_stat.st_mode & 0777, fifo_stat.st_size, 
                  S_ISFIFO(fifo_stat.st_mode) ? "YES" : "NO");
    } else {
        debug_log("ERROR: FIFO file does not exist: %s", strerror(errno));
        return;
    }
    
    // 尝试打开FIFO
    if (fifo_fd < 0) {
        open_attempts++;
        debug_log("Attempting to open FIFO (attempt #%d): %s", open_attempts, MTP_FIFO_NAME);
        
        fifo_fd = open(MTP_FIFO_NAME, O_WRONLY | O_NONBLOCK);
        if (fifo_fd < 0) {
            debug_log("ERROR: Failed to open FIFO: %s", strerror(errno));
            debug_log("FIFO open failed - errno: %d (%s)", errno, 
                      errno == ENXIO ? "No reader" :
                      errno == EACCES ? "Permission denied" :
                      errno == ENOENT ? "File not found" : "Other error");
            return;
        } else {
            debug_log("SUCCESS: FIFO opened successfully, fd = %d", fifo_fd);
        }
    } else {
        debug_log("Using existing FIFO fd: %d", fifo_fd);
    }
    
    // 检查FIFO状态
    int flags = fcntl(fifo_fd, F_GETFL);
    debug_log("FIFO flags: 0x%x (O_NONBLOCK: %s)", flags, 
              (flags & O_NONBLOCK) ? "YES" : "NO");
    
    // 发送MTP命令
    debug_log("Sending MTP UPDATE command to root directory: %s", WATCH_DIR);
    int ret = mtp_tools_send_command(fifo_fd, MTP_TOOLS_FUNCTION_UPDATE, MTP_TOOLS_TYPE_DIR, WATCH_DIR, NULL);
    
    if (ret >= 0) {
        debug_log("SUCCESS: MTP command sent successfully");
    } else {
        debug_log("ERROR: MTP command failed, closing FIFO");
        close(fifo_fd);
        fifo_fd = -1;
        open_attempts = 0;
    }
    
    debug_log("<<< MTP update process completed");
}

/* -------------------- 活动跟踪 -------------------- */

static void mark_activity(const char *event_type, const char *path) {
    pthread_mutex_lock(&simple_mutex);
    
    time_t now = time(NULL);
    time_t prev_activity = last_activity;
    
    last_activity = now;
    pending_update = 1;
    
    debug_log("*** ACTIVITY MARKED ***");
    debug_log("Event: %s", event_type);
    debug_log("Path: %s", path);
    debug_log("Previous activity: %ld seconds ago", prev_activity > 0 ? now - prev_activity : 0);
    debug_log("Pending update: YES");
    
    pthread_mutex_unlock(&simple_mutex);
}

static void check_and_update(void) {
    pthread_mutex_lock(&simple_mutex);
    
    time_t now = time(NULL);
    
    if (pending_update) {
        time_t quiet_time = now - last_activity;
        debug_log("Checking update conditions - quiet time: %ld seconds (need: %d)", 
                  quiet_time, OPERATION_QUIET_TIME);
        
        if (quiet_time >= OPERATION_QUIET_TIME) {
            debug_log("*** QUIET TIME REACHED - SENDING UPDATE ***");
            pthread_mutex_unlock(&simple_mutex);
            
            send_mtp_update();
            
            pthread_mutex_lock(&simple_mutex);
            pending_update = 0;
            debug_log("*** UPDATE COMPLETED - PENDING CLEARED ***");
        }
    }
    
    pthread_mutex_unlock(&simple_mutex);
}

/* -------------------- Watch map (wd <-> path) -------------------- */

struct watch_entry {
    int wd;
    char path[MAX_PATH_LEN];
    struct watch_entry *next;
};

static struct watch_entry *watch_list = NULL;

static const char* path_for_wd(int wd) {
    for (struct watch_entry *e = watch_list; e; e = e->next) {
        if (e->wd == wd) return e->path;
    }
    debug_log("WARNING: No path found for wd: %d", wd);
    return NULL;
}

static void add_watch_entry(int wd, const char *path) {
    struct watch_entry *e = (struct watch_entry*)malloc(sizeof(struct watch_entry));
    if (!e) {
        debug_log("ERROR: Failed to allocate watch entry");
        return;
    }
    e->wd = wd;
    snprintf(e->path, sizeof(e->path), "%s", path);
    e->next = watch_list;
    watch_list = e;
    debug_log("Added watch entry: wd=%d, path=%s", wd, path);
}

static void remove_watch_entry_by_wd(int wd) {
    struct watch_entry *prev = NULL, *cur = watch_list;
    while (cur) {
        if (cur->wd == wd) {
            debug_log("Removing watch entry: wd=%d, path=%s", wd, cur->path);
            if (prev) prev->next = cur->next; else watch_list = cur->next;
            free(cur);
            return;
        }
        prev = cur;
        cur = cur->next;
    }
    debug_log("WARNING: Watch entry not found for removal: wd=%d", wd);
}

static void remove_watch_entries_by_path_prefix(int inotify_fd, const char *path_prefix) {
    struct watch_entry *prev = NULL, *cur = watch_list;
    size_t prefix_len = strlen(path_prefix);
    int removed_count = 0;
    
    debug_log("Removing watch entries with prefix: %s", path_prefix);
    
    while (cur) {
        if (strncmp(cur->path, path_prefix, prefix_len) == 0 && 
            (cur->path[prefix_len] == '/' || cur->path[prefix_len] == '\0')) {
            
            debug_log("Removing inotify watch: wd=%d, path=%s", cur->wd, cur->path);
            inotify_rm_watch(inotify_fd, cur->wd);
            removed_count++;
            
            if (prev) {
                prev->next = cur->next;
                free(cur);
                cur = prev->next;
            } else {
                watch_list = cur->next;
                free(cur);
                cur = watch_list;
            }
        } else {
            prev = cur;
            cur = cur->next;
        }
    }
    
    debug_log("Removed %d watch entries with prefix: %s", removed_count, path_prefix);
}

/* -------------------- Inotify setup -------------------- */

static uint32_t WATCH_MASK =
    IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MODIFY |
    IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE;

static int add_watch_for_dir(int inotify_fd, const char *dirpath) {
    debug_log("Adding inotify watch for directory: %s", dirpath);
    
    int wd = inotify_add_watch(inotify_fd, dirpath, WATCH_MASK);
    if (wd < 0) {
        debug_log("ERROR: Failed to add watch for %s: %s", dirpath, strerror(errno));
        return -1;
    }
    
    debug_log("SUCCESS: Watch added - wd=%d, path=%s, mask=0x%x", wd, dirpath, WATCH_MASK);
    add_watch_entry(wd, dirpath);
    return wd;
}

static int add_watch_recursive(int inotify_fd, const char *dirpath) {
    debug_log("Adding recursive watch for: %s", dirpath);
    
    if (!directory_exists(dirpath)) {
        debug_log("ERROR: Directory does not exist: %s", dirpath);
        return -1;
    }
    
    if (add_watch_for_dir(inotify_fd, dirpath) < 0) {
        debug_log("ERROR: Failed to add watch for root directory: %s", dirpath);
        return -1;
    }

    DIR *d = opendir(dirpath);
    if (!d) {
        debug_log("ERROR: Failed to open directory: %s", dirpath);
        return -1;
    }

    struct dirent *ent;
    char child[MAX_PATH_LEN];
    int child_count = 0;
    
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        
        snprintf(child, sizeof(child), "%s/%s", dirpath, ent->d_name);
        struct stat st;
        if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            debug_log("Found child directory: %s", child);
            add_watch_recursive(inotify_fd, child);
            child_count++;
        }
    }
    closedir(d);
    
    debug_log("Recursive watch completed for %s - found %d child directories", dirpath, child_count);
    return 0;
}

/* -------------------- 事件处理 -------------------- */

static void handle_inotify_event(int inotify_fd, struct inotify_event *event) {
    if (event->len == 0) {
        debug_log("WARNING: Event with zero length name");
        return;
    }

    const char *base = path_for_wd(event->wd);
    if (!base) {
        debug_log("ERROR: No base path found for wd: %d", event->wd);
        return;
    }

    char full_path[MAX_PATH_LEN];
    if (snprintf(full_path, sizeof(full_path), "%s/%s", base, event->name) >= (int)sizeof(full_path)) {
        debug_log("ERROR: Path too long: %s/%s", base, event->name);
        return;
    }

    debug_log("=== INOTIFY EVENT ===");
    debug_log("Type: %s", get_event_type_name(event));
    debug_log("Path: %s", full_path);
    debug_log("Mask: 0x%x", event->mask);
    debug_log("Is Directory: %s", (event->mask & IN_ISDIR) ? "YES" : "NO");
    debug_log("Watch Descriptor: %d", event->wd);
    debug_log("Base Path: %s", base);
    debug_log("File Name: %s", event->name);

    // 过滤临时文件
    if (should_ignore_path(full_path)) {
        debug_log("Event ignored (temp file)");
        debug_log("=== EVENT END ===");
        return;
    }

    // 处理所有有效事件
    if (event->mask & (IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MODIFY | 
                      IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE)) {
        
        debug_log("Processing valid event");
        
        // 如果是目录删除，清理监控项
        if ((event->mask & (IN_DELETE | IN_DELETE_SELF)) && (event->mask & IN_ISDIR)) {
            debug_log("Directory deletion detected, cleaning up watches");
            remove_watch_entries_by_path_prefix(inotify_fd, full_path);
        }
        
        // 如果是目录创建，添加监控
        if ((event->mask & IN_CREATE) && (event->mask & IN_ISDIR)) {
            debug_log("Directory creation detected, adding recursive watch");
            add_watch_recursive(inotify_fd, full_path);
        }
        
        // 标记活动
        mark_activity(get_event_type_name(event), full_path);
    } else {
        debug_log("Event not processed (invalid mask)");
    }
    
    debug_log("=== EVENT END ===");
}

/* -------------------- Main -------------------- */

int main(int argc, char *argv[]) {
    debug_log("=== MTP DAEMON DEBUG VERSION STARTING ===");
    debug_log("Watch Directory: %s", WATCH_DIR);
    debug_log("MTP FIFO: %s", MTP_FIFO_NAME);
    debug_log("PID File: %s", PID_FILE);
    debug_log("Event Check Interval: %dms", EVENT_CHECK_INTERVAL);
    debug_log("Operation Quiet Time: %ds", OPERATION_QUIET_TIME);

    if (!directory_exists(WATCH_DIR)) {
        debug_log("FATAL: Watch directory does not exist: %s", WATCH_DIR);
        return EXIT_FAILURE;
    }
    
    if (ensure_fifo(MTP_FIFO_NAME) != 0) {
        debug_log("FATAL: Failed to ensure FIFO exists: %s", MTP_FIFO_NAME);
        return EXIT_FAILURE;
    }

    // 检查是否以daemon模式运行
    int daemon_mode = !(argc > 1 && strcmp(argv[1], "--no-daemon") == 0);
    debug_log("Daemon mode: %s", daemon_mode ? "YES" : "NO");
    
    if (daemon_mode) {
        debug_log("WARNING: Running in daemon mode - logs will be redirected");
        // 注释掉daemonize调用以便调试
        // daemonize();
    }

    setup_signal_handlers();

    debug_log("Initializing inotify");
    int inotify_fd = inotify_init1(IN_NONBLOCK);
    if (inotify_fd < 0) {
        debug_log("FATAL: Failed to initialize inotify: %s", strerror(errno));
        return EXIT_FAILURE;
    }
    debug_log("Inotify initialized successfully, fd = %d", inotify_fd);

    debug_log("Adding recursive watches");
    if (add_watch_recursive(inotify_fd, WATCH_DIR) != 0) {
        debug_log("FATAL: Failed to add recursive watch for %s", WATCH_DIR);
        close(inotify_fd);
        return EXIT_FAILURE;
    }

    debug_log("=== MTP DAEMON READY - MONITORING STARTED ===");

    int loop_count = 0;
    while (daemon_running) {
        loop_count++;
        
        fd_set read_fds;
        struct timeval timeout;

        FD_ZERO(&read_fds);
        FD_SET(inotify_fd, &read_fds);

        timeout.tv_sec = 0;
        timeout.tv_usec = EVENT_CHECK_INTERVAL * 1000;

        int ret = select(inotify_fd + 1, &read_fds, NULL, NULL, &timeout);
        
        if (loop_count % 100 == 0) {  // 每10秒打印一次状态
            debug_log("Main loop status - count: %d, select result: %d", loop_count, ret);
        }
        
        if (ret < 0) {
            if (errno == EINTR) {
                debug_log("Select interrupted by signal");
                continue;
            }
            debug_log("ERROR: Select failed: %s", strerror(errno));
            break;
        }

        // 检查是否需要发送更新
        check_and_update();
        
        if (ret == 0) continue;  // Timeout

        if (FD_ISSET(inotify_fd, &read_fds)) {
            debug_log("Inotify data available, reading events");
            
            char buffer[EVENT_BUF_LEN];
            ssize_t length = read(inotify_fd, buffer, sizeof(buffer));
            
            debug_log("Read %zd bytes from inotify", length);
            
            if (length > 0) {
                int i = 0;
                int event_count = 0;
                while (i < length) {
                    struct inotify_event *event = (struct inotify_event *)&buffer[i];
                    event_count++;
                    debug_log("Processing event #%d", event_count);
                    handle_inotify_event(inotify_fd, event);
                    i += EVENT_SIZE + event->len;
                }
                debug_log("Processed %d events from this read", event_count);
            } else if (length < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                debug_log("ERROR: Inotify read failed: %s", strerror(errno));
                break;
            }
        }
    }

    debug_log("=== MTP DAEMON SHUTTING DOWN ===");

    // 清理资源
    int cleanup_count = 0;
    while (watch_list) {
        inotify_rm_watch(inotify_fd, watch_list->wd);
        struct watch_entry *tmp = watch_list;
        watch_list = watch_list->next;
        free(tmp);
        cleanup_count++;
    }
    debug_log("Cleaned up %d watch entries", cleanup_count);
    
    close(inotify_fd);
    remove(PID_FILE);

    debug_log("=== MTP DAEMON SHUTDOWN COMPLETE ===");
    return EXIT_SUCCESS;
}
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
#define OPERATION_QUIET_TIME 2  // 2秒静默时间
#define EVENT_CHECK_INTERVAL 200  // 200ms检查间隔，更频繁
#define MAX_EVENT_WAIT_TIME 8     // 8秒最大等待
#define MAX_PENDING_PATHS 6       // 6个路径
#define DELETION_FINAL_DELAY 500000  // 删除后额外等待500ms再最终更新

volatile sig_atomic_t daemon_running = 1;

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

/* -------------------- 删除操作跟踪 -------------------- */

typedef struct {
    char deleted_paths[MAX_PENDING_PATHS][MAX_PATH_LEN];
    time_t deletion_times[MAX_PENDING_PATHS];
    int deletion_count;
    time_t last_deletion_time;
} deletion_state_t;

static deletion_state_t deletion_state;
static pthread_mutex_t deletion_mutex = PTHREAD_MUTEX_INITIALIZER;

/* -------------------- 操作状态跟踪 -------------------- */

typedef struct {
    char paths[MAX_PENDING_PATHS][MAX_PATH_LEN];
    time_t path_times[MAX_PENDING_PATHS];
    int path_count;
    time_t last_global_event;
    int total_events;
} operation_state_t;

static operation_state_t operation_state;
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;

/* -------------------- Signals -------------------- */

static void signal_handler(int sig) {
    switch (sig) {
        case SIGHUP:
            break;
        case SIGTERM:
        case SIGINT:
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
}

/* -------------------- Utils -------------------- */

static int directory_exists(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

static int is_regular_file(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

static int ensure_fifo(const char *path) {
    if (access(path, F_OK) == 0) return 0;
    if (mkfifo(path, 0666) != 0) {
        return -1;
    }
    return 0;
}

// 获取路径的父目录
static void get_parent_directory(const char *path, char *parent, size_t parent_size) {
    strncpy(parent, path, parent_size - 1);
    parent[parent_size - 1] = '\0';
    
    char *last_slash = strrchr(parent, '/');
    if (last_slash && last_slash != parent) {
        *last_slash = '\0';
    } else {
        strncpy(parent, WATCH_DIR, parent_size - 1);
        parent[parent_size - 1] = '\0';
    }
}

// 获取最上层的变更目录
static void get_top_level_change_dir(const char *path, char *result, size_t result_size) {
    if (strncmp(path, WATCH_DIR, strlen(WATCH_DIR)) != 0) {
        strncpy(result, WATCH_DIR, result_size - 1);
        result[result_size - 1] = '\0';
        return;
    }
    
    const char *relative_path = path + strlen(WATCH_DIR);
    if (relative_path[0] == '/') relative_path++;
    
    if (strlen(relative_path) == 0) {
        strncpy(result, WATCH_DIR, result_size - 1);
        result[result_size - 1] = '\0';
        return;
    }
    
    // 找到第一个目录分隔符
    const char *first_slash = strchr(relative_path, '/');
    if (first_slash) {
        snprintf(result, result_size, "%s/%.*s", 
                WATCH_DIR, (int)(first_slash - relative_path), relative_path);
    } else {
        snprintf(result, result_size, "%s/%s", WATCH_DIR, relative_path);
    }
}

// 改进的临时文件检测
static int should_ignore_path(const char *path) {
    if (!path) return 1;
    
    // 检查是否在.tmp目录中
    if (strstr(path, "/.tmp/") || strstr(path, "\\.tmp\\")) {
        return 1;
    }
    
    // 检查文件名
    const char *filename = strrchr(path, '/');
    if (filename) filename++; else filename = path;
    
    // 忽略的文件模式
    if (strstr(filename, ".tmp") || 
        strstr(filename, ".temp") ||
        strstr(filename, ".part") ||
        strstr(filename, "~") ||
        (strncmp(filename, ".", 1) == 0 && strcmp(filename, "..") != 0) ||
        strncmp(filename, "#", 1) == 0 ||
        (strlen(filename) > 10 && strspn(filename, "0123456789") == strlen(filename))) {
        return 1;
    }
    
    return 0;
}

// 获取事件类型描述
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
    int ret;
    mtp_command_t *command;
    size_t spathLen = 0, dpathLen = 0, pathLen = 0;
    size_t command_size = 0;

    if (spath != NULL) {
        spathLen = strlen(spath) + 1;
        dpathLen = (dpath != NULL) ? (strlen(dpath) + 1) : 0;
        pathLen = spathLen + dpathLen;
    } else if (action != MTP_TOOLS_FUNCTION_CONNECT) {
        return -1;
    }

    command_size = sizeof(mtp_command_t) + pathLen;

    command = calloc(1, command_size);
    if (!command) return -1;
    
    command->action = action;
    command->type = type;
    command->srcPathLen = spathLen;
    command->destPathLen = dpathLen;
    
    if (spathLen > 1) {
        command->path = (char *)&command[1];
        strcpy(command->path, spath);
    } else if (action != MTP_TOOLS_FUNCTION_CONNECT) {
        free(command);
        return -1;
    }
    
    if (dpathLen > 1) {
        strcpy(command->path + spathLen, dpath);
    }

    ret = write(fd, command, command_size);
    free(command);
    
    return (ret == (int)command_size) ? 0 : -1;
}

static int execute_mtp_command_with_retry(const char *function, const char *type, const char *path, int max_retries) {
    static int fifo_fd = -1;
    int retry_count = 0;
    
    // 对于删除操作，总是更新根目录以确保完全刷新
    const char *update_path = (strcmp(function, "update") == 0) ? WATCH_DIR : path;
    
    // 确保路径存在且是目录（删除操作除外）
    if (strcmp(function, "update") == 0 && !directory_exists(update_path)) {
        printf("Skipping MTP update for non-existent directory: %s\n", update_path);
        return -1;
    }
    
    while (retry_count < max_retries) {
        if (fifo_fd < 0) {
            fifo_fd = open(MTP_FIFO_NAME, O_WRONLY | O_NONBLOCK);
            if (fifo_fd < 0) {
                retry_count++;
                usleep(200000); // 200ms
                continue;
            }
        }
        
        uint32_t action;
        uint32_t mtp_type;
        
        if (strcmp(function, "add") == 0) {
            action = MTP_TOOLS_FUNCTION_ADD;
        } else if (strcmp(function, "remove") == 0) {
            action = MTP_TOOLS_FUNCTION_REMOVE;
        } else if (strcmp(function, "update") == 0) {
            action = MTP_TOOLS_FUNCTION_UPDATE;
        } else {
            return -1;
        }
        
        if (strcmp(type, "FILE") == 0) {
            mtp_type = MTP_TOOLS_TYPE_FILE;
        } else if (strcmp(type, "DIR") == 0) {
            mtp_type = MTP_TOOLS_TYPE_DIR;
        } else {
            return -1;
        }
        
        printf("Sending MTP command: function=%s, type=%s, path=%s\n", function, type, update_path);
        
        int ret = mtp_tools_send_command(fifo_fd, action, mtp_type, update_path, NULL);
        if (ret >= 0) {
            printf("MTP command sent successfully\n");
            return 0;
        } else {
            printf("MTP command failed, retrying... (%d/%d)\n", retry_count + 1, max_retries);
            close(fifo_fd);
            fifo_fd = -1;
            retry_count++;
            usleep(200000); // 200ms
        }
    }
    
    printf("MTP command failed after %d retries\n", max_retries);
    return -1;
}

static int execute_mtp_command(const char *function, const char *type, const char *path) {
    return execute_mtp_command_with_retry(function, type, path, 3);
}

/* -------------------- 删除操作跟踪 -------------------- */

static void add_deletion_tracking(const char *deleted_path) {
    pthread_mutex_lock(&deletion_mutex);
    
    time_t now = time(NULL);
    deletion_state.last_deletion_time = now;
    
    // 添加到删除跟踪列表
    if (deletion_state.deletion_count < MAX_PENDING_PATHS) {
        strncpy(deletion_state.deleted_paths[deletion_state.deletion_count], 
               deleted_path, MAX_PATH_LEN - 1);
        deletion_state.deleted_paths[deletion_state.deletion_count][MAX_PATH_LEN - 1] = '\0';
        deletion_state.deletion_times[deletion_state.deletion_count] = now;
        deletion_state.deletion_count++;
    }
    
    printf("Added deletion tracking for: %s (total: %d)\n", deleted_path, deletion_state.deletion_count);
    
    pthread_mutex_unlock(&deletion_mutex);
}

static void process_pending_deletions(void) {
    pthread_mutex_lock(&deletion_mutex);
    
    time_t now = time(NULL);
    
    if (deletion_state.deletion_count > 0) {
        // 检查是否有足够的静默时间（1秒）
        if ((now - deletion_state.last_deletion_time) >= 1) {
            printf("Processing %d pending deletions after quiet time\n", deletion_state.deletion_count);
            
            // 强制更新根目录，确保MTP服务器完全刷新
            printf("Sending final deletion update to root directory\n");
            pthread_mutex_unlock(&deletion_mutex);
            execute_mtp_command_with_retry("update", "DIR", WATCH_DIR, 5);
            pthread_mutex_lock(&deletion_mutex);
            
            // 清空删除跟踪列表
            deletion_state.deletion_count = 0;
            
            printf("All pending deletions processed\n");
        }
    }
    
    pthread_mutex_unlock(&deletion_mutex);
}

/* -------------------- 操作跟踪 -------------------- */

static void add_pending_update_path(const char *path) {
    pthread_mutex_lock(&state_mutex);
    
    time_t now = time(NULL);
    operation_state.last_global_event = now;
    operation_state.total_events++;
    
    // 如果是文件，使用父目录
    char update_path[MAX_PATH_LEN];
    if (is_regular_file(path)) {
        get_parent_directory(path, update_path, sizeof(update_path));
        printf("File detected, updating parent directory: %s -> %s\n", path, update_path);
    } else {
        strncpy(update_path, path, sizeof(update_path) - 1);
        update_path[sizeof(update_path) - 1] = '\0';
    }
    
    // 检查路径是否已存在
    int found = -1;
    for (int i = 0; i < operation_state.path_count; i++) {
        if (strcmp(operation_state.paths[i], update_path) == 0) {
            found = i;
            break;
        }
    }
    
    if (found >= 0) {
        operation_state.path_times[found] = now;
    } else if (operation_state.path_count < MAX_PENDING_PATHS) {
        strncpy(operation_state.paths[operation_state.path_count], 
               update_path, MAX_PATH_LEN - 1);
        operation_state.paths[operation_state.path_count][MAX_PATH_LEN - 1] = '\0';
        operation_state.path_times[operation_state.path_count] = now;
        operation_state.path_count++;
    }
    
    printf("Added pending update path: %s (total paths: %d)\n", 
           update_path, operation_state.path_count);
    
    pthread_mutex_unlock(&state_mutex);
}

static void process_pending_updates(void) {
    pthread_mutex_lock(&state_mutex);
    
    time_t now = time(NULL);
    
    if (operation_state.path_count > 0) {
        if ((now - operation_state.last_global_event) >= OPERATION_QUIET_TIME) {
            printf("Processing %d pending updates after quiet time\n", operation_state.path_count);
            
            // 发送所有待更新路径的MTP命令
            for (int i = 0; i < operation_state.path_count; i++) {
                printf("Sending update for path: %s\n", operation_state.paths[i]);
                pthread_mutex_unlock(&state_mutex);
                execute_mtp_command("update", "DIR", operation_state.paths[i]);
                pthread_mutex_lock(&state_mutex);
                usleep(100000); // 100ms
            }
            
            operation_state.path_count = 0;
            operation_state.total_events = 0;
            
            printf("All pending updates processed\n");
        }
    }
    
    pthread_mutex_unlock(&state_mutex);
}

/* -------------------- 简化但强化的删除处理 -------------------- */

static void handle_deletion_immediately(const char *deleted_path) {
    printf("Deletion detected: %s\n", deleted_path);
    
    // 立即发送根目录更新，确保MTP服务器刷新
    printf("Sending immediate root directory update for deletion\n");
    execute_mtp_command_with_retry("update", "DIR", WATCH_DIR, 5);
    
    // 添加到删除跟踪，稍后再次确认更新
    add_deletion_tracking(deleted_path);
    
    printf("Deletion handling completed for: %s\n", deleted_path);
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
    return NULL;
}

static void add_watch_entry(int wd, const char *path) {
    struct watch_entry *e = (struct watch_entry*)malloc(sizeof(struct watch_entry));
    if (!e) return;
    e->wd = wd;
    snprintf(e->path, sizeof(e->path), "%s", path);
    e->next = watch_list;
    watch_list = e;
}

static void remove_watch_entry_by_wd(int wd) {
    struct watch_entry *prev = NULL, *cur = watch_list;
    while (cur) {
        if (cur->wd == wd) {
            if (prev) prev->next = cur->next; else watch_list = cur->next;
            free(cur);
            return;
        }
        prev = cur;
        cur = cur->next;
    }
}

static void remove_watch_entries_by_path_prefix(int inotify_fd, const char *path_prefix) {
    struct watch_entry *prev = NULL, *cur = watch_list;
    size_t prefix_len = strlen(path_prefix);
    
    while (cur) {
        if (strncmp(cur->path, path_prefix, prefix_len) == 0 && 
            (cur->path[prefix_len] == '/' || cur->path[prefix_len] == '\0')) {
            
            inotify_rm_watch(inotify_fd, cur->wd);
            printf("Removed watch for deleted path: %s\n", cur->path);
            
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
}

/* -------------------- Inotify setup -------------------- */

static uint32_t WATCH_MASK =
    IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MODIFY |
    IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE;

static int add_watch_for_dir(int inotify_fd, const char *dirpath) {
    int wd = inotify_add_watch(inotify_fd, dirpath, WATCH_MASK);
    if (wd < 0) {
        return -1;
    }
    add_watch_entry(wd, dirpath);
    return wd;
}

static int add_watch_recursive(int inotify_fd, const char *dirpath) {
    if (!directory_exists(dirpath)) return -1;
    if (add_watch_for_dir(inotify_fd, dirpath) < 0) return -1;

    DIR *d = opendir(dirpath);
    if (!d) return -1;

    struct dirent *ent;
    char child[MAX_PATH_LEN];
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        snprintf(child, sizeof(child), "%s/%s", dirpath, ent->d_name);
        struct stat st;
        if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            add_watch_recursive(inotify_fd, child);
        }
    }
    closedir(d);
    return 0;
}

/* -------------------- 事件处理 -------------------- */

static void handle_inotify_event(int inotify_fd, struct inotify_event *event) {
    if (event->len == 0) return;

    const char *base = path_for_wd(event->wd);
    if (!base) return;

    char full_path[MAX_PATH_LEN];
    if (snprintf(full_path, sizeof(full_path), "%s/%s", base, event->name) >= (int)sizeof(full_path)) {
        return;
    }

    printf("Event: %s on %s (mask: 0x%x)\n", get_event_type_name(event), full_path, event->mask);

    // 过滤临时文件
    if (should_ignore_path(full_path)) {
        printf("Ignoring path: %s\n", full_path);
        return;
    }

    // 立即处理删除事件
    if (event->mask & (IN_DELETE | IN_DELETE_SELF | IN_MOVED_FROM)) {
        // 如果是目录删除，清理相关的监控项
        if (event->mask & IN_ISDIR) {
            remove_watch_entries_by_path_prefix(inotify_fd, full_path);
        }
        
        // 立即处理删除
        handle_deletion_immediately(full_path);
        return;
    }
    
    // 处理其他事件
    if (event->mask & (IN_CREATE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO)) {
        add_pending_update_path(full_path);
        
        // 如果是目录创建或移动到，需要添加监控
        if ((event->mask & IN_ISDIR) && (event->mask & (IN_CREATE | IN_MOVED_TO))) {
            add_watch_recursive(inotify_fd, full_path);
        }
    }
}

/* -------------------- Inotify Recovery -------------------- */

static int recover_inotify_watches(int inotify_fd) {
    while (watch_list) {
        inotify_rm_watch(inotify_fd, watch_list->wd);
        struct watch_entry *tmp = watch_list;
        watch_list = watch_list->next;
        free(tmp);
    }
    
    return add_watch_recursive(inotify_fd, WATCH_DIR);
}

/* -------------------- Daemonize -------------------- */

static void daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);

    umask(0);
    if (setsid() < 0) exit(EXIT_FAILURE);
    if (chdir("/") < 0) exit(EXIT_FAILURE);

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    open("/dev/null", O_RDONLY);
    open("/dev/null", O_RDWR);
    open("/dev/null", O_RDWR);

    FILE *pidfile = fopen(PID_FILE, "w");
    if (pidfile) {
        fprintf(pidfile, "%d\n", getpid());
        fclose(pidfile);
    }
}

/* -------------------- Main -------------------- */

int main(int argc, char *argv[]) {
    int inotify_fd;

    if (!directory_exists(WATCH_DIR)) {
        printf("Watch directory %s does not exist\n", WATCH_DIR);
        return EXIT_FAILURE;
    }
    if (ensure_fifo(MTP_FIFO_NAME) != 0) {
        printf("Failed to create MTP FIFO %s\n", MTP_FIFO_NAME);
        return EXIT_FAILURE;
    }

    if (!(argc > 1 && strcmp(argv[1], "--no-daemon") == 0)) {
        daemonize();
    }

    setup_signal_handlers();

    inotify_fd = inotify_init1(IN_NONBLOCK);
    if (inotify_fd < 0) {
        printf("Failed to initialize inotify\n");
        return EXIT_FAILURE;
    }

    if (add_watch_recursive(inotify_fd, WATCH_DIR) != 0) {
        printf("Failed to add recursive watch for %s\n", WATCH_DIR);
        close(inotify_fd);
        return EXIT_FAILURE;
    }

    printf("MTP daemon started, watching %s\n", WATCH_DIR);

    int inotify_recover_count = 0;
    const int MAX_RECOVER_ATTEMPTS = 3;
    
    while (daemon_running) {
        fd_set read_fds;
        struct timeval timeout;

        FD_ZERO(&read_fds);
        FD_SET(inotify_fd, &read_fds);

        timeout.tv_sec = 0;
        timeout.tv_usec = EVENT_CHECK_INTERVAL * 1000;

        int ret = select(inotify_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (ret < 0) {
            if (errno == EINTR) continue;
            
            if (inotify_recover_count < MAX_RECOVER_ATTEMPTS) {
                printf("Select error, attempting recovery...\n");
                sleep(1);
                if (recover_inotify_watches(inotify_fd) == 0) {
                    inotify_recover_count++;
                    continue;
                }
            }
            break;
        }

        // 定期处理待更新的路径和待删除的路径
        process_pending_updates();
        process_pending_deletions();
        
        if (ret == 0) continue;

        if (FD_ISSET(inotify_fd, &read_fds)) {
            char buffer[EVENT_BUF_LEN];
            ssize_t length;
            
            while ((length = read(inotify_fd, buffer, sizeof(buffer))) > 0) {
                int i = 0;
                while (i < length) {
                    struct inotify_event *event = (struct inotify_event *)&buffer[i];
                    handle_inotify_event(inotify_fd, event);
                    i += EVENT_SIZE + event->len;
                }
            }
            
            if (length < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                if (inotify_recover_count < MAX_RECOVER_ATTEMPTS) {
                    printf("Read error, attempting recovery...\n");
                    sleep(1);
                    if (recover_inotify_watches(inotify_fd) == 0) {
                        inotify_recover_count++;
                        continue;
                    }
                }
                break;
            }
        }
        
        inotify_recover_count = 0;
    }

    printf("MTP daemon shutting down...\n");

    // 清理资源
    while (watch_list) {
        inotify_rm_watch(inotify_fd, watch_list->wd);
        struct watch_entry *tmp = watch_list;
        watch_list = watch_list->next;
        free(tmp);
    }
    
    close(inotify_fd);
    remove(PID_FILE);

    return EXIT_SUCCESS;
}
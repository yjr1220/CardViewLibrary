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
#define OPERATION_QUIET_TIME 3  // 增加到3秒，确保拷贝操作完成
#define EVENT_CHECK_INTERVAL 500  // 保持500ms检查间隔
#define MAX_EVENT_WAIT_TIME 15   // 增加最大等待时间到15秒，适应大文件拷贝
#define COPY_OPERATION_MIN_TIME 2  // 拷贝操作最小持续时间（秒）

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

/* -------------------- 改进的操作状态跟踪 -------------------- */

typedef enum {
    OP_STATE_IDLE = 0,        // 空闲状态
    OP_STATE_STARTING,        // 操作开始
    OP_STATE_IN_PROGRESS,     // 操作进行中
    OP_STATE_ENDING,          // 操作结束中
    OP_STATE_COMPLETED,       // 操作完成
    OP_STATE_COPY_IN_PROGRESS,// 拷贝操作进行中
    OP_STATE_DELETION         // 删除操作（需要立即处理）
} operation_state_enum_t;

typedef struct {
    char path[MAX_PATH_LEN];
    char deleted_path[MAX_PATH_LEN];  // 记录被删除的路径
    time_t last_event_time;   // 最后事件时间
    time_t operation_start;   // 操作开始时间
    operation_state_enum_t state;
    int event_count;          // 事件计数
    int pending_events;       // 待处理事件数
    int is_deletion;          // 是否为删除操作
    int is_copy_operation;    // 是否为拷贝操作
    int create_events;        // 创建事件计数
    int modify_events;        // 修改事件计数
    int close_write_events;   // 写入完成事件计数
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

static int file_exists(const char *path) {
    return (access(path, F_OK) == 0);
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

// 改进的删除路径处理：对于删除操作，使用父目录进行更新
static void get_update_path_for_deletion(const char *deleted_path, char *result, size_t result_size) {
    // 对于删除操作，总是使用父目录来更新MTP
    get_parent_directory(deleted_path, result, result_size);
    
    // 如果父目录也不存在，则使用根目录
    if (!directory_exists(result)) {
        strncpy(result, WATCH_DIR, result_size - 1);
        result[result_size - 1] = '\0';
    }
}

// 检测是否为临时文件或系统文件
static int is_temp_or_system_file(const char *filename) {
    if (!filename) return 0;
    
    // 检查常见的临时文件模式
    if (strstr(filename, ".tmp") || 
        strstr(filename, ".temp") ||
        strstr(filename, "~") ||
        strncmp(filename, ".", 1) == 0) {
        return 1;
    }
    
    return 0;
}

// 检测事件是否为真正的删除操作（非拷贝过程中的临时删除）
static int is_real_deletion_event(struct inotify_event *event, const char *full_path) {
    // 如果是临时文件的删除，不认为是真正的删除操作
    if (is_temp_or_system_file(event->name)) {
        return 0;
    }
    
    // 如果当前正在进行拷贝操作，延迟判断删除事件
    pthread_mutex_lock(&state_mutex);
    int is_copy_active = (operation_state.is_copy_operation && 
                         operation_state.state == OP_STATE_COPY_IN_PROGRESS);
    pthread_mutex_unlock(&state_mutex);
    
    if (is_copy_active) {
        return 0; // 拷贝过程中的删除事件延迟处理
    }
    
    return (event->mask & (IN_DELETE | IN_DELETE_SELF | IN_MOVED_FROM));
}

// 检测事件是否为拷贝操作的开始
static int is_copy_operation_start(struct inotify_event *event) {
    // 目录创建通常是拷贝操作的开始
    return (event->mask & IN_CREATE) && (event->mask & IN_ISDIR);
}

// 检测事件是否为操作开始标志
static int is_operation_start_event(struct inotify_event *event) {
    return (event->mask & (IN_CREATE | IN_MOVED_FROM)) && 
           (event->mask & IN_ISDIR);
}

// 检测事件是否为操作进行中标志
static int is_operation_progress_event(struct inotify_event *event) {
    return (event->mask & (IN_CREATE | IN_MODIFY | IN_CLOSE_WRITE | 
                          IN_MOVED_FROM | IN_MOVED_TO));
}

// 检测事件是否为操作结束标志
static int is_operation_end_event(struct inotify_event *event) {
    return (event->mask & (IN_CLOSE_WRITE | IN_MOVED_TO));
}

// 检测拷贝操作是否完成
static int is_copy_operation_complete(void) {
    pthread_mutex_lock(&state_mutex);
    
    time_t now = time(NULL);
    int is_complete = 0;
    
    if (operation_state.is_copy_operation && 
        operation_state.state == OP_STATE_COPY_IN_PROGRESS) {
        
        // 检查是否有足够的静默时间
        if ((now - operation_state.last_event_time) >= OPERATION_QUIET_TIME) {
            // 检查是否达到最小拷贝时间
            if ((now - operation_state.operation_start) >= COPY_OPERATION_MIN_TIME) {
                is_complete = 1;
            }
        }
        
        // 检查是否超时
        if ((now - operation_state.operation_start) >= MAX_EVENT_WAIT_TIME) {
            is_complete = 1;
        }
    }
    
    pthread_mutex_unlock(&state_mutex);
    return is_complete;
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

static void execute_mtp_command(const char *function, const char *type, const char *path) {
    static int fifo_fd = -1;
    
    if (fifo_fd < 0) {
        fifo_fd = open(MTP_FIFO_NAME, O_WRONLY | O_NONBLOCK);
        if (fifo_fd < 0) {
            return;
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
        return;
    }
    
    if (strcmp(type, "FILE") == 0) {
        mtp_type = MTP_TOOLS_TYPE_FILE;
    } else if (strcmp(type, "DIR") == 0) {
        mtp_type = MTP_TOOLS_TYPE_DIR;
    } else {
        return;
    }
    
    // 调试信息：打印发送的命令
    printf("Sending MTP command: function=%s, type=%s, path=%s\n", function, type, path);
    
    int ret = mtp_tools_send_command(fifo_fd, action, mtp_type, path, NULL);
    if (ret < 0) {
        printf("MTP command failed, retrying...\n");
        close(fifo_fd);
        fifo_fd = -1;
        
        // 重新尝试打开FIFO
        fifo_fd = open(MTP_FIFO_NAME, O_WRONLY | O_NONBLOCK);
        if (fifo_fd >= 0) {
            ret = mtp_tools_send_command(fifo_fd, action, mtp_type, path, NULL);
        }
    }
    
    if (ret >= 0) {
        printf("MTP command sent successfully\n");
    } else {
        printf("MTP command failed after retry\n");
    }
}

/* -------------------- 改进的操作跟踪 - 区分拷贝和删除操作 -------------------- */

static void mark_deletion_operation(const char *path) {
    pthread_mutex_lock(&state_mutex);
    
    time_t now = time(NULL);
    
    // 删除操作需要立即处理
    operation_state.state = OP_STATE_DELETION;
    operation_state.operation_start = now;
    operation_state.last_event_time = now;
    operation_state.event_count = 1;
    operation_state.is_deletion = 1;
    operation_state.is_copy_operation = 0;
    
    // 保存被删除的路径
    strncpy(operation_state.deleted_path, path, sizeof(operation_state.deleted_path) - 1);
    operation_state.deleted_path[sizeof(operation_state.deleted_path) - 1] = '\0';
    
    // 获取用于更新的父目录路径
    get_update_path_for_deletion(path, operation_state.path, sizeof(operation_state.path));
    
    pthread_mutex_unlock(&state_mutex);
}

static void mark_copy_operation_start(const char *path) {
    pthread_mutex_lock(&state_mutex);
    
    time_t now = time(NULL);
    
    // 开始拷贝操作
    operation_state.state = OP_STATE_COPY_IN_PROGRESS;
    operation_state.operation_start = now;
    operation_state.last_event_time = now;
    operation_state.event_count = 1;
    operation_state.is_deletion = 0;
    operation_state.is_copy_operation = 1;
    operation_state.create_events = 0;
    operation_state.modify_events = 0;
    operation_state.close_write_events = 0;
    
    get_top_level_change_dir(path, operation_state.path, sizeof(operation_state.path));
    
    printf("Copy operation started for path: %s\n", operation_state.path);
    
    pthread_mutex_unlock(&state_mutex);
}

static void mark_operation_start(const char *path) {
    pthread_mutex_lock(&state_mutex);
    
    time_t now = time(NULL);
    
    if (operation_state.state == OP_STATE_IDLE) {
        // 新操作开始
        operation_state.state = OP_STATE_STARTING;
        operation_state.operation_start = now;
        operation_state.event_count = 1;
        operation_state.pending_events = 0;
        operation_state.is_deletion = 0;
        operation_state.is_copy_operation = 0;
        get_top_level_change_dir(path, operation_state.path, sizeof(operation_state.path));
    } else if (!operation_state.is_copy_operation) {
        // 非拷贝操作进行中，更新状态
        operation_state.state = OP_STATE_IN_PROGRESS;
        operation_state.event_count++;
    }
    
    operation_state.last_event_time = now;
    pthread_mutex_unlock(&state_mutex);
}

static void mark_operation_progress(const char *path) {
    pthread_mutex_lock(&state_mutex);
    
    time_t now = time(NULL);
    
    if (operation_state.state != OP_STATE_IDLE && operation_state.state != OP_STATE_DELETION) {
        if (operation_state.is_copy_operation) {
            operation_state.state = OP_STATE_COPY_IN_PROGRESS;
        } else {
            operation_state.state = OP_STATE_IN_PROGRESS;
        }
        operation_state.event_count++;
        operation_state.last_event_time = now;
    }
    
    pthread_mutex_unlock(&state_mutex);
}

static void mark_copy_event(struct inotify_event *event) {
    pthread_mutex_lock(&state_mutex);
    
    if (operation_state.is_copy_operation) {
        if (event->mask & IN_CREATE) {
            operation_state.create_events++;
        }
        if (event->mask & IN_MODIFY) {
            operation_state.modify_events++;
        }
        if (event->mask & IN_CLOSE_WRITE) {
            operation_state.close_write_events++;
        }
        operation_state.last_event_time = time(NULL);
    }
    
    pthread_mutex_unlock(&state_mutex);
}

static void mark_operation_end(const char *path) {
    pthread_mutex_lock(&state_mutex);
    
    if (operation_state.state != OP_STATE_IDLE && 
        operation_state.state != OP_STATE_DELETION &&
        !operation_state.is_copy_operation) {
        operation_state.state = OP_STATE_ENDING;
        operation_state.pending_events++;
        operation_state.last_event_time = time(NULL);
    }
    
    pthread_mutex_unlock(&state_mutex);
}

static int check_operation_complete(char *update_path, size_t path_size, int *is_deletion) {
    pthread_mutex_lock(&state_mutex);
    
    time_t now = time(NULL);
    int should_send = 0;
    *is_deletion = operation_state.is_deletion;
    
    if (operation_state.state != OP_STATE_IDLE) {
        // 删除操作立即发送
        if (operation_state.state == OP_STATE_DELETION) {
            should_send = 1;
            strncpy(update_path, operation_state.path, path_size - 1);
            update_path[path_size - 1] = '\0';
        }
        // 拷贝操作完成检查
        else if (operation_state.is_copy_operation && is_copy_operation_complete()) {
            should_send = 1;
            strncpy(update_path, operation_state.path, path_size - 1);
            update_path[path_size - 1] = '\0';
            printf("Copy operation completed for path: %s\n", update_path);
        }
        // 检查其他操作是否超时
        else if (!operation_state.is_copy_operation && 
                 (now - operation_state.operation_start) >= MAX_EVENT_WAIT_TIME) {
            operation_state.state = OP_STATE_COMPLETED;
            should_send = 1;
            strncpy(update_path, operation_state.path, path_size - 1);
            update_path[path_size - 1] = '\0';
        }
        // 检查其他操作是否真正结束
        else if (operation_state.state == OP_STATE_ENDING && !operation_state.is_copy_operation) {
            if ((now - operation_state.last_event_time) >= OPERATION_QUIET_TIME) {
                operation_state.state = OP_STATE_COMPLETED;
                should_send = 1;
                strncpy(update_path, operation_state.path, path_size - 1);
                update_path[path_size - 1] = '\0';
            }
        }
        // 检查其他操作是否长时间无活动
        else if (!operation_state.is_copy_operation && 
                 (now - operation_state.last_event_time) >= OPERATION_QUIET_TIME) {
            operation_state.state = OP_STATE_COMPLETED;
            should_send = 1;
            strncpy(update_path, operation_state.path, path_size - 1);
            update_path[path_size - 1] = '\0';
        }
        
        if (should_send) {
            operation_state.state = OP_STATE_IDLE;
            operation_state.is_deletion = 0;
            operation_state.is_copy_operation = 0;
        }
    }
    
    pthread_mutex_unlock(&state_mutex);
    return should_send;
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

// 根据路径前缀删除监控项（用于删除目录时清理子目录监控）
static void remove_watch_entries_by_path_prefix(int inotify_fd, const char *path_prefix) {
    struct watch_entry *prev = NULL, *cur = watch_list;
    size_t prefix_len = strlen(path_prefix);
    
    while (cur) {
        if (strncmp(cur->path, path_prefix, prefix_len) == 0 && 
            (cur->path[prefix_len] == '/' || cur->path[prefix_len] == '\0')) {
            
            inotify_rm_watch(inotify_fd, cur->wd);
            
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

/* -------------------- 改进的事件处理 - 区分拷贝和删除操作 -------------------- */

static void handle_inotify_event(int inotify_fd, struct inotify_event *event) {
    if (event->len == 0) return;

    const char *base = path_for_wd(event->wd);
    if (!base) return;

    char full_path[MAX_PATH_LEN];
    if (snprintf(full_path, sizeof(full_path), "%s/%s", base, event->name) >= (int)sizeof(full_path)) {
        return;
    }

    printf("Event: %s on %s (mask: 0x%x)\n", get_event_type_name(event), full_path, event->mask);

    // 特殊处理真正的删除事件
    if (is_real_deletion_event(event, full_path)) {
        mark_deletion_operation(full_path);
        
        // 如果是目录删除，清理相关的监控项
        if (event->mask & IN_ISDIR) {
            remove_watch_entries_by_path_prefix(inotify_fd, full_path);
        }
        
        // 立即发送删除更新命令
        char update_path[MAX_PATH_LEN];
        get_update_path_for_deletion(full_path, update_path, sizeof(update_path));
        execute_mtp_command("update", "DIR", update_path);
        
        return; // 删除事件处理完毕，直接返回
    }
    
    // 检测拷贝操作开始
    if (is_copy_operation_start(event)) {
        mark_copy_operation_start(full_path);
    }
    
    // 处理拷贝操作中的事件
    pthread_mutex_lock(&state_mutex);
    int is_copy_active = operation_state.is_copy_operation;
    pthread_mutex_unlock(&state_mutex);
    
    if (is_copy_active) {
        mark_copy_event(event);
    } else {
        // 处理其他类型的事件
        if (is_operation_start_event(event)) {
            mark_operation_start(full_path);
        } else if (is_operation_progress_event(event)) {
            mark_operation_progress(full_path);
        } else if (is_operation_end_event(event)) {
            mark_operation_end(full_path);
        }
    }
    
    // 如果是目录创建或移动，需要添加监控
    if ((event->mask & IN_ISDIR) && (event->mask & (IN_CREATE | IN_MOVED_TO))) {
        add_watch_recursive(inotify_fd, full_path);
    }
}

/* -------------------- Inotify Recovery -------------------- */

static int recover_inotify_watches(int inotify_fd) {
    // 清理旧的watch列表
    while (watch_list) {
        inotify_rm_watch(inotify_fd, watch_list->wd);
        struct watch_entry *tmp = watch_list;
        watch_list = watch_list->next;
        free(tmp);
    }
    
    // 重新添加监控
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
        return EXIT_FAILURE;
    }
    if (ensure_fifo(MTP_FIFO_NAME) != 0) {
        return EXIT_FAILURE;
    }

    if (!(argc > 1 && strcmp(argv[1], "--no-daemon") == 0)) {
        daemonize();
    }

    setup_signal_handlers();

    inotify_fd = inotify_init1(IN_NONBLOCK);
    if (inotify_fd < 0) {
        return EXIT_FAILURE;
    }

    if (add_watch_recursive(inotify_fd, WATCH_DIR) != 0) {
        close(inotify_fd);
        return EXIT_FAILURE;
    }

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
                sleep(1);
                if (recover_inotify_watches(inotify_fd) == 0) {
                    inotify_recover_count++;
                    continue;
                }
            }
            break;
        }

        // 检查操作是否完成
        char update_path[MAX_PATH_LEN];
        int is_deletion = 0;
        if (check_operation_complete(update_path, sizeof(update_path), &is_deletion)) {
            if (!is_deletion) {
                // 非删除操作完成，发送更新消息
                execute_mtp_command("update", "DIR", update_path);
            }
            // 删除操作已经在handle_inotify_event中立即处理了
        }
        
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
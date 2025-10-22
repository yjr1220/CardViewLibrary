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
#define EVENT_CHECK_INTERVAL 100  // 100ms检查间隔，非常频繁

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

/* -------------------- 极简状态跟踪 -------------------- */

static time_t last_activity = 0;
static int pending_update = 0;
static pthread_mutex_t simple_mutex = PTHREAD_MUTEX_INITIALIZER;

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

static int ensure_fifo(const char *path) {
    if (access(path, F_OK) == 0) return 0;
    if (mkfifo(path, 0666) != 0) {
        return -1;
    }
    return 0;
}

// 检测是否为临时文件
static int should_ignore_path(const char *path) {
    if (!path) return 1;
    
    if (strstr(path, "/.tmp/") || strstr(path, "\\.tmp\\")) {
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

// 极简MTP命令发送 - 只发送根目录更新
static void send_mtp_update(void) {
    static int fifo_fd = -1;
    
    printf("Sending MTP update to root directory\n");
    
    // 尝试打开FIFO
    if (fifo_fd < 0) {
        fifo_fd = open(MTP_FIFO_NAME, O_WRONLY | O_NONBLOCK);
        if (fifo_fd < 0) {
            printf("Failed to open MTP FIFO\n");
            return;
        }
    }
    
    // 发送UPDATE命令到根目录
    int ret = mtp_tools_send_command(fifo_fd, MTP_TOOLS_FUNCTION_UPDATE, MTP_TOOLS_TYPE_DIR, WATCH_DIR, NULL);
    if (ret >= 0) {
        printf("MTP update sent successfully\n");
    } else {
        printf("MTP update failed, closing FIFO\n");
        close(fifo_fd);
        fifo_fd = -1;
    }
}

/* -------------------- 极简活动跟踪 -------------------- */

static void mark_activity(void) {
    pthread_mutex_lock(&simple_mutex);
    last_activity = time(NULL);
    pending_update = 1;
    printf("Activity detected, marking for update\n");
    pthread_mutex_unlock(&simple_mutex);
}

static void check_and_update(void) {
    pthread_mutex_lock(&simple_mutex);
    
    time_t now = time(NULL);
    
    if (pending_update && (now - last_activity) >= OPERATION_QUIET_TIME) {
        printf("Quiet time reached, sending MTP update\n");
        pthread_mutex_unlock(&simple_mutex);
        
        send_mtp_update();
        
        pthread_mutex_lock(&simple_mutex);
        pending_update = 0;
        printf("Update completed\n");
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

/* -------------------- 极简事件处理 -------------------- */

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

    // 所有有效事件都标记为活动
    if (event->mask & (IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MODIFY | 
                      IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE)) {
        
        // 如果是目录删除，清理监控项
        if ((event->mask & (IN_DELETE | IN_DELETE_SELF)) && (event->mask & IN_ISDIR)) {
            remove_watch_entries_by_path_prefix(inotify_fd, full_path);
        }
        
        // 如果是目录创建，添加监控
        if ((event->mask & IN_CREATE) && (event->mask & IN_ISDIR)) {
            add_watch_recursive(inotify_fd, full_path);
        }
        
        // 标记活动，稍后统一更新
        mark_activity();
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

    printf("MTP daemon started (SIMPLE VERSION), watching %s\n", WATCH_DIR);

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

        // 检查是否需要发送更新
        check_and_update();
        
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
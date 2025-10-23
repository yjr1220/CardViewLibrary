#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdint.h>
#include <limits.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "mtp_daemon.h"

#define EVENT_SIZE (sizeof(struct inotify_event))
#define BUF_LEN (1024 * (EVENT_SIZE + 16))

// Debounce state
static time_t last_operation_time = 0;
static time_t last_sent_time = 0;
static int pending_operations = 0;

// inotify bookkeeping
typedef struct WatchNode {
    int wd;
    char *path;
    struct WatchNode *next;
} WatchNode;

static WatchNode *watch_list_head = NULL;
static int inotify_fd_global = -1;

static void add_watch_node(int wd, const char *path) {
    WatchNode *node = (WatchNode *)calloc(1, sizeof(WatchNode));
    if (!node) return;
    node->wd = wd;
    node->path = strdup(path);
    node->next = watch_list_head;
    watch_list_head = node;
}

static const char *path_for_wd(int wd) {
    for (WatchNode *n = watch_list_head; n; n = n->next) {
        if (n->wd == wd) return n->path;
    }
    return NULL;
}

static void remove_watch_node_by_wd(int wd) {
    WatchNode **pp = &watch_list_head;
    while (*pp) {
        if ((*pp)->wd == wd) {
            WatchNode *del = *pp;
            *pp = del->next;
            free(del->path);
            free(del);
            return;
        }
        pp = &((*pp)->next);
    }
}

static int should_ignore_file(const char *name) {
    if (!name || !*name) return 1;
    if (name[0] == '.') return 1;
    if (strstr(name, ".tmp")) return 1;
    if (strstr(name, ".part")) return 1;
    if (strstr(name, "~")) return 1;
    return 0;
}

static void mark_operation_completed(const char* operation, const char* filename) {
    time_t current_time = time(NULL);
    last_operation_time = current_time;
    pending_operations++;

    struct tm tm_info;
    localtime_r(&current_time, &tm_info);
    printf("[MTPD] %s completed: %s at %02d:%02d:%02d (pending: %d)\n",
           operation, filename ? filename : "(null)",
           tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
           pending_operations);
}

static int send_mtp_update_command() {
    const char *path = WATCH_DIR;
    printf("[MTPD] Preparing to send UPDATE\n");

    int rc_fifo = send_update_via_fifo(path);
    int rc_sock = send_update_via_socket(path);

    if (rc_fifo == 0 || rc_sock == 0) {
        printf("[MTPD] UPDATE sent (fifo:%d socket:%d)\n", rc_fifo, rc_sock);
        return 0;
    }

    printf("[MTPD] UPDATE failed on all channels (fifo:%d socket:%d)\n", rc_fifo, rc_sock);
    return -1;
}

static void check_and_send_update(void) {
    time_t current_time = time(NULL);
    if (pending_operations > 0 &&
        (current_time - last_operation_time >= 2) &&
        (current_time - last_sent_time >= 3)) {

        printf("[MTPD] Sending UPDATE after idle %ld sec for %d ops\n",
               (long)(current_time - last_operation_time), pending_operations);

        if (send_mtp_update_command() == 0) {
            last_sent_time = current_time;
            pending_operations = 0;
            printf("[MTPD] UPDATE command sent successfully\n");
        } else {
            printf("[MTPD] UPDATE send failed\n");
        }
    }
}

static int add_watch_recursive(const char *root_path) {
    if (inotify_fd_global < 0) return -1;
    int wd = inotify_add_watch(inotify_fd_global, root_path,
                               IN_CLOSE_WRITE | IN_MOVED_TO |
                               IN_CREATE | IN_DELETE | IN_MOVED_FROM |
                               IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB);
    if (wd < 0) {
        printf("[MTPD] Failed to add watch: %s: %s\n", root_path, strerror(errno));
        return -1;
    }
    add_watch_node(wd, root_path);

    DIR *dir = opendir(root_path);
    if (!dir) return 0; // file or no permission, ignore

    struct dirent *ent;
    char child[PATH_MAX];
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (ent->d_name[0] == '.') continue; // skip hidden

        snprintf(child, sizeof(child), "%s/%s", root_path, ent->d_name);

        struct stat st;
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            add_watch_recursive(child);
        }
    }
    closedir(dir);
    return 0;
}

static void handle_dir_create(const char *parent_path, const char *name) {
    char newdir[PATH_MAX];
    snprintf(newdir, sizeof(newdir), "%s/%s", parent_path, name);

    struct stat st;
    if (lstat(newdir, &st) == 0 && S_ISDIR(st.st_mode)) {
        add_watch_recursive(newdir);
        mark_operation_completed("Directory create", name);
    }
}

static void handle_inotify_events(int inotify_fd) {
    char buffer[BUF_LEN];
    int length = read(inotify_fd, buffer, sizeof(buffer));

    if (length < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            printf("[MTPD] Failed to read inotify: %s\n", strerror(errno));
        }
        return;
    }

    int i = 0;
    int completed_operations = 0;

    while (i < length) {
        struct inotify_event *event = (struct inotify_event *)&buffer[i];
        const char *base = path_for_wd(event->wd);

        const char *name = (event->len > 0) ? event->name : NULL;
        if (name && should_ignore_file(name)) {
            i += EVENT_SIZE + event->len;
            continue;
        }

        if (event->mask & IN_IGNORED) {
            remove_watch_node_by_wd(event->wd);
        }

        if ((event->mask & IN_CREATE) && (event->mask & IN_ISDIR)) {
            if (base && name) handle_dir_create(base, name);
            completed_operations++;
        } else if ((event->mask & IN_DELETE) && (event->mask & IN_ISDIR)) {
            mark_operation_completed("Directory delete", name ? name : "");
            completed_operations++;
        } else if (event->mask & IN_CLOSE_WRITE) {
            mark_operation_completed("File write", name ? name : "");
            completed_operations++;
        } else if (event->mask & IN_MOVED_TO) {
            mark_operation_completed("File move", name ? name : "");
            completed_operations++;
        } else if (event->mask & IN_DELETE) {
            mark_operation_completed("File delete", name ? name : "");
            completed_operations++;
        } else if (event->mask & (IN_ATTRIB)) {
            // metadata changed; debounce with others
            mark_operation_completed("Metadata change", name ? name : "");
            completed_operations++;
        }

        i += EVENT_SIZE + event->len;
    }

    if (completed_operations > 0) {
        printf("[MTPD] Batch completed operations: %d\n", completed_operations);
    }
}

int send_update_via_fifo(const char *path) {
    int fd = open(MTP_FIFO_NAME, O_WRONLY | O_NONBLOCK, 0);
    if (fd < 0) {
        // silent failure to allow other channel
        return -1;
    }

    size_t pathLen = strlen(path) + 1;
    mtp_msg_header_t header;
    memset(&header, 0, sizeof(header));
    header.magic = 0x4D545031; // 'MTP1'
    header.version = 1;
    header.headerSize = (uint16_t)sizeof(header);
    header.action = MTP_ACTION_UPDATE;
    header.type = 1; // dir
    header.srcPathLen = (uint32_t)pathLen;
    header.destPathLen = 0;
    header.payloadLen = 0;

    struct iovec {
        void *iov_base;
        size_t iov_len;
    } vec[2];

    vec[0].iov_base = &header;
    vec[0].iov_len  = sizeof(header);
    vec[1].iov_base = (void *)path;
    vec[1].iov_len  = pathLen;

    ssize_t wrote = write(fd, &header, sizeof(header));
    if (wrote != (ssize_t)sizeof(header)) {
        close(fd);
        return -1;
    }
    wrote = write(fd, path, pathLen);
    if (wrote != (ssize_t)pathLen) {
        close(fd);
        return -1;
    }

    fsync(fd);
    close(fd);
    return 0;
}

int send_update_via_socket(const char *path) {
    int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, MTP_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }

    size_t pathLen = strlen(path) + 1;
    mtp_msg_header_t header;
    memset(&header, 0, sizeof(header));
    header.magic = 0x4D545031; // 'MTP1'
    header.version = 1;
    header.headerSize = (uint16_t)sizeof(header);
    header.action = MTP_ACTION_UPDATE;
    header.type = 1; // dir
    header.srcPathLen = (uint32_t)pathLen;
    header.destPathLen = 0;
    header.payloadLen = 0;

    ssize_t wrote = send(sock, &header, sizeof(header), MSG_NOSIGNAL);
    if (wrote != (ssize_t)sizeof(header)) {
        close(sock);
        return -1;
    }
    wrote = send(sock, path, pathLen, MSG_NOSIGNAL);
    if (wrote != (ssize_t)pathLen) {
        close(sock);
        return -1;
    }

    shutdown(sock, SHUT_WR);
    close(sock);
    return 0;
}

static int check_mtp_fifo(void) {
    struct stat st;
    if (stat(MTP_FIFO_NAME, &st) == 0) {
        printf("[MTPD] MTP FIFO found: %s\n", MTP_FIFO_NAME);
        return 1;
    }
    printf("[MTPD] MTP FIFO not found: %s\n", MTP_FIFO_NAME);
    return 0;
}

static int check_mtp_socket(void) {
    struct stat st;
    if (stat(MTP_SOCKET_PATH, &st) == 0) {
        printf("[MTPD] MTP socket found: %s\n", MTP_SOCKET_PATH);
        return 1;
    }
    printf("[MTPD] MTP socket not found: %s\n", MTP_SOCKET_PATH);
    return 0;
}

static int check_watch_dir(void) {
    struct stat st;
    if (stat(WATCH_DIR, &st) == 0 && S_ISDIR(st.st_mode)) {
        printf("[MTPD] Watch directory found: %s\n", WATCH_DIR);
        return 1;
    }
    printf("[MTPD] Watch directory missing or not a directory: %s\n", WATCH_DIR);
    return 0;
}

int main(void) {
    printf("[MTPD] Starting MTP inotify daemon (recursive + debounce)\n");
    printf("[MTPD] Target directory: %s\n", WATCH_DIR);
    printf("[MTPD] FIFO: %s\n", MTP_FIFO_NAME);
    printf("[MTPD] Socket: %s\n", MTP_SOCKET_PATH);

    if (!check_watch_dir()) {
        printf("[MTPD] Cannot proceed without watch directory\n");
        return 1;
    }

    check_mtp_fifo();
    check_mtp_socket();

    inotify_fd_global = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd_global < 0) {
        printf("[MTPD] inotify_init1 failed: %s\n", strerror(errno));
        return 1;
    }

    if (add_watch_recursive(WATCH_DIR) != 0) {
        printf("[MTPD] Initial recursive watch failed\n");
    }

    printf("[MTPD] Inotify daemon started. Monitoring writes, moves, creates, deletes.\n");
    printf("[MTPD] Debounce: 2s idle; Send min interval: 3s\n\n");

    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(inotify_fd_global, &readfds);

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 500000; // 500ms

        int ret = select(inotify_fd_global + 1, &readfds, NULL, NULL, &timeout);
        if (ret > 0 && FD_ISSET(inotify_fd_global, &readfds)) {
            handle_inotify_events(inotify_fd_global);
        }

        check_and_send_update();

        if (ret < 0 && errno != EINTR) {
            printf("[MTPD] select error: %s\n", strerror(errno));
            break;
        }
    }

    printf("[MTPD] Shutting down daemon\n");
    close(inotify_fd_global);
    return 0;
}

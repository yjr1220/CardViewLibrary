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

#define WATCH_DIR "/mnt/extsd"
#define MTP_FIFO_NAME "/tmp/.mtp_fifo"
#define EVENT_SIZE (sizeof(struct inotify_event))
#define BUF_LEN (1024 * (EVENT_SIZE + 16))

// 🎯 简单的防抖动机制
static time_t last_activity_time = 0;
static time_t last_sent_time = 0;

typedef struct {
    uint32_t action;
    uint32_t type;
    uint32_t srcPathLen;
    uint32_t destPathLen;
    char *path;
} mtp_command_t;

enum {
    MTP_TOOLS_FUNCTION_UPDATE = 2,
};

// 发送MTP命令（完全模仿MtpTools的实现）
int send_mtp_update_command() {
    int fd;
    mtp_command_t *command;
    const char *path = WATCH_DIR;
    size_t pathLen = strlen(path) + 1;
    size_t command_size = sizeof(mtp_command_t) + pathLen;
    int ret;

    // 打开FIFO（与MtpTools完全相同的方式）
    fd = open(MTP_FIFO_NAME, O_WRONLY | O_NONBLOCK, 0);
    if (fd < 0) {
        printf("❌ Failed to open MTP FIFO: %s\n", strerror(errno));
        return -1;
    }

    // 分配命令结构体（与MtpTools完全相同）
    command = calloc(1, command_size);
    if (!command) {
        close(fd);
        return -1;
    }

    // 填充命令结构体（与MtpTools完全相同）
    command->action = MTP_TOOLS_FUNCTION_UPDATE;
    command->type = 2; // DIR type (与MtpTools中的MTP_TOOLS_TYPE_DIR相同)
    command->srcPathLen = pathLen;
    command->destPathLen = 0;
    command->path = (char *)&command[1];
    strcpy(command->path, path);

    // 发送命令（与MtpTools完全相同）
    ret = write(fd, command, command_size);
    if (ret != command_size) {
        printf("❌ Failed to write MTP command: %d/%zu bytes written\n", ret, command_size);
        free(command);
        close(fd);
        return -1;
    }

    printf("✅ Sent MTP UPDATE command for %s (like MtpTools)\n", path);
    free(command);
    close(fd);
    return 0;
}

// 标记有文件活动
void mark_activity() {
    time_t current_time = time(NULL);
    last_activity_time = current_time;
    
    struct tm *tm_info = localtime(&current_time);
    printf("📁 File activity detected at %02d:%02d:%02d\n", 
           tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
}

// 检查是否需要发送更新命令
void check_and_send_update() {
    time_t current_time = time(NULL);
    
    // 如果有活动，且活动已经停止3秒，且距离上次发送超过5秒
    if (last_activity_time > 0 && 
        (current_time - last_activity_time >= 3) &&
        (current_time - last_sent_time >= 5)) {
        
        printf("🚀 Sending UPDATE command (activity stopped %ld seconds ago)\n", 
               current_time - last_activity_time);
        
        if (send_mtp_update_command() == 0) {
            last_sent_time = current_time;
            last_activity_time = 0;  // 重置活动时间
            printf("✅ UPDATE command sent successfully\n");
        } else {
            printf("❌ Failed to send UPDATE command\n");
        }
    }
}

// 简单的文件过滤（忽略临时文件）
int should_ignore_file(const char *name) {
    if (!name) return 1;
    
    // 忽略临时文件和隐藏文件
    if (name[0] == '.') return 1;
    if (strstr(name, ".tmp")) return 1;
    if (strstr(name, ".part")) return 1;
    if (strstr(name, "~")) return 1;
    
    return 0;
}

// 处理inotify事件
void handle_inotify_events(int inotify_fd) {
    char buffer[BUF_LEN];
    int length = read(inotify_fd, buffer, BUF_LEN);
    
    if (length < 0) {
        printf("❌ Failed to read inotify events: %s\n", strerror(errno));
        return;
    }
    
    int i = 0;
    int event_count = 0;
    
    while (i < length) {
        struct inotify_event *event = (struct inotify_event *)&buffer[i];
        
        if (event->len > 0) {
            if (!should_ignore_file(event->name)) {
                printf("📂 Event: %s (mask: 0x%x)\n", event->name, event->mask);
                mark_activity();
                event_count++;
            } else {
                printf("🚫 Ignored: %s (temporary file)\n", event->name);
            }
        }
        
        i += EVENT_SIZE + event->len;
    }
    
    if (event_count > 0) {
        printf("📊 Processed %d valid events\n", event_count);
    }
}

// 检查MTP FIFO是否存在
int check_mtp_fifo() {
    struct stat st;
    if (stat(MTP_FIFO_NAME, &st) == 0) {
        printf("✅ MTP FIFO found: %s\n", MTP_FIFO_NAME);
        return 1;
    } else {
        printf("❌ MTP FIFO not found: %s\n", MTP_FIFO_NAME);
        return 0;
    }
}

// 检查监控目录是否存在
int check_watch_dir() {
    struct stat st;
    if (stat(WATCH_DIR, &st) == 0 && S_ISDIR(st.st_mode)) {
        printf("✅ Watch directory found: %s\n", WATCH_DIR);
        return 1;
    } else {
        printf("❌ Watch directory not found or not a directory: %s\n", WATCH_DIR);
        return 0;
    }
}

int main() {
    int inotify_fd, watch_fd;
    fd_set readfds;
    struct timeval timeout;
    
    printf("🚀 Starting optimized MTP inotify daemon\n");
    printf("📁 Target directory: %s\n", WATCH_DIR);
    printf("📡 MTP FIFO: %s\n", MTP_FIFO_NAME);
    
    // 检查前置条件
    if (!check_watch_dir()) {
        printf("❌ Cannot proceed without watch directory\n");
        return 1;
    }
    
    if (!check_mtp_fifo()) {
        printf("⚠️  MTP FIFO not found, but continuing (MtpDaemon may not be running)\n");
    }
    
    // 初始化inotify
    inotify_fd = inotify_init();
    if (inotify_fd < 0) {
        printf("❌ Failed to initialize inotify: %s\n", strerror(errno));
        return 1;
    }
    
    // 添加监控目录（递归监控所有事件）
    watch_fd = inotify_add_watch(inotify_fd, WATCH_DIR, 
                                IN_CREATE | IN_DELETE | IN_MOVE | IN_MODIFY | 
                                IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO);
    if (watch_fd < 0) {
        printf("❌ Failed to add watch for %s: %s\n", WATCH_DIR, strerror(errno));
        close(inotify_fd);
        return 1;
    }
    
    printf("✅ Inotify daemon started successfully\n");
    printf("⏰ Activity timeout: 3 seconds\n");
    printf("🔄 Send interval: 5 seconds minimum\n");
    printf("🎯 Ready to monitor file changes...\n\n");
    
    // 主循环
    while (1) {
        FD_ZERO(&readfds);
        FD_SET(inotify_fd, &readfds);
        
        // 500ms超时，用于定期检查
        timeout.tv_sec = 0;
        timeout.tv_usec = 500000;
        
        int ret = select(inotify_fd + 1, &readfds, NULL, NULL, &timeout);
        
        if (ret > 0 && FD_ISSET(inotify_fd, &readfds)) {
            handle_inotify_events(inotify_fd);
        } else if (ret == 0) {
            // 超时，检查是否需要发送更新命令
            check_and_send_update();
        } else if (ret < 0 && errno != EINTR) {
            printf("❌ Select error: %s\n", strerror(errno));
            break;
        }
    }
    
    printf("🛑 Shutting down daemon\n");
    close(inotify_fd);
    return 0;
}
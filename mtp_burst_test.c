#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>

#define MTP_FIFO_NAME "/tmp/.mtp_fifo"

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

static int send_mtp_update(const char *path) {
    int fd = open(MTP_FIFO_NAME, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("Failed to open FIFO");
        return -1;
    }
    
    size_t pathLen = strlen(path) + 1;
    size_t command_size = sizeof(mtp_command_t) + pathLen;
    
    mtp_command_t *command = calloc(1, command_size);
    if (!command) {
        close(fd);
        return -1;
    }
    
    command->action = MTP_TOOLS_FUNCTION_UPDATE;
    command->type = MTP_TOOLS_TYPE_DIR;
    command->srcPathLen = pathLen;
    command->destPathLen = 0;
    command->path = (char *)&command[1];
    strcpy(command->path, path);
    
    printf("Sending UPDATE command for: %s\n", path);
    int ret = write(fd, command, command_size);
    
    free(command);
    close(fd);
    
    return (ret > 0) ? 0 : -1;
}

int main() {
    printf("=== MTP Burst Test - Multiple UPDATE Commands ===\n");
    printf("This will send multiple UPDATE commands rapidly to trigger epoll_wait\n");
    
    const char *paths[] = {
        "/mnt/extsd",
        "/mnt/extsd",
        "/mnt/extsd",
        "/mnt/extsd",
        "/mnt/extsd"
    };
    
    int count = sizeof(paths) / sizeof(paths[0]);
    
    printf("Sending %d UPDATE commands rapidly...\n", count);
    
    for (int i = 0; i < count; i++) {
        if (send_mtp_update(paths[i]) == 0) {
            printf("Command %d sent successfully\n", i + 1);
        } else {
            printf("Command %d failed\n", i + 1);
        }
        usleep(10000); // 10ms间隔
    }
    
    printf("All commands sent. Check PC side for immediate response.\n");
    return 0;
}
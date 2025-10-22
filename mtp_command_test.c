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

static int mtp_tools_send_command(int fd, uint32_t action, uint32_t type, const char *spath, const char *dpath)
{
    printf("=== Sending MTP Command ===\n");
    printf("Action: %u (%s)\n", action, 
        action == MTP_TOOLS_FUNCTION_ADD ? "ADD" :
        action == MTP_TOOLS_FUNCTION_REMOVE ? "REMOVE" :
        action == MTP_TOOLS_FUNCTION_UPDATE ? "UPDATE" : "UNKNOWN");
    printf("Type: %u (%s)\n", type, 
        type == MTP_TOOLS_TYPE_FILE ? "FILE" : 
        type == MTP_TOOLS_TYPE_DIR ? "DIR" : "UNKNOWN");
    printf("Source Path: %s\n", spath ? spath : "NULL");
    printf("Dest Path: %s\n", dpath ? dpath : "NULL");
    
    int ret;
    mtp_command_t *command;
    size_t spathLen = 0, dpathLen = 0, pathLen = 0;
    size_t command_size = 0;

    if (spath != NULL) {
        spathLen = strlen(spath) + 1;
        dpathLen = (dpath != NULL) ? (strlen(dpath) + 1) : 0;
        pathLen = spathLen + dpathLen;
    } else if (action != MTP_TOOLS_FUNCTION_CONNECT) {
        printf("ERROR: No source path provided\n");
        return -1;
    }

    command_size = sizeof(mtp_command_t) + pathLen;
    printf("Command size: %zu bytes\n", command_size);

    command = calloc(1, command_size);
    if (!command) {
        printf("ERROR: Memory allocation failed\n");
        return -1;
    }
    
    command->action = action;
    command->type = type;
    command->srcPathLen = spathLen;
    command->destPathLen = dpathLen;
    
    printf("Command structure:\n");
    printf("  action = %u\n", command->action);
    printf("  type = %u\n", command->type);
    printf("  srcPathLen = %u\n", command->srcPathLen);
    printf("  destPathLen = %u\n", command->destPathLen);
    
    if (spathLen > 1) {
        command->path = (char *)&command[1];
        strcpy(command->path, spath);
        printf("  path = %s\n", command->path);
    } else if (action != MTP_TOOLS_FUNCTION_CONNECT) {
        printf("ERROR: Invalid path length\n");
        free(command);
        return -1;
    }
    
    if (dpathLen > 1) {
        strcpy(command->path + spathLen, dpath);
        printf("  dest path = %s\n", dpath);
    }

    // 打印原始字节数据
    printf("Raw command bytes: ");
    unsigned char *bytes = (unsigned char *)command;
    for (size_t i = 0; i < command_size; i++) {
        printf("%02x ", bytes[i]);
    }
    printf("\n");

    printf("Writing %zu bytes to FIFO...\n", command_size);
    ret = write(fd, command, command_size);
    printf("Write result: %d bytes\n", ret);
    
    if (ret < 0) {
        perror("Write failed");
    } else if (ret != (int)command_size) {
        printf("WARNING: Partial write - expected %zu, wrote %d\n", command_size, ret);
    } else {
        printf("SUCCESS: Command sent successfully\n");
    }
    
    free(command);
    printf("=== Command Send Complete ===\n");
    
    return (ret == (int)command_size) ? 0 : -1;
}

int main(int argc, char *argv[]) {
    printf("=== MTP Command Test Tool ===\n");
    
    if (argc < 2) {
        printf("Usage: %s <command> [path]\n", argv[0]);
        printf("Commands:\n");
        printf("  add <path>     - Add file/directory\n");
        printf("  remove <path>  - Remove file/directory\n");
        printf("  update <path>  - Update directory\n");
        printf("  connect        - Connect command\n");
        return 1;
    }
    
    const char *cmd = argv[1];
    const char *path = argc > 2 ? argv[2] : "/mnt/extsd";
    
    uint32_t action;
    if (strcmp(cmd, "add") == 0) {
        action = MTP_TOOLS_FUNCTION_ADD;
    } else if (strcmp(cmd, "remove") == 0) {
        action = MTP_TOOLS_FUNCTION_REMOVE;
    } else if (strcmp(cmd, "update") == 0) {
        action = MTP_TOOLS_FUNCTION_UPDATE;
    } else if (strcmp(cmd, "connect") == 0) {
        action = MTP_TOOLS_FUNCTION_CONNECT;
        path = NULL;
    } else {
        printf("ERROR: Unknown command: %s\n", cmd);
        return 1;
    }
    
    printf("Opening FIFO: %s\n", MTP_FIFO_NAME);
    int fd = open(MTP_FIFO_NAME, O_WRONLY);
    if (fd < 0) {
        perror("Failed to open FIFO");
        return 1;
    }
    printf("FIFO opened successfully, fd = %d\n", fd);
    
    // 发送命令
    int result = mtp_tools_send_command(fd, action, MTP_TOOLS_TYPE_DIR, path, NULL);
    
    close(fd);
    
    printf("Test completed with result: %d\n", result);
    return result;
}
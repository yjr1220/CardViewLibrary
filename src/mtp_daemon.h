#pragma once

#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// Communication endpoints
#define WATCH_DIR "/mnt/extsd"
#define MTP_FIFO_NAME "/tmp/.mtp_fifo"
#define MTP_SOCKET_PATH "/tmp/.mtp_socket"

// Wire protocol
// Fixed-size header to precede any payload
// All integer fields are in host byte order for simplicity on embedded target
// If cross-endian communication is needed, update to use htole32/htobe32.

typedef struct {
    uint32_t magic;        // 'MTP1'
    uint16_t version;      // protocol version
    uint16_t headerSize;   // sizeof(mtp_msg_header_t)
    uint32_t action;       // command code
    uint32_t type;         // object type (0 file, 1 dir, etc.)
    uint32_t srcPathLen;   // bytes incl. NUL when present
    uint32_t destPathLen;  // bytes incl. NUL when present
    uint32_t payloadLen;   // optional extra payload len (bytes)
} mtp_msg_header_t;

enum {
    MTP_ACTION_INVALID = 0,
    MTP_ACTION_UPDATE = 2,
};

// Helpers
int send_update_via_fifo(const char *path);
int send_update_via_socket(const char *path);

#ifdef __cplusplus
}
#endif

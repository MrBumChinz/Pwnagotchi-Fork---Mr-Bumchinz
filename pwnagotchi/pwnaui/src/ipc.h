/*
 * PwnaUI - IPC Header
 * UNIX domain socket server for Python ↔ C communication
 */

#ifndef PWNAUI_IPC_H
#define PWNAUI_IPC_H

#include <sys/types.h>

/* IPC Command types */
typedef enum {
    IPC_CMD_UNKNOWN = 0,
    IPC_CMD_PING,
    IPC_CMD_QUIT,
    IPC_CMD_UPDATE,
    IPC_CMD_CLEAR,
    IPC_CMD_SET_FACE,
    IPC_CMD_SET_STATUS,
    IPC_CMD_SET_CHANNEL,
    IPC_CMD_SET_APS,
    IPC_CMD_SET_UPTIME,
    IPC_CMD_SET_SHAKES,
    IPC_CMD_SET_MODE,
    IPC_CMD_SET_NAME,
    IPC_CMD_SET_FRIEND,
    IPC_CMD_SET_LAYOUT,
    IPC_CMD_SET_INVERT,
    IPC_CMD_SET_THEME,
    IPC_CMD_LIST_THEMES,
    IPC_CMD_GET_THEME,
    /* Phase 3: GPS commands */
    IPC_CMD_SET_GPS,
    IPC_CMD_SET_GPS_STATUS,
    IPC_CMD_GET_GPS,
} ipc_cmd_t;

/*
 * Create and bind a UNIX domain socket server
 * Returns socket fd on success, -1 on error
 */
int ipc_server_create(const char *socket_path);

/*
 * Accept a new client connection
 * Returns client fd on success, -1 on error
 */
int ipc_server_accept(int server_fd);

/*
 * Destroy the server socket and cleanup
 */
void ipc_server_destroy(int server_fd, const char *socket_path);

/*
 * Read a line from client (blocking, with timeout)
 * Returns number of bytes read, 0 on disconnect, -1 on error
 */
ssize_t ipc_read_line(int client_fd, char *buffer, size_t buf_size, int timeout_ms);

/*
 * Write response to client
 * Returns number of bytes written, -1 on error
 */
ssize_t ipc_write(int client_fd, const char *data, size_t len);

/*
 * Parse a command string and extract argument
 * Returns command type, sets arg to point to argument (within cmd buffer)
 */
ipc_cmd_t ipc_parse_command(char *cmd, char **arg);

#endif /* PWNAUI_IPC_H */

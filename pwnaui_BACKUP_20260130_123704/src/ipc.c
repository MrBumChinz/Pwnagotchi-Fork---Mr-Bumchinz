/*
 * PwnaUI - IPC Implementation
 * UNIX domain socket server for Python ↔ C communication
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/select.h>

#include "ipc.h"

#define SOCKET_BACKLOG 128  /* Handle high burst - web UI + pwnagotchi + manual tests */

/*
 * Create and bind a UNIX domain socket server
 */
int ipc_server_create(const char *socket_path) {
    int fd;
    struct sockaddr_un addr;
    
    /* Remove any existing socket file */
    unlink(socket_path);
    
    /* Create socket */
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    
    /* Set non-blocking */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    
    /* Setup address */
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    
    /* Bind */
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    
    /* Set socket permissions (accessible by pwnagotchi user) */
    chmod(socket_path, 0666);
    
    /* Listen */
    if (listen(fd, SOCKET_BACKLOG) < 0) {
        perror("listen");
        close(fd);
        unlink(socket_path);
        return -1;
    }
    
    return fd;
}

/*
 * Accept a new client connection
 */
int ipc_server_accept(int server_fd) {
    int client_fd;
    struct sockaddr_un client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -1;  /* No pending connections */
        }
        perror("accept");
        return -1;
    }
    
    /* Set client socket to non-blocking */
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
    
    return client_fd;
}

/*
 * Destroy the server socket and cleanup
 */
void ipc_server_destroy(int server_fd, const char *socket_path) {
    if (server_fd >= 0) {
        close(server_fd);
    }
    if (socket_path) {
        unlink(socket_path);
    }
}

/*
 * Read a line from client (blocking with timeout)
 */
ssize_t ipc_read_line(int client_fd, char *buffer, size_t buf_size, int timeout_ms) {
    fd_set read_fds;
    struct timeval timeout;
    ssize_t total = 0;
    ssize_t n;
    
    if (timeout_ms > 0) {
        FD_ZERO(&read_fds);
        FD_SET(client_fd, &read_fds);
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;
        
        int ready = select(client_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (ready <= 0) {
            return ready;  /* Timeout or error */
        }
    }
    
    /* Read data */
    while (total < (ssize_t)(buf_size - 1)) {
        n = read(client_fd, buffer + total, 1);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  /* No more data available */
            }
            return -1;  /* Error */
        }
        if (n == 0) {
            if (total == 0) return 0;  /* Client disconnected */
            break;
        }
        
        total += n;
        
        /* Check for newline */
        if (buffer[total - 1] == '\n') {
            break;
        }
    }
    
    buffer[total] = '\0';
    return total;
}

/*
 * Write response to client
 */
ssize_t ipc_write(int client_fd, const char *data, size_t len) {
    ssize_t written = 0;
    ssize_t n;
    
    while (written < (ssize_t)len) {
        n = write(client_fd, data + written, len - written);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Would block, try again */
                usleep(1000);
                continue;
            }
            return -1;  /* Error */
        }
        written += n;
    }
    
    return written;
}

/*
 * Parse a command string and extract argument
 */
ipc_cmd_t ipc_parse_command(char *cmd, char **arg) {
    if (!cmd) return IPC_CMD_UNKNOWN;
    
    /* Remove trailing newline */
    size_t len = strlen(cmd);
    if (len > 0 && cmd[len-1] == '\n') {
        cmd[len-1] = '\0';
        len--;
    }
    
    if (len == 0) return IPC_CMD_UNKNOWN;
    
    /* Find space separating command from argument */
    char *space = strchr(cmd, ' ');
    if (space) {
        *space = '\0';
        if (arg) *arg = space + 1;
    } else {
        if (arg) *arg = NULL;
    }
    
    /* Match command */
    if (strcmp(cmd, "PING") == 0) return IPC_CMD_PING;
    if (strcmp(cmd, "QUIT") == 0) return IPC_CMD_QUIT;
    if (strcmp(cmd, "UPDATE") == 0) return IPC_CMD_UPDATE;
    if (strcmp(cmd, "CLEAR") == 0) return IPC_CMD_CLEAR;
    if (strcmp(cmd, "SET_FACE") == 0) return IPC_CMD_SET_FACE;
    if (strcmp(cmd, "SET_STATUS") == 0) return IPC_CMD_SET_STATUS;
    if (strcmp(cmd, "SET_CHANNEL") == 0) return IPC_CMD_SET_CHANNEL;
    if (strcmp(cmd, "SET_APS") == 0) return IPC_CMD_SET_APS;
    if (strcmp(cmd, "SET_UPTIME") == 0) return IPC_CMD_SET_UPTIME;
    if (strcmp(cmd, "SET_SHAKES") == 0) return IPC_CMD_SET_SHAKES;
    if (strcmp(cmd, "SET_MODE") == 0) return IPC_CMD_SET_MODE;
    if (strcmp(cmd, "SET_NAME") == 0) return IPC_CMD_SET_NAME;
    if (strcmp(cmd, "SET_FRIEND") == 0) return IPC_CMD_SET_FRIEND;
    if (strcmp(cmd, "SET_LAYOUT") == 0) return IPC_CMD_SET_LAYOUT;
    if (strcmp(cmd, "SET_INVERT") == 0) return IPC_CMD_SET_INVERT;
    if (strcmp(cmd, "SET_THEME") == 0) return IPC_CMD_SET_THEME;
    if (strcmp(cmd, "LIST_THEMES") == 0) return IPC_CMD_LIST_THEMES;
    if (strcmp(cmd, "GET_THEME") == 0) return IPC_CMD_GET_THEME;
    /* Phase 3: GPS commands */
    if (strcmp(cmd, "SET_GPS") == 0) return IPC_CMD_SET_GPS;
    if (strcmp(cmd, "SET_GPS_STATUS") == 0) return IPC_CMD_SET_GPS_STATUS;
    if (strcmp(cmd, "GET_GPS") == 0) return IPC_CMD_GET_GPS;
    /* Phase 4: PwnHub Stats commands */
    if (strcmp(cmd, "SET_PWNHUB_MACROS") == 0) return IPC_CMD_SET_PWNHUB_MACROS;
    if (strcmp(cmd, "SET_PWNHUB_XP") == 0) return IPC_CMD_SET_PWNHUB_XP;
    if (strcmp(cmd, "SET_PWNHUB_STAGE") == 0) return IPC_CMD_SET_PWNHUB_STAGE;
    if (strcmp(cmd, "SET_PWNHUB_ENABLED") == 0) return IPC_CMD_SET_PWNHUB_ENABLED;
    
    return IPC_CMD_UNKNOWN;
}

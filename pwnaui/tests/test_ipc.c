/*
 * PwnaUI IPC Module Tests
 * Tests for UNIX domain socket server and client communication
 * 
 * Note: These tests require Linux socket functionality.
 * On Windows, they will be skipped with appropriate messages.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef __linux__
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#endif

#include "test_framework.h"
#include "../src/ipc.h"

/* Test socket path */
#define TEST_SOCKET_PATH "/tmp/pwnaui_test.sock"

#ifdef __linux__
/* Helper to create a client connection */
static int create_test_client(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    
    return fd;
}
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * Server Creation Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(ipc_server_create_success) {
#ifdef __linux__
    unlink(TEST_SOCKET_PATH);
    int server_fd = ipc_server_create(TEST_SOCKET_PATH);
    ASSERT_TRUE(server_fd >= 0);
    ipc_server_destroy(server_fd, TEST_SOCKET_PATH);
#else
    printf("    (Skipped on non-Linux)\n");
    ASSERT_TRUE(1);
#endif
}

TEST(ipc_server_create_removes_existing_socket) {
#ifdef __linux__
    /* Create a file at the socket path */
    FILE *f = fopen(TEST_SOCKET_PATH, "w");
    if (f) {
        fprintf(f, "test");
        fclose(f);
    }
    
    int server_fd = ipc_server_create(TEST_SOCKET_PATH);
    ASSERT_TRUE(server_fd >= 0);
    ipc_server_destroy(server_fd, TEST_SOCKET_PATH);
#else
    ASSERT_TRUE(1);
#endif
}

TEST(ipc_server_destroy_removes_socket) {
#ifdef __linux__
    unlink(TEST_SOCKET_PATH);
    int server_fd = ipc_server_create(TEST_SOCKET_PATH);
    ASSERT_TRUE(server_fd >= 0);
    
    ipc_server_destroy(server_fd, TEST_SOCKET_PATH);
    
    /* Socket file should be removed */
    int exists = access(TEST_SOCKET_PATH, F_OK) == 0;
    ASSERT_FALSE(exists);
#else
    ASSERT_TRUE(1);
#endif
}

TEST(ipc_server_destroy_null_path_safe) {
#ifdef __linux__
    unlink(TEST_SOCKET_PATH);
    int server_fd = ipc_server_create(TEST_SOCKET_PATH);
    ASSERT_TRUE(server_fd >= 0);
    
    /* Should not crash with NULL path */
    ipc_server_destroy(server_fd, NULL);
    unlink(TEST_SOCKET_PATH);
    ASSERT_TRUE(1);
#else
    ASSERT_TRUE(1);
#endif
}

TEST(ipc_server_destroy_invalid_fd_safe) {
#ifdef __linux__
    /* Should not crash with invalid fd */
    ipc_server_destroy(-1, TEST_SOCKET_PATH);
    ASSERT_TRUE(1);
#else
    ASSERT_TRUE(1);
#endif
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Client Connection Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(ipc_server_accept_returns_client_fd) {
#ifdef __linux__
    unlink(TEST_SOCKET_PATH);
    int server_fd = ipc_server_create(TEST_SOCKET_PATH);
    ASSERT_TRUE(server_fd >= 0);
    
    /* Create client connection */
    int client_fd = create_test_client(TEST_SOCKET_PATH);
    ASSERT_TRUE(client_fd >= 0);
    
    /* Accept should return a valid fd */
    int accepted_fd = ipc_server_accept(server_fd);
    ASSERT_TRUE(accepted_fd >= 0);
    
    close(client_fd);
    close(accepted_fd);
    ipc_server_destroy(server_fd, TEST_SOCKET_PATH);
#else
    ASSERT_TRUE(1);
#endif
}

TEST(ipc_server_accept_no_pending_returns_minus_one) {
#ifdef __linux__
    unlink(TEST_SOCKET_PATH);
    int server_fd = ipc_server_create(TEST_SOCKET_PATH);
    ASSERT_TRUE(server_fd >= 0);
    
    /* Accept with no pending connections */
    int accepted_fd = ipc_server_accept(server_fd);
    ASSERT_EQUAL(-1, accepted_fd);
    
    ipc_server_destroy(server_fd, TEST_SOCKET_PATH);
#else
    ASSERT_TRUE(1);
#endif
}

TEST(ipc_server_accept_multiple_clients) {
#ifdef __linux__
    unlink(TEST_SOCKET_PATH);
    int server_fd = ipc_server_create(TEST_SOCKET_PATH);
    ASSERT_TRUE(server_fd >= 0);
    
    int clients[3];
    int accepted[3];
    
    for (int i = 0; i < 3; i++) {
        clients[i] = create_test_client(TEST_SOCKET_PATH);
        ASSERT_TRUE(clients[i] >= 0);
        
        accepted[i] = ipc_server_accept(server_fd);
        ASSERT_TRUE(accepted[i] >= 0);
    }
    
    for (int i = 0; i < 3; i++) {
        close(clients[i]);
        close(accepted[i]);
    }
    
    ipc_server_destroy(server_fd, TEST_SOCKET_PATH);
#else
    ASSERT_TRUE(1);
#endif
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Read Line Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(ipc_read_line_simple) {
#ifdef __linux__
    unlink(TEST_SOCKET_PATH);
    int server_fd = ipc_server_create(TEST_SOCKET_PATH);
    ASSERT_TRUE(server_fd >= 0);
    
    int client_fd = create_test_client(TEST_SOCKET_PATH);
    ASSERT_TRUE(client_fd >= 0);
    
    int accepted_fd = ipc_server_accept(server_fd);
    ASSERT_TRUE(accepted_fd >= 0);
    
    /* Client sends data */
    const char *msg = "TEST_COMMAND\n";
    write(client_fd, msg, strlen(msg));
    
    /* Server reads line */
    char buffer[256];
    ssize_t n = ipc_read_line(accepted_fd, buffer, sizeof(buffer), 1000);
    ASSERT_TRUE(n > 0);
    ASSERT_STR_EQUAL("TEST_COMMAND\n", buffer);
    
    close(client_fd);
    close(accepted_fd);
    ipc_server_destroy(server_fd, TEST_SOCKET_PATH);
#else
    ASSERT_TRUE(1);
#endif
}

TEST(ipc_read_line_with_data) {
#ifdef __linux__
    unlink(TEST_SOCKET_PATH);
    int server_fd = ipc_server_create(TEST_SOCKET_PATH);
    ASSERT_TRUE(server_fd >= 0);
    
    int client_fd = create_test_client(TEST_SOCKET_PATH);
    ASSERT_TRUE(client_fd >= 0);
    
    int accepted_fd = ipc_server_accept(server_fd);
    ASSERT_TRUE(accepted_fd >= 0);
    
    /* Client sends command with data */
    const char *msg = "SET_FACE (◕‿‿◕)\n";
    write(client_fd, msg, strlen(msg));
    
    char buffer[256];
    ssize_t n = ipc_read_line(accepted_fd, buffer, sizeof(buffer), 1000);
    ASSERT_TRUE(n > 0);
    
    close(client_fd);
    close(accepted_fd);
    ipc_server_destroy(server_fd, TEST_SOCKET_PATH);
#else
    ASSERT_TRUE(1);
#endif
}

TEST(ipc_read_line_long_line) {
#ifdef __linux__
    unlink(TEST_SOCKET_PATH);
    int server_fd = ipc_server_create(TEST_SOCKET_PATH);
    ASSERT_TRUE(server_fd >= 0);
    
    int client_fd = create_test_client(TEST_SOCKET_PATH);
    ASSERT_TRUE(client_fd >= 0);
    
    int accepted_fd = ipc_server_accept(server_fd);
    ASSERT_TRUE(accepted_fd >= 0);
    
    /* Send a long line */
    char long_msg[1024];
    memset(long_msg, 'A', sizeof(long_msg) - 2);
    long_msg[sizeof(long_msg) - 2] = '\n';
    long_msg[sizeof(long_msg) - 1] = '\0';
    write(client_fd, long_msg, strlen(long_msg));
    
    char buffer[2048];
    ssize_t n = ipc_read_line(accepted_fd, buffer, sizeof(buffer), 1000);
    ASSERT_TRUE(n > 0);
    
    close(client_fd);
    close(accepted_fd);
    ipc_server_destroy(server_fd, TEST_SOCKET_PATH);
#else
    ASSERT_TRUE(1);
#endif
}

TEST(ipc_read_line_timeout) {
#ifdef __linux__
    unlink(TEST_SOCKET_PATH);
    int server_fd = ipc_server_create(TEST_SOCKET_PATH);
    ASSERT_TRUE(server_fd >= 0);
    
    int client_fd = create_test_client(TEST_SOCKET_PATH);
    ASSERT_TRUE(client_fd >= 0);
    
    int accepted_fd = ipc_server_accept(server_fd);
    ASSERT_TRUE(accepted_fd >= 0);
    
    /* Don't send anything, should timeout */
    char buffer[256];
    ssize_t n = ipc_read_line(accepted_fd, buffer, sizeof(buffer), 100);  /* 100ms timeout */
    ASSERT_TRUE(n <= 0);  /* Should timeout or return error */
    
    close(client_fd);
    close(accepted_fd);
    ipc_server_destroy(server_fd, TEST_SOCKET_PATH);
#else
    ASSERT_TRUE(1);
#endif
}

TEST(ipc_read_line_client_disconnect) {
#ifdef __linux__
    unlink(TEST_SOCKET_PATH);
    int server_fd = ipc_server_create(TEST_SOCKET_PATH);
    ASSERT_TRUE(server_fd >= 0);
    
    int client_fd = create_test_client(TEST_SOCKET_PATH);
    ASSERT_TRUE(client_fd >= 0);
    
    int accepted_fd = ipc_server_accept(server_fd);
    ASSERT_TRUE(accepted_fd >= 0);
    
    /* Client disconnects */
    close(client_fd);
    
    char buffer[256];
    ssize_t n = ipc_read_line(accepted_fd, buffer, sizeof(buffer), 1000);
    ASSERT_TRUE(n <= 0);  /* Should return 0 or error */
    
    close(accepted_fd);
    ipc_server_destroy(server_fd, TEST_SOCKET_PATH);
#else
    ASSERT_TRUE(1);
#endif
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Write Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(ipc_write_simple) {
#ifdef __linux__
    unlink(TEST_SOCKET_PATH);
    int server_fd = ipc_server_create(TEST_SOCKET_PATH);
    ASSERT_TRUE(server_fd >= 0);
    
    int client_fd = create_test_client(TEST_SOCKET_PATH);
    ASSERT_TRUE(client_fd >= 0);
    
    int accepted_fd = ipc_server_accept(server_fd);
    ASSERT_TRUE(accepted_fd >= 0);
    
    /* Server writes response */
    const char *response = "OK\n";
    ssize_t n = ipc_write(accepted_fd, response, strlen(response));
    ASSERT_EQUAL(strlen(response), n);
    
    /* Client should receive it */
    char buffer[256];
    ssize_t r = recv(client_fd, buffer, sizeof(buffer), 0);
    buffer[r] = '\0';
    ASSERT_STR_EQUAL("OK\n", buffer);
    
    close(client_fd);
    close(accepted_fd);
    ipc_server_destroy(server_fd, TEST_SOCKET_PATH);
#else
    ASSERT_TRUE(1);
#endif
}

TEST(ipc_write_long_data) {
#ifdef __linux__
    unlink(TEST_SOCKET_PATH);
    int server_fd = ipc_server_create(TEST_SOCKET_PATH);
    ASSERT_TRUE(server_fd >= 0);
    
    int client_fd = create_test_client(TEST_SOCKET_PATH);
    ASSERT_TRUE(client_fd >= 0);
    
    int accepted_fd = ipc_server_accept(server_fd);
    ASSERT_TRUE(accepted_fd >= 0);
    
    /* Write a larger buffer */
    char data[4096];
    memset(data, 'X', sizeof(data));
    
    ssize_t n = ipc_write(accepted_fd, data, sizeof(data));
    ASSERT_EQUAL(sizeof(data), n);
    
    close(client_fd);
    close(accepted_fd);
    ipc_server_destroy(server_fd, TEST_SOCKET_PATH);
#else
    ASSERT_TRUE(1);
#endif
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Command Parsing Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(ipc_parse_command_ping) {
    char cmd[64] = "PING\n";
    char *arg = NULL;
    int type = ipc_parse_command(cmd, &arg);
    ASSERT_EQUAL(IPC_CMD_PING, type);
}

TEST(ipc_parse_command_quit) {
    char cmd[64] = "QUIT\n";
    char *arg = NULL;
    int type = ipc_parse_command(cmd, &arg);
    ASSERT_EQUAL(IPC_CMD_QUIT, type);
}

TEST(ipc_parse_command_update) {
    char cmd[64] = "UPDATE\n";
    char *arg = NULL;
    int type = ipc_parse_command(cmd, &arg);
    ASSERT_EQUAL(IPC_CMD_UPDATE, type);
}

TEST(ipc_parse_command_set_face) {
    char cmd[64] = "SET_FACE (◕‿‿◕)\n";
    char *arg = NULL;
    int type = ipc_parse_command(cmd, &arg);
    ASSERT_EQUAL(IPC_CMD_SET_FACE, type);
    ASSERT_NOT_NULL(arg);
}

TEST(ipc_parse_command_set_status) {
    char cmd[64] = "SET_STATUS Hello World!\n";
    char *arg = NULL;
    int type = ipc_parse_command(cmd, &arg);
    ASSERT_EQUAL(IPC_CMD_SET_STATUS, type);
    ASSERT_NOT_NULL(arg);
}

TEST(ipc_parse_command_set_channel) {
    char cmd[64] = "SET_CHANNEL 11\n";
    char *arg = NULL;
    int type = ipc_parse_command(cmd, &arg);
    ASSERT_EQUAL(IPC_CMD_SET_CHANNEL, type);
    ASSERT_NOT_NULL(arg);
}

TEST(ipc_parse_command_set_aps) {
    char cmd[64] = "SET_APS 5 (10)\n";
    char *arg = NULL;
    int type = ipc_parse_command(cmd, &arg);
    ASSERT_EQUAL(IPC_CMD_SET_APS, type);
}

TEST(ipc_parse_command_set_mode) {
    char cmd[64] = "SET_MODE AUTO\n";
    char *arg = NULL;
    int type = ipc_parse_command(cmd, &arg);
    ASSERT_EQUAL(IPC_CMD_SET_MODE, type);
}

TEST(ipc_parse_command_unknown) {
    char cmd[64] = "UNKNOWN_CMD\n";
    char *arg = NULL;
    int type = ipc_parse_command(cmd, &arg);
    ASSERT_EQUAL(IPC_CMD_UNKNOWN, type);
}

TEST(ipc_parse_command_empty) {
    char cmd[64] = "\n";
    char *arg = NULL;
    int type = ipc_parse_command(cmd, &arg);
    ASSERT_EQUAL(IPC_CMD_UNKNOWN, type);
}

TEST(ipc_parse_command_null) {
    char *arg = NULL;
    int type = ipc_parse_command(NULL, &arg);
    ASSERT_EQUAL(IPC_CMD_UNKNOWN, type);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Full Communication Test
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(ipc_full_communication_flow) {
#ifdef __linux__
    unlink(TEST_SOCKET_PATH);
    int server_fd = ipc_server_create(TEST_SOCKET_PATH);
    ASSERT_TRUE(server_fd >= 0);
    
    int client_fd = create_test_client(TEST_SOCKET_PATH);
    ASSERT_TRUE(client_fd >= 0);
    
    int accepted_fd = ipc_server_accept(server_fd);
    ASSERT_TRUE(accepted_fd >= 0);
    
    /* Full command/response flow */
    const char *commands[] = {
        "PING\n",
        "SET_FACE (◕‿‿◕)\n",
        "SET_STATUS Testing!\n",
        "UPDATE\n"
    };
    
    for (size_t i = 0; i < sizeof(commands)/sizeof(commands[0]); i++) {
        write(client_fd, commands[i], strlen(commands[i]));
        
        char buffer[256];
        ssize_t n = ipc_read_line(accepted_fd, buffer, sizeof(buffer), 1000);
        ASSERT_TRUE(n > 0);
        
        /* Send OK response */
        ipc_write(accepted_fd, "OK\n", 3);
    }
    
    close(client_fd);
    close(accepted_fd);
    ipc_server_destroy(server_fd, TEST_SOCKET_PATH);
#else
    ASSERT_TRUE(1);
#endif
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test Suite Runner
 * ═══════════════════════════════════════════════════════════════════════════ */

void run_suite_ipc(void) {
    printf("\n");
    printf("Ipc Module Tests\n");
    printf("================\n");

    /* Server creation tests */
    RUN_TEST(ipc_server_create_success);
    RUN_TEST(ipc_server_create_removes_existing_socket);
    RUN_TEST(ipc_server_destroy_removes_socket);
    RUN_TEST(ipc_server_destroy_null_path_safe);
    RUN_TEST(ipc_server_destroy_invalid_fd_safe);
    
    /* Client connection tests */
    RUN_TEST(ipc_server_accept_returns_client_fd);
    RUN_TEST(ipc_server_accept_no_pending_returns_minus_one);
    RUN_TEST(ipc_server_accept_multiple_clients);
    
    /* Read line tests */
    RUN_TEST(ipc_read_line_simple);
    RUN_TEST(ipc_read_line_with_data);
    RUN_TEST(ipc_read_line_long_line);
    RUN_TEST(ipc_read_line_timeout);
    RUN_TEST(ipc_read_line_client_disconnect);
    
    /* Write tests */
    RUN_TEST(ipc_write_simple);
    RUN_TEST(ipc_write_long_data);
    
    /* Command parsing tests */
    RUN_TEST(ipc_parse_command_ping);
    RUN_TEST(ipc_parse_command_quit);
    RUN_TEST(ipc_parse_command_update);
    RUN_TEST(ipc_parse_command_set_face);
    RUN_TEST(ipc_parse_command_set_status);
    RUN_TEST(ipc_parse_command_set_channel);
    RUN_TEST(ipc_parse_command_set_aps);
    RUN_TEST(ipc_parse_command_set_mode);
    RUN_TEST(ipc_parse_command_unknown);
    RUN_TEST(ipc_parse_command_empty);
    RUN_TEST(ipc_parse_command_null);
    
    /* Full communication test */
    RUN_TEST(ipc_full_communication_flow);
}

#ifndef TEST_ALL
int main(void) {
    printf("PwnaUI IPC Module Tests\n");
    printf("=======================\n");
    
    run_suite_ipc();
    
    test_print_summary();
    return test_exit_code();
}
#endif

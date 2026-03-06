#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <stdint.h>

/* Initialize web server on specified port */
int webserver_init(int port);

/* Process web server requests (non-blocking) */
int webserver_poll(int server_fd);

/* Cleanup web server */
void webserver_cleanup(int server_fd);
void webserver_set_theme(const char *theme);

/* Set state callback for JSON API */
typedef void (*webserver_state_callback_t)(char *buf, size_t bufsize);
void webserver_set_state_callback(webserver_state_callback_t cb);

/* GPS callback for Crack City current position */
typedef void (*webserver_gps_callback_t)(double *lat, double *lon, int *has_fix);
void webserver_set_gps_callback(webserver_gps_callback_t cb);

/* Name-change callback — called when POST /api/config/name updates the name.
 * The callback receives the new name (already validated & lowercased for hostname).
 * It should update g_ui_state.name and optionally config.toml. */
typedef void (*webserver_name_callback_t)(const char *new_name);
void webserver_set_name_callback(webserver_name_callback_t cb);

#endif /* WEBSERVER_H */

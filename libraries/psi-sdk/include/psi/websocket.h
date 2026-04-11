/*
 * psi/websocket.h — WebSocket server utilities for the psi platform.
 *
 * Handles the HTTP upgrade handshake and provides simple send/recv
 * for text and binary frames.  Built on psi_read/psi_write.
 *
 * Usage:
 *   int conn = psi_accept(0);
 *   if (psi_ws_accept(conn) < 0) { psi_close(conn); return; }
 *   psi_ws_send_text(conn, "hello", 5);
 *   char buf[512]; int n = psi_ws_recv(conn, buf, sizeof(buf));
 */

#ifndef PSI_WEBSOCKET_H
#define PSI_WEBSOCKET_H

#ifdef __cplusplus
extern "C" {
#endif

/* Perform the HTTP → WebSocket upgrade handshake on an accepted connection.
 * Reads the HTTP request, validates Upgrade: websocket, computes
 * Sec-WebSocket-Accept, and sends the HTTP 101 response.
 *
 * Returns 0 on success, -1 on failure (bad request, missing headers, etc.).
 * On failure the connection is still open — caller should close it.
 */
int psi_ws_accept(int conn_fd);

/* Send a WebSocket text frame.
 * Returns 0 on success, -1 on error.
 */
int psi_ws_send_text(int conn_fd, const char* data, int len);

/* Send a WebSocket binary frame.
 * Returns 0 on success, -1 on error.
 */
int psi_ws_send_binary(int conn_fd, const char* data, int len);

/* Send a WebSocket close frame.
 * Returns 0 on success, -1 on error.
 */
int psi_ws_send_close(int conn_fd);

/* Receive a WebSocket message.
 * Reads and decodes one complete frame (handles unmasking).
 * Returns the number of payload bytes written to buf,
 * 0 on close frame / EOF, or -1 on error.
 *
 * The opcode (WS_OP_TEXT=1, WS_OP_BINARY=2, etc.) is written to *opcode
 * if opcode is non-NULL.
 */
int psi_ws_recv(int conn_fd, char* buf, int buf_len, int* opcode);

#ifdef __cplusplus
}
#endif

#endif /* PSI_WEBSOCKET_H */

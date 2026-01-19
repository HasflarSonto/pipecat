/**
 * WebSocket Client for Luna Simulator
 * Uses libwebsockets for WebSocket communication
 */

#include "ws_client_sim.h"
#include <libwebsockets.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Buffer sizes */
#define RX_BUFFER_SIZE  (64 * 1024)
#define TX_BUFFER_SIZE  (64 * 1024)

/* State */
static struct lws_context* s_context = NULL;
static struct lws* s_wsi = NULL;
static ws_event_callback_t s_callback = NULL;
static bool s_connected = false;
static bool s_connecting = false;

/* Connection parameters */
static char s_host[256] = {0};
static int s_port = 0;
static char s_path[256] = {0};

/* Receive buffer for fragmented messages */
static uint8_t* s_rx_buffer = NULL;
static size_t s_rx_len = 0;
static bool s_rx_is_binary = false;

/* Send queue (simple single-message queue) */
static uint8_t* s_tx_buffer = NULL;
static size_t s_tx_len = 0;
static bool s_tx_is_binary = false;
static bool s_tx_pending = false;

/**
 * WebSocket callback
 */
static int ws_callback(struct lws* wsi, enum lws_callback_reasons reason,
                       void* user, void* in, size_t len)
{
    (void)user;

    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            printf("WebSocket connected\n");
            s_connected = true;
            s_connecting = false;
            if (s_callback) {
                s_callback(WS_EVENT_CONNECTED, NULL, 0);
            }
            break;

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            fprintf(stderr, "WebSocket connection error: %s\n",
                    in ? (char*)in : "unknown");
            s_connected = false;
            s_connecting = false;
            if (s_callback) {
                s_callback(WS_EVENT_ERROR, in, len);
            }
            break;

        case LWS_CALLBACK_CLOSED:
        case LWS_CALLBACK_CLIENT_CLOSED:
            printf("WebSocket disconnected\n");
            s_connected = false;
            s_connecting = false;
            s_wsi = NULL;
            if (s_callback) {
                s_callback(WS_EVENT_DISCONNECTED, NULL, 0);
            }
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE:
            {
                bool is_binary = lws_frame_is_binary(wsi);
                bool is_first = lws_is_first_fragment(wsi);
                bool is_final = lws_is_final_fragment(wsi);

                if (is_first) {
                    /* Start new message */
                    s_rx_len = 0;
                    s_rx_is_binary = is_binary;
                }

                /* Append to buffer */
                if (s_rx_len + len <= RX_BUFFER_SIZE) {
                    memcpy(s_rx_buffer + s_rx_len, in, len);
                    s_rx_len += len;
                }

                if (is_final) {
                    /* Complete message received */
                    if (s_callback) {
                        if (s_rx_is_binary) {
                            s_callback(WS_EVENT_BINARY, s_rx_buffer, s_rx_len);
                        } else {
                            /* Null-terminate text */
                            if (s_rx_len < RX_BUFFER_SIZE) {
                                s_rx_buffer[s_rx_len] = '\0';
                            }
                            s_callback(WS_EVENT_TEXT, s_rx_buffer, s_rx_len);
                        }
                    }
                    s_rx_len = 0;
                }
            }
            break;

        case LWS_CALLBACK_CLIENT_WRITEABLE:
            if (s_tx_pending && s_tx_len > 0) {
                /* Prepare buffer with LWS_PRE padding */
                size_t padded_len = LWS_PRE + s_tx_len;
                uint8_t* buf = malloc(padded_len);
                if (buf) {
                    memcpy(buf + LWS_PRE, s_tx_buffer, s_tx_len);

                    int flags = s_tx_is_binary ? LWS_WRITE_BINARY : LWS_WRITE_TEXT;
                    int written = lws_write(wsi, buf + LWS_PRE, s_tx_len, flags);

                    free(buf);

                    if (written < 0) {
                        fprintf(stderr, "WebSocket write failed\n");
                    }
                }
                s_tx_pending = false;
                s_tx_len = 0;
            }
            break;

        default:
            break;
    }

    return 0;
}

/* Protocol definition */
static const struct lws_protocols protocols[] = {
    {
        .name = "luna-protocol",
        .callback = ws_callback,
        .per_session_data_size = 0,
        .rx_buffer_size = RX_BUFFER_SIZE,
    },
    { NULL, NULL, 0, 0 }  /* terminator */
};

bool ws_client_init(ws_event_callback_t callback)
{
    if (s_context) {
        return true;  /* Already initialized */
    }

    s_callback = callback;

    /* Allocate buffers */
    s_rx_buffer = malloc(RX_BUFFER_SIZE);
    s_tx_buffer = malloc(TX_BUFFER_SIZE);

    if (!s_rx_buffer || !s_tx_buffer) {
        fprintf(stderr, "Failed to allocate WebSocket buffers\n");
        free(s_rx_buffer);
        free(s_tx_buffer);
        return false;
    }

    /* Create context */
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));

    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    s_context = lws_create_context(&info);
    if (!s_context) {
        fprintf(stderr, "Failed to create WebSocket context\n");
        free(s_rx_buffer);
        free(s_tx_buffer);
        return false;
    }

    printf("WebSocket client initialized\n");
    return true;
}

void ws_client_deinit(void)
{
    ws_client_disconnect();

    if (s_context) {
        lws_context_destroy(s_context);
        s_context = NULL;
    }

    free(s_rx_buffer);
    free(s_tx_buffer);
    s_rx_buffer = NULL;
    s_tx_buffer = NULL;
    s_callback = NULL;

    printf("WebSocket client deinitialized\n");
}

bool ws_client_connect(const char* host, int port, const char* path)
{
    if (!s_context) {
        fprintf(stderr, "WebSocket not initialized\n");
        return false;
    }

    if (s_connected || s_connecting) {
        printf("Already connected/connecting\n");
        return true;
    }

    /* Save connection parameters */
    strncpy(s_host, host, sizeof(s_host) - 1);
    s_port = port;
    strncpy(s_path, path, sizeof(s_path) - 1);

    /* Create connection */
    struct lws_client_connect_info ccinfo;
    memset(&ccinfo, 0, sizeof(ccinfo));

    ccinfo.context = s_context;
    ccinfo.address = s_host;
    ccinfo.port = s_port;
    ccinfo.path = s_path;
    ccinfo.host = s_host;
    ccinfo.origin = s_host;
    ccinfo.protocol = NULL;  /* Don't require specific subprotocol - FastAPI doesn't accept it */
    ccinfo.pwsi = &s_wsi;

    printf("Connecting to ws://%s:%d%s\n", host, port, path);

    s_wsi = lws_client_connect_via_info(&ccinfo);
    if (!s_wsi) {
        fprintf(stderr, "Failed to start WebSocket connection\n");
        return false;
    }

    s_connecting = true;
    return true;
}

void ws_client_disconnect(void)
{
    if (s_wsi) {
        /* Request graceful close */
        s_connected = false;
        s_connecting = false;
        /* The close will happen in the service loop */
    }
}

bool ws_client_is_connected(void)
{
    return s_connected;
}

void ws_client_service(int timeout_ms)
{
    (void)timeout_ms;  /* Always non-blocking */

    /* Skip service if no context or no active/pending connection
     * lws_service() can block even with timeout=0 after connection failure */
    if (!s_context) {
        return;
    }

    /* Only service if we have an active connection or are trying to connect */
    if (s_connected || s_connecting) {
        lws_service(s_context, 0);
    }
}

bool ws_client_send_text(const char* text)
{
    if (!s_connected || !s_wsi || !text) {
        return false;
    }

    size_t len = strlen(text);
    if (len >= TX_BUFFER_SIZE) {
        fprintf(stderr, "Text too long to send\n");
        return false;
    }

    /* Queue the message */
    memcpy(s_tx_buffer, text, len);
    s_tx_len = len;
    s_tx_is_binary = false;
    s_tx_pending = true;

    /* Request write callback */
    lws_callback_on_writable(s_wsi);

    return true;
}

bool ws_client_send_binary(const void* data, size_t len)
{
    if (!s_connected || !s_wsi || !data || len == 0) {
        return false;
    }

    if (len >= TX_BUFFER_SIZE) {
        fprintf(stderr, "Binary data too large to send\n");
        return false;
    }

    /* Queue the message */
    memcpy(s_tx_buffer, data, len);
    s_tx_len = len;
    s_tx_is_binary = true;
    s_tx_pending = true;

    /* Request write callback */
    lws_callback_on_writable(s_wsi);

    return true;
}

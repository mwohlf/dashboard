/**
 * ha_client.c
 *
 * Home Assistant WebSocket API client.
 *
 * Protocol summary:
 *   1. Connect to  ws://HA_HOST:8123/api/websocket
 *   2. Receive:    {"type":"auth_required", ...}
 *   3. Send:       {"type":"auth","access_token":"<TOKEN>"}
 *   4. Receive:    {"type":"auth_ok", ...}
 *   5. Send:       {"id":1,"type":"get_states"}
 *   6. Receive:    {"id":1,"type":"result","success":true,"result":[...]}
 *   7. Send:       {"id":2,"type":"subscribe_events","event_type":"state_changed"}
 *   8. Receive:    state_changed events → parsed → state_cb()
 *
 * Service calls (toggle a light, etc.):
 *   Send:  {"id":N,"type":"call_service","domain":"light",
 *           "service":"toggle","service_data":{"entity_id":"..."}}
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include "config.h"
#include "ha_client.h"

static const char *TAG = "ha_client";

/* ------------------------------------------------------------------ */
/*  Internal state                                                      */
/* ------------------------------------------------------------------ */
#define HA_WS_RX_BUF_SIZE  (256 * 1024)

static esp_websocket_client_handle_t s_ws_client = NULL;
static ha_state_cb_t s_state_cb  = NULL;
static ha_conn_cb_t  s_conn_cb   = NULL;
static void         *s_user_ctx  = NULL;
static volatile ha_conn_status_t s_conn_status = HA_CONN_DISCONNECTED;
static volatile int  s_msg_id    = 10;   /* start above reserved IDs */

/* Larger receive buffer assembled across fragmented frames */
static char   *s_rx_buf   = NULL;
static size_t  s_rx_used  = 0;

/* ------------------------------------------------------------------ */
/*  Helpers                                                             */
/* ------------------------------------------------------------------ */
static ha_entity_type_t domain_to_type(const char *entity_id)
{
    if (!entity_id) return HA_ENTITY_UNKNOWN;
    if (strncmp(entity_id, "light.",         6) == 0) return HA_ENTITY_LIGHT;
    if (strncmp(entity_id, "switch.",        7) == 0) return HA_ENTITY_SWITCH;
    if (strncmp(entity_id, "sensor.",        7) == 0) return HA_ENTITY_SENSOR;
    if (strncmp(entity_id, "binary_sensor.", 14) == 0) return HA_ENTITY_BINARY_SENSOR;
    if (strncmp(entity_id, "climate.",       8) == 0) return HA_ENTITY_CLIMATE;
    if (strncmp(entity_id, "cover.",         6) == 0) return HA_ENTITY_COVER;
    return HA_ENTITY_UNKNOWN;
}

static void send_json(const char *payload)
{
    if (!s_ws_client || !payload) return;
    ESP_LOGD(TAG, "TX: %s", payload);
    esp_websocket_client_send_text(s_ws_client, payload, strlen(payload),
                                   pdMS_TO_TICKS(3000));
}

static void parse_entity_from_json(cJSON *state_obj)
{
    if (!state_obj || !s_state_cb) return;

    cJSON *j_eid   = cJSON_GetObjectItem(state_obj, "entity_id");
    cJSON *j_state = cJSON_GetObjectItem(state_obj, "state");
    cJSON *j_attrs = cJSON_GetObjectItem(state_obj, "attributes");

    if (!j_eid || !j_state) return;

    ha_entity_t entity = {0};
    strlcpy(entity.entity_id, j_eid->valuestring,   sizeof(entity.entity_id));
    strlcpy(entity.state,     j_state->valuestring,  sizeof(entity.state));
    entity.type  = domain_to_type(entity.entity_id);
    entity.valid = true;

    /* Friendly name */
    if (j_attrs) {
        cJSON *j_name = cJSON_GetObjectItem(j_attrs, "friendly_name");
        if (j_name && j_name->valuestring) {
            strlcpy(entity.name, j_name->valuestring, sizeof(entity.name));
        }
        /* Unit for sensors */
        cJSON *j_unit = cJSON_GetObjectItem(j_attrs, "unit_of_measurement");
        if (j_unit && j_unit->valuestring) {
            strlcpy(entity.unit, j_unit->valuestring, sizeof(entity.unit));
        }
        /* Device class (temperature, door, window, etc.) */
        cJSON *j_dc = cJSON_GetObjectItem(j_attrs, "device_class");
        if (j_dc && j_dc->valuestring) {
            strlcpy(entity.device_class, j_dc->valuestring, sizeof(entity.device_class));
        }
    }
    /* Fall back to entity_id if no friendly name */
    if (entity.name[0] == '\0') {
        strlcpy(entity.name, entity.entity_id, sizeof(entity.name));
    }

    s_state_cb(&entity, s_user_ctx);
}

/* ------------------------------------------------------------------ */
/*  Handle a complete JSON message                                      */
/* ------------------------------------------------------------------ */
static void handle_message(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse JSON (len=%d)", len);
        return;
    }

    cJSON *j_type = cJSON_GetObjectItem(root, "type");
    if (!j_type || !j_type->valuestring) {
        cJSON_Delete(root);
        return;
    }
    const char *msg_type = j_type->valuestring;

    /* ---------- auth_required ---------- */
    if (strcmp(msg_type, "auth_required") == 0) {
        ESP_LOGI(TAG, "HA auth required — sending token");
        char auth_buf[512];
        snprintf(auth_buf, sizeof(auth_buf),
                 "{\"type\":\"auth\",\"access_token\":\"%s\"}", HA_TOKEN);
        send_json(auth_buf);
    }
    /* ---------- auth_ok ---------- */
    else if (strcmp(msg_type, "auth_ok") == 0) {
        ESP_LOGI(TAG, "HA authenticated");
        s_conn_status = HA_CONN_AUTHENTICATED;
        if (s_conn_cb) s_conn_cb(HA_CONN_AUTHENTICATED, s_user_ctx);

        /* Fetch all states */
        send_json("{\"id\":1,\"type\":\"get_states\"}");

        /* Subscribe to state changes */
        send_json("{\"id\":2,\"type\":\"subscribe_events\","
                  "\"event_type\":\"state_changed\"}");
    }
    /* ---------- auth_invalid ---------- */
    else if (strcmp(msg_type, "auth_invalid") == 0) {
        ESP_LOGE(TAG, "HA auth invalid — check HA_TOKEN in config.h");
    }
    /* ---------- result (get_states) ---------- */
    else if (strcmp(msg_type, "result") == 0) {
        cJSON *j_id  = cJSON_GetObjectItem(root, "id");
        cJSON *j_ok  = cJSON_GetObjectItem(root, "success");
        if (j_id && j_id->valueint == 1 && j_ok && cJSON_IsTrue(j_ok)) {
            cJSON *j_result = cJSON_GetObjectItem(root, "result");
            if (cJSON_IsArray(j_result)) {
                int n = cJSON_GetArraySize(j_result);
                ESP_LOGI(TAG, "Processing %d states from get_states", n);
                for (int i = 0; i < n; i++) {
                    parse_entity_from_json(cJSON_GetArrayItem(j_result, i));
                }
            }
        }
    }
    /* ---------- event (state_changed) ---------- */
    else if (strcmp(msg_type, "event") == 0) {
        cJSON *j_event    = cJSON_GetObjectItem(root, "event");
        cJSON *j_evt_type = j_event ? cJSON_GetObjectItem(j_event, "event_type") : NULL;
        if (j_evt_type && strcmp(j_evt_type->valuestring, "state_changed") == 0) {
            cJSON *j_data     = cJSON_GetObjectItem(j_event, "data");
            cJSON *j_new_state = j_data ? cJSON_GetObjectItem(j_data, "new_state") : NULL;
            parse_entity_from_json(j_new_state);
        }
    }
    /* ---------- pong / other ---------- */
    else {
        ESP_LOGD(TAG, "Unhandled msg type: %s", msg_type);
    }

    cJSON_Delete(root);
}

/* ------------------------------------------------------------------ */
/*  WebSocket event handler                                             */
/* ------------------------------------------------------------------ */
static void ws_event_handler(void                      *handler_args,
                             esp_event_base_t           base,
                             int32_t                    event_id,
                             void                      *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {

    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        s_conn_status = HA_CONN_CONNECTING;
        s_rx_used = 0;
        if (s_conn_cb) s_conn_cb(HA_CONN_CONNECTING, s_user_ctx);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected");
        s_conn_status = HA_CONN_DISCONNECTED;
        s_rx_used = 0;
        if (s_conn_cb) s_conn_cb(HA_CONN_DISCONNECTED, s_user_ctx);
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x08) {  /* close frame */
            ESP_LOGW(TAG, "WebSocket close frame received");
            break;
        }
        if (!data->data_ptr || data->data_len <= 0) break;

        /* Accumulate chunks — esp_websocket_client splits large frames
         * into buffer_size chunks.  payload_offset tells us where this
         * chunk sits within the full WebSocket payload. */
        if (s_rx_used + data->data_len < HA_WS_RX_BUF_SIZE) {
            memcpy(s_rx_buf + s_rx_used, data->data_ptr, data->data_len);
            s_rx_used += data->data_len;
        } else {
            ESP_LOGE(TAG, "RX buffer overflow (%d + %d > %d) — dropping",
                     (int)s_rx_used, data->data_len, HA_WS_RX_BUF_SIZE);
            s_rx_used = 0;
            break;
        }

        /* Complete payload received?  payload_len is the total frame
         * payload size; we're done when offset + chunk reaches it. */
        if (data->payload_offset + data->data_len >= data->payload_len) {
            s_rx_buf[s_rx_used] = '\0';
            ESP_LOGD(TAG, "RX complete (%d bytes)", (int)s_rx_used);
            handle_message(s_rx_buf, (int)s_rx_used);
            s_rx_used = 0;
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */
esp_err_t ha_client_start(ha_state_cb_t state_cb,
                          ha_conn_cb_t  conn_cb,
                          void         *user_ctx)
{
    s_state_cb = state_cb;
    s_conn_cb  = conn_cb;
    s_user_ctx = user_ctx;

    s_rx_buf = heap_caps_malloc(HA_WS_RX_BUF_SIZE + 1, MALLOC_CAP_SPIRAM);
    if (!s_rx_buf) {
        ESP_LOGE(TAG, "Failed to allocate RX buffer in PSRAM");
        return ESP_ERR_NO_MEM;
    }

    esp_websocket_client_config_t ws_cfg = {
        .uri              = HA_WS_URI,
        .port             = HA_PORT,
        .buffer_size      = 16384,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms   = 10000,
        .ping_interval_sec    = 30,
        .disable_auto_reconnect = false,
    };

    s_ws_client = esp_websocket_client_init(&ws_cfg);
    if (!s_ws_client) {
        ESP_LOGE(TAG, "esp_websocket_client_init failed");
        return ESP_FAIL;
    }

    esp_websocket_register_events(s_ws_client,
                                  WEBSOCKET_EVENT_ANY,
                                  ws_event_handler, NULL);

    ESP_RETURN_ON_ERROR(esp_websocket_client_start(s_ws_client),
                        TAG, "esp_websocket_client_start failed");

    ESP_LOGI(TAG, "HA WebSocket client started → %s", HA_WS_URI);
    return ESP_OK;
}

esp_err_t ha_call_service(const char *domain,
                          const char *service,
                          const char *entity_id)
{
    if (!s_ws_client || s_conn_status != HA_CONN_AUTHENTICATED) {
        ESP_LOGW(TAG, "ha_call_service: not connected");
        return ESP_ERR_INVALID_STATE;
    }

    int id = ++s_msg_id;
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"id\":%d,\"type\":\"call_service\","
             "\"domain\":\"%s\",\"service\":\"%s\","
             "\"service_data\":{\"entity_id\":\"%s\"}}",
             id, domain, service, entity_id);
    send_json(buf);
    return ESP_OK;
}

ha_conn_status_t ha_get_conn_status(void)
{
    return s_conn_status;
}

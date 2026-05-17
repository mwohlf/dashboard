#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Entity types we care about                                          */
/* ------------------------------------------------------------------ */
typedef enum {
    HA_ENTITY_UNKNOWN = 0,
    HA_ENTITY_LIGHT,
    HA_ENTITY_SWITCH,
    HA_ENTITY_SENSOR,
    HA_ENTITY_BINARY_SENSOR,
    HA_ENTITY_CLIMATE,
    HA_ENTITY_COVER,
} ha_entity_type_t;

/* ------------------------------------------------------------------ */
/*  Entity descriptor                                                   */
/* ------------------------------------------------------------------ */
#define HA_MAX_ENTITY_ID_LEN   64
#define HA_MAX_NAME_LEN        48
#define HA_MAX_STATE_LEN       32
#define HA_MAX_ATTR_LEN        64   /* e.g. unit_of_measurement */
#define HA_MAX_DEVCLASS_LEN    32   /* e.g. temperature, door, window */

typedef struct {
    char            entity_id[HA_MAX_ENTITY_ID_LEN];
    char            name[HA_MAX_NAME_LEN];
    char            state[HA_MAX_STATE_LEN];
    char            unit[HA_MAX_ATTR_LEN];       /* sensor: unit_of_measurement */
    char            device_class[HA_MAX_DEVCLASS_LEN]; /* e.g. "temperature", "door" */
    ha_entity_type_t type;
    bool            valid;
} ha_entity_t;

/* ------------------------------------------------------------------ */
/*  Callback invoked from the HA client task whenever an entity changes */
/*  state.  Runs in the HA client task context — do not call LVGL       */
/*  directly; use lvgl_port_lock() or post a message to the UI task.   */
/* ------------------------------------------------------------------ */
typedef void (*ha_state_cb_t)(const ha_entity_t *entity, void *ctx);

/* ------------------------------------------------------------------ */
/*  Connection status callback                                          */
/* ------------------------------------------------------------------ */
typedef enum {
    HA_CONN_CONNECTING = 0,
    HA_CONN_AUTHENTICATED,
    HA_CONN_DISCONNECTED,
} ha_conn_status_t;

typedef void (*ha_conn_cb_t)(ha_conn_status_t status, void *ctx);

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief  Start the HA WebSocket client task.
 *         Connects to HA_WS_URI from config.h, authenticates,
 *         fetches all states, then subscribes to state_changed events.
 *
 * @param  state_cb   Called whenever an entity state changes (may be NULL)
 * @param  conn_cb    Called on connect / disconnect (may be NULL)
 * @param  user_ctx   Passed as-is to both callbacks
 */
esp_err_t ha_client_start(ha_state_cb_t state_cb,
                          ha_conn_cb_t  conn_cb,
                          void         *user_ctx);

/**
 * @brief  Send a call_service command to HA.
 *         Thread-safe; may be called from any task.
 *
 * @param  domain       e.g. "light", "switch", "cover"
 * @param  service      e.g. "toggle", "turn_on", "turn_off"
 * @param  entity_id    e.g. "light.living_room"
 */
esp_err_t ha_call_service(const char *domain,
                          const char *service,
                          const char *entity_id);

/**
 * @brief Return HA connection status (atomic read).
 */
ha_conn_status_t ha_get_conn_status(void);

#pragma once
#include <stdint.h>
#include <stdbool.h>

// ─── BLE Service & Characteristic UUIDs ─────────────────────────────────────
#define UUID_ASTRALPOOL_SERVICE     "45000001-98b7-4e29-a03f-160174643001"
#define UUID_SLAVE_SESSION_KEY      "45000002-98b7-4e29-a03f-160174643001"
#define UUID_MASTER_AUTHENTICATION  "45000003-98b7-4e29-a03f-160174643001"
#define UUID_CHLORINATOR_STATE      "45000200-98b7-4e29-a03f-160174643001"
#define UUID_CHLORINATOR_APP_ACTION "45000203-98b7-4e29-a03f-160174643001"

// ─── Enumerations ────────────────────────────────────────────────────────────

typedef enum {
    MODE_BLE_ERROR = -1,  // sentinel: BLE operation failed, posted via state queue
    MODE_OFF       = 0,
    MODE_MANUAL    = 1,
    MODE_AUTO      = 2,
} ChlorinatorMode;

typedef enum {
    SPEED_LOW    = 0,
    SPEED_MEDIUM = 1,
    SPEED_HIGH   = 2,
    SPEED_AI     = 3,
    SPEED_NOTSET = 255,   // Python uses -1; stored as 255 in uint8
} SpeedLevel;

typedef enum {
    INFO_NONE                        = 0,
    INFO_PH_PROBE_NO_COMMS           = 1,
    INFO_PH_PROBE_OTHER_ERROR        = 2,
    INFO_PH_PROBE_CLEAN_CALIBRATE    = 3,
    INFO_ORP_PROBE_NO_COMMS          = 4,
    INFO_ORP_PROBE_OTHER_ERROR       = 5,
    INFO_ORP_PROBE_CLEAN_CALIBRATE   = 6,
    INFO_G4_COMMS_FAILURE            = 7,
    INFO_NO_WATER_FLOW               = 8,
    INFO_RTCC_FAULT                  = 128,
    INFO_ORP_PROBE_PH_PROBE_MISSING  = 129,
    INFO_AI_PUMP_SPEED               = 130,
    INFO_LOW_SALT                    = 131,
    INFO_UNSPECIFIED                 = 132,
} InfoMessage;

typedef enum {
    CHLORINE_UNKNOWN       = 255,  // -1 in Python
    CHLORINE_INVALID       = 0,
    CHLORINE_VERY_VERY_LOW = 1,
    CHLORINE_VERY_LOW      = 2,
    CHLORINE_LOW           = 3,
    CHLORINE_OK            = 4,
    CHLORINE_HIGH          = 5,
    CHLORINE_VERY_HIGH     = 6,
    CHLORINE_VERY_VERY_HIGH= 7,
} ChlorineStatus;

typedef enum {
    ACTION_NO_ACTION                 = 0,
    ACTION_OFF                       = 1,
    ACTION_AUTO                      = 2,
    ACTION_MANUAL                    = 3,
    ACTION_LOW                       = 4,
    ACTION_MEDIUM                    = 5,
    ACTION_HIGH                      = 6,
    ACTION_POOL                      = 7,
    ACTION_SPA                       = 8,
    ACTION_DISMISS_INFO              = 9,
    ACTION_DISABLE_ACID_INDEFINITELY = 10,
    ACTION_DISABLE_ACID_FOR_PERIOD   = 11,
    ACTION_RESET_STATISTICS          = 12,
    ACTION_TRIGGER_CELL_REVERSAL     = 13,
} ChlorinatorAction;

// ─── State Flags (byte 5 of the decrypted state packet) ──────────────────────
#define FLAG_CHEMISTRY_CURRENT    0x01
#define FLAG_CHEMISTRY_VALID      0x02
#define FLAG_SPA_SELECTION        0x04
#define FLAG_PUMP_PRIMING         0x08
#define FLAG_PUMP_OPERATING       0x10
#define FLAG_CELL_OPERATING       0x20
#define FLAG_SETTINGS_CHANGED     0x40
#define FLAG_SANITISING_TOMORROW  0x80

// ─── Parsed Device State ─────────────────────────────────────────────────────
typedef struct {
    ChlorinatorMode  mode;
    SpeedLevel       pump_speed;
    uint8_t          active_timer;
    InfoMessage      info_message;
    uint8_t          flags;
    float            ph_measurement;       // divided by 10 (e.g. 72 → 7.2)
    ChlorineStatus   chlorine_status;
    uint8_t          time_hours;
    uint8_t          time_minutes;
    uint8_t          time_seconds;
    // Pre-decoded flag bits for convenience
    bool chemistry_current;
    bool chemistry_valid;
    bool spa_selection;
    bool pump_priming;
    bool pump_operating;
    bool cell_operating;
    bool sanitising_tomorrow;
} ChlorinatorState;

// ─── Action Command Packet ────────────────────────────────────────────────────
// Binary layout mirrors Python: struct.pack("=B i 15x", action, period_minutes)
// Total 20 bytes – matches CHAR_DATA_LEN.
typedef struct __attribute__((packed)) {
    uint8_t  action;           // ChlorinatorAction
    int32_t  period_minutes;   // Only used with ACTION_DISABLE_ACID_FOR_PERIOD
    uint8_t  _padding[15];
} ChlorinatorActionPacket;

// ─── Function Declarations ────────────────────────────────────────────────────

// Parse the first 11 bytes of a 20-byte decrypted characteristic into `state`.
void parse_chlorinator_state(const uint8_t *decrypted, ChlorinatorState *state);

// Zero-fill `pkt` and set action / period fields.
void build_action_packet(ChlorinatorAction action, int32_t period_minutes,
                         ChlorinatorActionPacket *pkt);

// Return a human-readable string for an InfoMessage value.
const char *info_message_name(InfoMessage msg);

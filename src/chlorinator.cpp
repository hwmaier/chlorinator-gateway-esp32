#include "chlorinator.h"
#include <string.h>

void parse_chlorinator_state(const uint8_t *data, ChlorinatorState *s) {
    // Python struct format: @BBBBBBBBBBB (11 bytes, native byte order, all uint8)
    // Offsets:
    //   [0] mode
    //   [1] pump_speed
    //   [2] active_timer
    //   [3] info_message
    //   [4] reserved
    //   [5] flags
    //   [6] ph_measurement  (raw, divide by 10)
    //   [7] chlorine_control_status
    //   [8] time_hours
    //   [9] time_minutes
    //  [10] time_seconds

    s->mode             = (ChlorinatorMode)data[0];
    s->pump_speed       = (SpeedLevel)data[1];
    s->active_timer     = data[2];
    s->info_message     = (InfoMessage)data[3];
    // data[4] is reserved
    s->flags            = data[5];
    s->ph_measurement   = data[6] / 10.0f;
    s->chlorine_status  = (ChlorineStatus)data[7];
    s->time_hours       = data[8];
    s->time_minutes     = data[9];
    s->time_seconds     = data[10];

    s->chemistry_current  = (s->flags & FLAG_CHEMISTRY_CURRENT)   != 0;
    s->chemistry_valid    = (s->flags & FLAG_CHEMISTRY_VALID)      != 0;
    s->spa_selection      = (s->flags & FLAG_SPA_SELECTION)        != 0;
    s->pump_priming       = (s->flags & FLAG_PUMP_PRIMING)         != 0;
    s->pump_operating     = (s->flags & FLAG_PUMP_OPERATING)       != 0;
    s->cell_operating     = (s->flags & FLAG_CELL_OPERATING)       != 0;
    s->sanitising_tomorrow= (s->flags & FLAG_SANITISING_TOMORROW)  != 0;
}

void build_action_packet(ChlorinatorAction action, int32_t period_minutes,
                         ChlorinatorActionPacket *pkt) {
    memset(pkt, 0, sizeof(ChlorinatorActionPacket));
    pkt->action         = (uint8_t)action;
    pkt->period_minutes = period_minutes;
}

const char *info_message_name(InfoMessage msg) {
    switch (msg) {
        case INFO_NONE:                       return "NoMessage";
        case INFO_PH_PROBE_NO_COMMS:          return "PhProbeNoComms";
        case INFO_PH_PROBE_OTHER_ERROR:       return "PhProbeOtherError";
        case INFO_PH_PROBE_CLEAN_CALIBRATE:   return "PhProbeCleanCalibrate";
        case INFO_ORP_PROBE_NO_COMMS:         return "OrpProbeNoComms";
        case INFO_ORP_PROBE_OTHER_ERROR:      return "OrpProbeOtherError";
        case INFO_ORP_PROBE_CLEAN_CALIBRATE:  return "OrpProbeCleanCalibrate";
        case INFO_G4_COMMS_FAILURE:           return "G4CommsFailure";
        case INFO_NO_WATER_FLOW:              return "NoWaterFlow";
        case INFO_RTCC_FAULT:                 return "RtccFault";
        case INFO_ORP_PROBE_PH_PROBE_MISSING: return "OrpProbeFittedPhProbeMissing";
        case INFO_AI_PUMP_SPEED:              return "AiPumpSpeed";
        case INFO_LOW_SALT:                   return "LowSalt";
        case INFO_UNSPECIFIED:                return "Unspecified";
        default:                              return "Unknown";
    }
}

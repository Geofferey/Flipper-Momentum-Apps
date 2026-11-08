#include "pocsag_pager_app_i.h"
#include <datetime/datetime.h>
#include <furi_hal_rtc.h>

#define PCSG_TIME_SYNC_MAX_AGE_MS 5000U
#define PCSG_TIME_SYNC_MIN_ADJUST_S 2
#define PCSG_GPS_UTC_OFFSET_S 18U

// Adapted from Mayhem FLEX RX's network_hour/update_rtc; keep the RTC date:
// https://github.com/Geofferey/mayhem-firmware/blob/67345f64389694e45d1873587a0f052c4baed8e5/firmware/application/external/flex_rx/ui_flex_rx.cpp
void pcsg_time_sync(POCSAGPagerApp* app) {
    if(app->filter.time_sync == PCSGTimeSyncOff ||
       app->txrx->txrx_state != PCSGTxRxStateRx) return;
    if(strncmp(furi_string_get_cstr(app->txrx->preset->name), "FLEX", 4) != 0) return;

    FlexFrameTiming timing;
    if(!flex_sampler_get_frame_timing(app->txrx->flex_sampler, &timing) ||
       timing.sequence == app->time_sync_sequence) return;
    app->time_sync_sequence = timing.sequence;
    if(timing.chase) return;
    const uint32_t age_ticks = furi_get_tick() - timing.tick;
    if(age_ticks > furi_ms_to_ticks(PCSG_TIME_SYNC_MAX_AGE_MS)) return;

    const bool full = app->filter.time_sync == PCSGTimeSyncFull && timing.clock_valid;
    const uint32_t age_ms = (uint64_t)age_ticks * 1000U / furi_kernel_get_tick_frequency();
    // FIW publication marks this frame's start; elapsed time includes reception.
    const uint32_t gps_seconds =
        ((uint32_t)timing.cycle * 240000U + (uint32_t)timing.frame * 1875U + age_ms + 500U) / 1000U;
    const uint32_t gps_second = gps_seconds % 3600U;
    const int32_t second = (gps_second + 3600U - PCSG_GPS_UTC_OFFSET_S) % 3600U;

    DateTime target;
    furi_hal_rtc_get_datetime(&target);
    int32_t adjustment = second - ((int32_t)target.minute * 60 + target.second);
    if(full) {
        // BIW hour uses daylight time year-round; set L0 means standard time.
        const uint8_t hour = (timing.hour + gps_seconds / 3600U + 24U -
                              timing.standard_time - (gps_second < PCSG_GPS_UTC_OFFSET_S)) % 24U;
        adjustment += ((int32_t)hour - target.hour) * 3600;
        target.hour = hour;
    } else {
        if(adjustment > 1800) adjustment -= 3600;
        if(adjustment < -1800) adjustment += 3600;
    }
    if(adjustment > -PCSG_TIME_SYNC_MIN_ADJUST_S &&
       adjustment < PCSG_TIME_SYNC_MIN_ADJUST_S) return;
    if(full) {
        target.minute = second / 60;
        target.second = second % 60;
    } else {
        // Preserve the existing MM:SS adjustment, including hour/date carry.
        const int64_t stamp = (int64_t)datetime_datetime_to_timestamp(&target) + adjustment;
        if(stamp < 0 || stamp > UINT32_MAX) return;
        datetime_timestamp_to_datetime((uint32_t)stamp, &target);
    }
    furi_hal_rtc_set_datetime(&target);
}

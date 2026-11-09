#include "pocsag_pager_app_i.h"

#define PCSG_CLASSIC_ALERT_INTERVAL_MS 5000U
#define PCSG_CLASSIC_ALERT_ON_MS 875U
#define PCSG_CLASSIC_ALERT_OFF_MS 125U
#define PCSG_CLASSIC_ALERT_BEEPS 3U

// Measured from the user's reference: https://youtu.be/IVyItS3sZLo
static const NotificationMessage pcsg_classic_note = {
    .type = NotificationMessageTypeSoundOn,
    .data.sound = {.frequency = 3200.0f, .volume = 1.0f},
};

static const NotificationSequence pcsg_classic_beep = {
    &pcsg_classic_note,
    &message_do_not_reset,
    NULL,
};

static const NotificationSequence pcsg_classic_beep_vibro = {
    &message_vibro_on,
    &pcsg_classic_note,
    &message_delay_50,
    &message_vibro_off,
    &message_do_not_reset,
    NULL,
};

static bool pcsg_classic_alert_enabled(const POCSAGPagerApp* app) {
    return app->filter.alert_tone == PCSGAlertToneClassic &&
           (app->filter.alert == PCSGAlertSound || app->filter.alert == PCSGAlertBoth);
}

// Receive callbacks only set a flag; playback runs on the UI event loop.
bool pcsg_classic_alert_request(POCSAGPagerApp* app) {
    if(!pcsg_classic_alert_enabled(app)) return false;
    atomic_store(&app->classic_alert_pending, true);
    return true;
}

void pcsg_classic_alert_reset(POCSAGPagerApp* app) {
    atomic_store(&app->classic_alert_pending, false);
    if(app->classic_alert_remaining) furi_event_loop_timer_stop(app->classic_alert_timer);
    if(app->classic_alert_sounding) {
        notification_message(app->notifications, &sequence_reset_sound);
        app->classic_alert_sounding = false;
    }
    app->classic_alert_remaining = 0;
}

void pcsg_classic_alert_free(POCSAGPagerApp* app) {
    pcsg_classic_alert_reset(app);
    if(app->classic_alert_timer) {
        furi_event_loop_timer_free(app->classic_alert_timer);
        app->classic_alert_timer = NULL;
        // Drain queued pointers to this FAP before its code/data are unloaded.
        notification_message_block(app->notifications, &sequence_reset_sound);
    }
}

static void pcsg_classic_alert_step(void* context) {
    POCSAGPagerApp* app = context;
    if(app->txrx->txrx_state != PCSGTxRxStateRx || !pcsg_classic_alert_enabled(app)) {
        pcsg_classic_alert_reset(app);
        return;
    }
    if(!app->classic_alert_remaining) return;
    if(app->classic_alert_sounding) {
        notification_message(app->notifications, &sequence_reset_sound);
        app->classic_alert_sounding = false;
        if(--app->classic_alert_remaining) {
            furi_event_loop_timer_start(
                app->classic_alert_timer, furi_ms_to_ticks(PCSG_CLASSIC_ALERT_OFF_MS));
        }
    } else {
        const bool vibrate = app->filter.alert == PCSGAlertBoth &&
                             app->classic_alert_remaining == PCSG_CLASSIC_ALERT_BEEPS;
        const NotificationSequence* sequence = &pcsg_classic_beep;
        if(vibrate) sequence = &pcsg_classic_beep_vibro;
        notification_message(app->notifications, sequence);
        app->classic_alert_sounding = true;
        furi_event_loop_timer_start(
            app->classic_alert_timer, furi_ms_to_ticks(PCSG_CLASSIC_ALERT_ON_MS));
    }
}

void pcsg_classic_alert_tick(POCSAGPagerApp* app) {
    if(app->txrx->txrx_state != PCSGTxRxStateRx || !pcsg_classic_alert_enabled(app)) {
        pcsg_classic_alert_reset(app);
        return;
    }
    const uint32_t now = furi_get_tick();
    // Consume requests during playback/cooldown, without replaying a backlog.
    if(!atomic_exchange(&app->classic_alert_pending, false) || app->classic_alert_remaining ||
       (app->classic_alert_started &&
        now - app->classic_alert_tick < furi_ms_to_ticks(PCSG_CLASSIC_ALERT_INTERVAL_MS))) {
        return;
    }
    if(!app->classic_alert_timer) {
        app->classic_alert_timer = furi_event_loop_timer_alloc(
            view_dispatcher_get_event_loop(app->view_dispatcher),
            pcsg_classic_alert_step,
            FuriEventLoopTimerTypeOnce,
            app);
    }
    app->classic_alert_tick = now;
    app->classic_alert_started = true;
    app->classic_alert_remaining = PCSG_CLASSIC_ALERT_BEEPS;
    pcsg_classic_alert_step(app);
}

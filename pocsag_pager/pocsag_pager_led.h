#pragma once

#include "pocsag_pager_settings.h"
#include <notification/notification_messages.h>

/** Persistent notification data, safe for the notification service's queue. */
const NotificationSequence* pcsg_led_blink_sequence(PCSGLedColor color);

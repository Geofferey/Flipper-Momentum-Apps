#include "pocsag_pager_led.h"

// Same 10 ms notification pattern as the firmware's notification_messages.c.
// Mixed shades set all three channels; the service applies LED brightness
// and restores the notification layer after each sequence.
#define PCSG_LED_MIX(name, red, green, blue)                                       \
    static const NotificationMessage pcsg_led_##name##_rgb[] = {                  \
        {.type = NotificationMessageTypeLedRed, .data.led.value = red},           \
        {.type = NotificationMessageTypeLedGreen, .data.led.value = green},       \
        {.type = NotificationMessageTypeLedBlue, .data.led.value = blue},         \
    };                                                                          \
    static const NotificationSequence pcsg_led_##name = {                        \
        &pcsg_led_##name##_rgb[0],                                               \
        &pcsg_led_##name##_rgb[1],                                               \
        &pcsg_led_##name##_rgb[2],                                               \
        &message_delay_10,                                                      \
        NULL,                                                                   \
    }

PCSG_LED_MIX(white, 255, 255, 255);
PCSG_LED_MIX(amber, 255, 96, 0);
PCSG_LED_MIX(lime, 128, 255, 0);
PCSG_LED_MIX(teal, 0, 255, 128);
PCSG_LED_MIX(sky, 0, 128, 255);
PCSG_LED_MIX(violet, 128, 0, 255);
PCSG_LED_MIX(pink, 255, 64, 128);
PCSG_LED_MIX(warm, 255, 160, 80);

#undef PCSG_LED_MIX

static const NotificationSequence* const pcsg_led_sequences[PCSGLedColorCount] = {
    [PCSGLedColorCyan] = &sequence_blink_cyan_10,
    [PCSGLedColorBlue] = &sequence_blink_blue_10,
    [PCSGLedColorGreen] = &sequence_blink_green_10,
    [PCSGLedColorYellow] = &sequence_blink_yellow_10,
    [PCSGLedColorMagenta] = &sequence_blink_magenta_10,
    [PCSGLedColorRed] = &sequence_blink_red_10,
    [PCSGLedColorWhite] = &pcsg_led_white,
    [PCSGLedColorAmber] = &pcsg_led_amber,
    [PCSGLedColorLime] = &pcsg_led_lime,
    [PCSGLedColorTeal] = &pcsg_led_teal,
    [PCSGLedColorSky] = &pcsg_led_sky,
    [PCSGLedColorViolet] = &pcsg_led_violet,
    [PCSGLedColorPink] = &pcsg_led_pink,
    [PCSGLedColorWarm] = &pcsg_led_warm,
};

const NotificationSequence* pcsg_led_blink_sequence(PCSGLedColor color) {
    if((unsigned)color >= PCSGLedColorCount) color = PCSGLedColorCyan;
    return pcsg_led_sequences[color];
}

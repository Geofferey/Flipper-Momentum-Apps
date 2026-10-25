#pragma once

typedef enum {
    //PCSGCustomEvent
    PCSGCustomEventStartId = 100,

    PCSGCustomEventSceneSettingLock,
    PCSGCustomEventSceneSettingCapcodeMin,
    PCSGCustomEventSceneSettingCapcodeMax,
    PCSGCustomEventSceneCapcodeInputDone,

    PCSGCustomEventViewReceiverOK,
    PCSGCustomEventViewReceiverConfig,
    PCSGCustomEventViewReceiverBack,
    PCSGCustomEventViewReceiverOffDisplay,
    PCSGCustomEventViewReceiverUnlock,
} PCSGCustomEvent;

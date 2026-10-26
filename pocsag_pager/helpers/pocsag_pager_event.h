#pragma once

typedef enum {
    //PCSGCustomEvent
    PCSGCustomEventStartId = 100,

    PCSGCustomEventSceneSettingLock,
    PCSGCustomEventSceneSettingCapcodeMin,
    PCSGCustomEventSceneSettingCapcodeMax,
    PCSGCustomEventSceneSettingFrameMin,
    PCSGCustomEventSceneSettingFrameMax,
    PCSGCustomEventSceneSettingRicMin,
    PCSGCustomEventSceneSettingRicMax,
    PCSGCustomEventSceneSettingFreqPpm,
    PCSGCustomEventSceneCapcodeInputDone,
    PCSGCustomEventTxRicInputDone,
    PCSGCustomEventTxMessageInputDone,
    PCSGCustomEventTxSend,

    PCSGCustomEventViewReceiverOK,
    PCSGCustomEventViewReceiverConfig,
    PCSGCustomEventViewReceiverBack,
    PCSGCustomEventViewReceiverOffDisplay,
    PCSGCustomEventViewReceiverUnlock,
} PCSGCustomEvent;

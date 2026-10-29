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
    PCSGCustomEventViewReceiverFilterAddress,
    PCSGCustomEventViewReceiverConfig,
    PCSGCustomEventViewReceiverBack,
    PCSGCustomEventViewReceiverOffDisplay,
    PCSGCustomEventViewReceiverUnlock,
} PCSGCustomEvent;

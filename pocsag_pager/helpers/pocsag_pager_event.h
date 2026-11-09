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

    /** Scrolled past the end of the open message: show the next one. */
    PCSGCustomEventViewInfoNextMessage,
    /** Scrolled back past its start: show the previous one, at its end. */
    PCSGCustomEventViewInfoPrevMessage,
} PCSGCustomEvent;

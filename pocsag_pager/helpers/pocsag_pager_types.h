#pragma once

#include <furi.h>
#include <furi_hal.h>

#define PCSG_VERSION_APP FAP_VERSION
// FLEX references: multimon-ng demod_flex.c (file header credits) and
// PortaPack Mayhem FLEX RX/TX (git history; the files have no headers).
#define PCSG_DEVELOPED                                                     \
    "@xMasterX & @Shmuma\nProtocol improvements by:\n@htotoo\n"            \
    "FLEX decoding by:\n@Geofferey\n"                                      \
    "Based on multimon-ng FLEX:\nCraig Shelley, Bruce Quinton,\n"          \
    "Jason Lingohr & contributors\n"                                       \
    "and Mayhem FLEX:\n@timelf123, @VasylSamoilov\n"                       \
    "Icons by:\n@Svaarich"
#define PCSG_GITHUB "https://github.com/xMasterX/flipper-pager"

#define PCSG_KEY_FILE_VERSION 1
#define PCSG_KEY_FILE_TYPE "Flipper POCSAG Pager Key File"

/** PCSGRxKeyState state */
typedef enum {
    PCSGRxKeyStateIDLE,
    PCSGRxKeyStateBack,
    PCSGRxKeyStateStart,
    PCSGRxKeyStateAddKey,
} PCSGRxKeyState;

/** PCSGHopperState state */
typedef enum {
    PCSGHopperStateOFF,
    PCSGHopperStateRunnig,
    PCSGHopperStatePause,
    PCSGHopperStateRSSITimeOut,
} PCSGHopperState;

/** PCSGLock */
typedef enum {
    PCSGLockOff,
    PCSGLockOn,
} PCSGLock;

typedef enum {
    POCSAGPagerViewVariableItemList,
    POCSAGPagerViewSubmenu,
    POCSAGPagerViewReceiver,
    POCSAGPagerViewReceiverInfo,
    POCSAGPagerViewWidget,
    POCSAGPagerViewNumberInput,
    POCSAGPagerViewTextInput,
} POCSAGPagerView;

/** POCSAGPagerTxRx state */
typedef enum {
    PCSGTxRxStateIDLE,
    PCSGTxRxStateRx,
    PCSGTxRxStateTx,
    PCSGTxRxStateSleep,
} PCSGTxRxState;

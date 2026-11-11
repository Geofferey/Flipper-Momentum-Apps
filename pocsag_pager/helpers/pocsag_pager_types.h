#pragma once

#include <furi.h>
#include <furi_hal.h>

#define PCSG_VERSION_APP FAP_VERSION
/*
 * FLEX references: multimon-ng demod_flex.c (file header credits) and
 * PortaPack Mayhem FLEX RX/TX (git history; the files have no headers).
 *
 * The credits the About screen shows are in ABOUT.txt beside this app,
 * installed to /ext/pocsag/ABOUT.txt, with a shortened copy compiled into
 * pocsag_pager_scene_about.c for when that file is absent.
 */

#define PCSG_KEY_FILE_VERSION 1
#define PCSG_KEY_FILE_TYPE "Flipper POCSAG Pager Key File"

/** PCSGRxKeyState state */
typedef enum {
    PCSGRxKeyStateIDLE,
    PCSGRxKeyStateBack,
    PCSGRxKeyStateStart,
    PCSGRxKeyStateAddKey,
} PCSGRxKeyState;

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

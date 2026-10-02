#include <stdint.h>
#include <stdbool.h>
#include <orbis/_types/pad.h>
#include <orbis/_types/user.h>
#include <orbis/_types/pthread.h>

#include "common_data.h"

#ifndef REMOTE_PAD_PAD_H
#define REMOTE_PAD_PAD_H

#define ORBIS_HID_ERROR_ALREADY_LOGGED_OUT 0x803B0101
#define REMOTE_PAD_MAX_PADS 4
// Samples a pad keeps for scePadRead (one slot stays free, so 15 can wait). Stick-only updates replace the
// newest unread sample instead of queueing, so only button and touch changes add up here.
#define REMOTE_PAD_MAX_HISTORY 16

typedef struct {
    int32_t deviceClass;
    uint8_t reserved[4];
    uint8_t classData[12];
} OrbisPadDeviceClassExtInfo;

typedef struct {
    int32_t deviceClass;
    bool bDataValid;
    uint8_t classData[16];
} OrbisPadDeviceClassData;

void emptyPadInfo(OrbisPadInformation *info);

void emptyPadData(OrbisPadData *data);

void initPadData(size_t index);

void resetPadData(size_t index);

void pushPadData(size_t index, OrbisPadData *data);

void getLatestPadData(size_t index, OrbisPadData *data);

int32_t getPadData(size_t index, OrbisPadData *data, int32_t count);

// Whether the game is told that this pad's controller is connected
bool isPadConnected(size_t index);

typedef struct RemotePad RemotePad;
typedef const struct RemotePadDriver *RemotePadDriverPtr;
typedef struct RemotePadDriver {
    int32_t (*init)(RemotePadDriverPtr driver);

    int32_t (*term)(RemotePadDriverPtr driver);

    int32_t (*setLightBar)(RemotePad *pad, OrbisPadColor *inputColor);

    int32_t (*resetLightBar)(RemotePad *pad);

    int32_t (*setVibration)(RemotePad *pad, const OrbisPadVibeParam *param);

    int32_t (*resetOrientation)(RemotePad *pad);

    int32_t (*setMotionSensorState)(RemotePad *pad, bool enable);

    int32_t (*setTiltCorrectionState)(RemotePad *pad, bool enable);

    int32_t (*setAngularVelocityDeadbandState)(RemotePad *pad, bool enable);

    int32_t (*getControllerInformation)(RemotePad *pad, OrbisPadInformation *info);

    int32_t (*deviceClassParseData)(RemotePad *pad, const OrbisPadData *data, OrbisPadDeviceClassData *classData);

    int32_t (*deviceClassGetExtInfo)(RemotePad *pad, OrbisPadDeviceClassExtInfo *info);

    int32_t (*read)(RemotePad *pad, OrbisPadData *data, int32_t count);

    int32_t (*readState)(RemotePad *pad, OrbisPadData *data);

    int32_t (*close)(RemotePad *pad);

    // Called after a pad was opened/closed by the game or the remote users changed (optional)
    void (*statusChanged)(RemotePadDriverPtr driver);

    // Driver specific global data
    void *data;

    const char *name;
} RemotePadDriver;

typedef struct RemotePad {
    int32_t index;
    int32_t handle;
    int32_t userId;

    const RemotePadDriver *driver;
    circularBuf *padData;

    // A phone/PC is driving this pad. Without one the game sees a disconnected controller.
    bool deviceConnected;
    // Reported to the game as the controller's connection count (increases on every reconnect)
    uint8_t connectCount;
    // Share mode: the real controller handle this pad's input is added to (-1 = none)
    int32_t sharedHandle;
} RemotePad;

typedef struct RemotePadService {
    int32_t (*init)(void);

    int32_t (*term)(void);

    int32_t (*initDriver)(void);

    int32_t (*getPad)(int32_t handle, RemotePad **padPtr);

    int32_t (*setLightBar)(int32_t handle, OrbisPadColor *inputColor);

    int32_t (*resetLightBar)(int32_t handle);

    int32_t (*setVibration)(int32_t handle, const OrbisPadVibeParam *param);

    int32_t (*resetOrientation)(int32_t handle);

    int32_t (*setMotionSensorState)(int32_t handle, bool enable);

    int32_t (*setTiltCorrectionState)(int32_t handle, bool enable);

    int32_t (*setAngularVelocityDeadbandState)(int32_t handle, bool enable);

    int32_t (*getControllerInformation)(int32_t handle, OrbisPadInformation *info);

    int32_t (*deviceClassParseData)(int32_t handle, const OrbisPadData *data, OrbisPadDeviceClassData *classData);

    int32_t (*deviceClassGetExtInfo)(int32_t handle, OrbisPadDeviceClassExtInfo *info);

    int32_t (*read)(int32_t handle, OrbisPadData *data, int32_t count);

    int32_t (*readState)(int32_t handle, OrbisPadData *data);

    int32_t (*getHandle)(int32_t userId, uint32_t controller_type, uint32_t controller_index);

    int32_t (*open)(int32_t userId, int32_t type, int32_t index, void *param);

    int32_t (*close)(int32_t handle);

    // Tell all drivers that the pad/user status changed
    void (*notifyStatus)(void);

    // A driver reports whether a device is driving the pad with this index
    void (*setDeviceConnected)(int32_t index, bool connected);

    // Share mode: the game opened a real controller (handle) for the user of this pad. Reads of that
    // controller get this pad's input added, its vibration and lightbar also go to this pad's devices.
    void (*share)(int32_t index, int32_t realHandle);

    void (*unshare)(int32_t realHandle);

    // Add the input of the pad sharing this real controller to samples read from it (no-op otherwise)
    void (*mergeShared)(int32_t realHandle, OrbisPadData *data, int32_t count);

    // Output the game sends to a shared real controller, for the pad's devices too (no-op otherwise)
    void (*shareVibration)(int32_t realHandle, const OrbisPadVibeParam *param);

    void (*shareLightBar)(int32_t realHandle, OrbisPadColor *color);

    void (*shareResetLightBar)(int32_t realHandle);

    RemotePad pads[REMOTE_PAD_MAX_PADS];
    OrbisPthreadMutex padMutex;
    OrbisPthreadMutex dataMutex;
    // remote_pad.ini always_connected: pads look connected even without a device (the old behavior)
    bool alwaysConnected;
} RemotePadService;

RemotePadService *initRemotePadService(void);

RemotePadService *getRemotePadService(void);

void termRemotePadService(RemotePadService *);

#endif //REMOTE_PAD_PAD_H

#include <stdio.h>
#include <orbis/_types/errors.h>
#include <orbis/libkernel.h>
#include "pad.h"
#include "utils.h"

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

extern RemotePadService rps;

extern const RemotePadDriver dummyDriver;
extern const RemotePadDriver wsDriver;
extern const RemotePadDriver usbDriver;

static const RemotePadDriver *const padDrivers[] = {&dummyDriver, &wsDriver};

#define GET_PAD(handle) \
    RemotePad *pad = NULL; \
    scePthreadMutexLock(&rps.padMutex); \
    int ret = rps.getPad(handle, &pad); \
    if (ret != 0) { \
        scePthreadMutexUnlock(&rps.padMutex); \
        return ret; \
    }

#define CALL_FUNCTION(func, ...) \
    if (pad->driver->func) { \
        ret = pad->driver->func(pad, ## __VA_ARGS__); \
    } else { \
        ret = dummyDriver.func(pad, ## __VA_ARGS__); \
    }

#define RETURN_CODE() \
    scePthreadMutexUnlock(&rps.padMutex); \
    return ret;

#define PAD_FUNC_DEF(...) \
    GET_PAD(handle) \
    CALL_FUNCTION(__VA_ARGS__) \
    RETURN_CODE()

static int32_t init(void) {
    int ret;

    ret = scePthreadMutexInit(&rps.padMutex, 0, "padMtx");
    if (ret < 0) {
        final_printf("[RemotePad]: failed to init the pad mutex, 0x%X\n", ret);
        return -1;
    }

    ret = scePthreadMutexInit(&rps.dataMutex, 0, "dataMtx");
    if (ret < 0) {
        final_printf("[RemotePad]: failed to init the data mutex, 0x%X\n", ret);
        return -1;
    }

    rps.alwaysConnected = false;

    for (int i = 0; i < REMOTE_PAD_MAX_PADS; i++) {
        rps.pads[i].index = i;
        rps.pads[i].handle = 1000 + i;
        rps.pads[i].userId = 0;
        rps.pads[i].driver = &dummyDriver;
        rps.pads[i].deviceConnected = false;
        rps.pads[i].connectCount = 0;
        rps.pads[i].sharedHandle = -1;
        rps.pads[i].specialOpen = false;
        initPadData(i);
        if (rps.pads[i].padData == NULL) {
            final_printf("[RemotePad]: failed to allocate the data buffer of pad %d\n", i);
            return -1;
        }
    }
    return 0;
}

static void notifyStatus(void) {
    for (size_t i = 0; i < ARRAY_SIZE(padDrivers); i++) {
        if (padDrivers[i]->statusChanged)
            padDrivers[i]->statusChanged(padDrivers[i]);
    }
}

static void setDeviceConnected(int32_t index, bool connected) {
    if (index < 0 || index >= REMOTE_PAD_MAX_PADS)
        return;
    scePthreadMutexLock(&rps.padMutex);
    RemotePad *pad = &rps.pads[index];
    bool changed = pad->deviceConnected != connected;
    if (changed) {
        pad->deviceConnected = connected;
        if (connected && ++pad->connectCount == 0)
            pad->connectCount = 1; // 0 would read as "never connected"
        else if (!connected)
            resetPadData(index); // a controller that goes away takes its input with it
    }
    scePthreadMutexUnlock(&rps.padMutex);
    if (changed)
        final_printf("[RemotePad] pad %d: device %s\n", index + 1, connected ? "connected" : "disconnected");
}

static int32_t sharedIndex(int32_t realHandle) {
    for (int32_t i = 0; i < REMOTE_PAD_MAX_PADS; i++) {
        if (rps.pads[i].sharedHandle >= 0 && rps.pads[i].sharedHandle == realHandle)
            return i;
    }
    return -1;
}

static void share(int32_t index, int32_t realHandle) {
    if (index < 0 || index >= REMOTE_PAD_MAX_PADS || realHandle < 0)
        return;
    scePthreadMutexLock(&rps.padMutex);
    rps.pads[index].sharedHandle = realHandle;
    rps.pads[index].driver = &wsDriver;
    scePthreadMutexUnlock(&rps.padMutex);
    final_printf("[RemotePad] pad %d is added to the real controller (handle %d)\n", index + 1, realHandle);
    notifyStatus();
}

static void unshare(int32_t realHandle) {
    scePthreadMutexLock(&rps.padMutex);
    int32_t index = sharedIndex(realHandle);
    if (index >= 0)
        rps.pads[index].sharedHandle = -1;
    scePthreadMutexUnlock(&rps.padMutex);
    if (index >= 0) {
        final_printf("[RemotePad] pad %d is no longer added to the real controller\n", index + 1);
        notifyStatus();
    }
}

static inline uint8_t fartherFromCenter(uint8_t a, uint8_t b) {
    return abs((int) b - 128) > abs((int) a - 128) ? b : a;
}

static void mergeShared(int32_t realHandle, OrbisPadData *data, int32_t count) {
    // Called for every read of every real controller: return early without locking
    int32_t index = sharedIndex(realHandle);
    if (index < 0 || count <= 0 || !rps.pads[index].deviceConnected)
        return;
    OrbisPadData remote;
    scePthreadMutexLock(&rps.dataMutex);
    getLatestData(rps.pads[index].padData, &remote);
    scePthreadMutexUnlock(&rps.dataMutex);
    for (int32_t i = 0; i < count; i++) {
        OrbisPadData *sample = &data[i];
        sample->buttons |= remote.buttons;
        sample->leftStick.x = fartherFromCenter(sample->leftStick.x, remote.leftStick.x);
        sample->leftStick.y = fartherFromCenter(sample->leftStick.y, remote.leftStick.y);
        sample->rightStick.x = fartherFromCenter(sample->rightStick.x, remote.rightStick.x);
        sample->rightStick.y = fartherFromCenter(sample->rightStick.y, remote.rightStick.y);
        if (remote.analogButtons.l2 > sample->analogButtons.l2)
            sample->analogButtons.l2 = remote.analogButtons.l2;
        if (remote.analogButtons.r2 > sample->analogButtons.r2)
            sample->analogButtons.r2 = remote.analogButtons.r2;
        if (remote.touch.fingers > 0)
            sample->touch = remote.touch;
        // The phone keeps the player connected even when the DualShock is off
        sample->connected = 1;
    }
}

static void shareVibration(int32_t realHandle, const OrbisPadVibeParam *param) {
    scePthreadMutexLock(&rps.padMutex);
    int32_t index = sharedIndex(realHandle);
    if (index >= 0 && rps.pads[index].driver->setVibration)
        rps.pads[index].driver->setVibration(&rps.pads[index], param);
    scePthreadMutexUnlock(&rps.padMutex);
}

static void shareLightBar(int32_t realHandle, OrbisPadColor *color) {
    scePthreadMutexLock(&rps.padMutex);
    int32_t index = sharedIndex(realHandle);
    if (index >= 0 && rps.pads[index].driver->setLightBar)
        rps.pads[index].driver->setLightBar(&rps.pads[index], color);
    scePthreadMutexUnlock(&rps.padMutex);
}

static void shareResetLightBar(int32_t realHandle) {
    scePthreadMutexLock(&rps.padMutex);
    int32_t index = sharedIndex(realHandle);
    if (index >= 0 && rps.pads[index].driver->resetLightBar)
        rps.pads[index].driver->resetLightBar(&rps.pads[index]);
    scePthreadMutexUnlock(&rps.padMutex);
}

// What the game is told about the controller: connected only while a device drives it (unless always_connected)
static void stampConnection(const RemotePad *pad, OrbisPadData *data, int32_t count) {
    bool connected = rps.alwaysConnected || pad->deviceConnected;
    uint8_t connectCount = pad->connectCount;
    if (rps.alwaysConnected && connectCount == 0)
        connectCount = 1;
    for (int32_t i = 0; i < count; i++) {
        data[i].connected = connected ? 1 : 0;
        data[i].count = connectCount;
    }
}

static int32_t term(void) {
    // Stop the drivers first: their threads push pad data and use the mutexes destroyed below
    for (size_t i = 0; i < ARRAY_SIZE(padDrivers); i++) {
        if (padDrivers[i]->term)
            padDrivers[i]->term(padDrivers[i]);
    }

    scePthreadMutexLock(&rps.padMutex);
    for (int i = 0; i < REMOTE_PAD_MAX_PADS; i++) {
        RemotePad *pad = &rps.pads[i];
        if (pad->userId != 0) {
            int ret;
            CALL_FUNCTION(close)
            (void) ret;
            pad->userId = 0;
        }
        termData(pad->padData);
        pad->padData = NULL;
        pad->sharedHandle = -1;
        pad->specialOpen = false;
    }
    scePthreadMutexUnlock(&rps.padMutex);
    scePthreadMutexDestroy(&rps.padMutex);
    scePthreadMutexDestroy(&rps.dataMutex);
    return 0;
}

static int32_t initDriver() {
    for (size_t i = 0; i < ARRAY_SIZE(padDrivers); i++) {
        if (padDrivers[i]->init(padDrivers[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

static int32_t getPad(int32_t handle, RemotePad **padPtr) {
    RemotePad *pad = NULL;
    if (padPtr == NULL)
        return ORBIS_PAD_ERROR_INVALID_ARG;
    for (int i = 0; i < REMOTE_PAD_MAX_PADS; i++) {
        if (rps.pads[i].handle == handle) {
            pad = &rps.pads[i];
            if (pad->userId == 0)
                return ORBIS_HID_ERROR_ALREADY_LOGGED_OUT;
            break;
        }
        if (rps.pads[i].handle + REMOTE_PAD_SPECIAL_HANDLE_OFFSET == handle) {
            // The special port reads and answers like the pad itself
            pad = &rps.pads[i];
            if (!pad->specialOpen)
                return ORBIS_HID_ERROR_ALREADY_LOGGED_OUT;
            break;
        }
    }
    if (pad == NULL)
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    *padPtr = pad;
    return 0;
}

// A NULL buffer is refused before anything is touched; for a real controller the hook then hands the call to
// the system, which reports its own error.
#define CHECK_ARG(ptr) \
    if ((ptr) == NULL) \
        return ORBIS_PAD_ERROR_INVALID_ARG;

static int32_t padSetLightBar(int32_t handle, OrbisPadColor *inputColor) {
    CHECK_ARG(inputColor)
    PAD_FUNC_DEF(setLightBar, inputColor)
}

static int32_t padResetLightBar(int32_t handle) {
    PAD_FUNC_DEF(resetLightBar)
}

static int32_t padSetVibration(int32_t handle, const OrbisPadVibeParam *param) {
    CHECK_ARG(param)
    PAD_FUNC_DEF(setVibration, param)
}

static int32_t padResetOrientation(int32_t handle) {
    PAD_FUNC_DEF(resetOrientation)
}

static int32_t padSetMotionSensorState(int32_t handle, bool enable) {
    PAD_FUNC_DEF(setMotionSensorState, enable)
}

static int32_t padSetTiltCorrectionState(int32_t handle, bool enable) {
    PAD_FUNC_DEF(setTiltCorrectionState, enable)
}

static int32_t padSetAngularVelocityDeadbandState(int32_t handle, bool enable) {
    PAD_FUNC_DEF(setAngularVelocityDeadbandState, enable)
}

static int32_t padGetControllerInformation(int32_t handle, OrbisPadInformation *info) {
    CHECK_ARG(info)
    PAD_FUNC_DEF(getControllerInformation, info)
}

static int32_t padDeviceClassParseData(int32_t handle, const OrbisPadData *data, OrbisPadDeviceClassData *classData) {
    CHECK_ARG(classData)
    PAD_FUNC_DEF(deviceClassParseData, data, classData)
}

static int32_t padDeviceClassGetExtInfo(int32_t handle, OrbisPadDeviceClassExtInfo *info) {
    CHECK_ARG(info)
    PAD_FUNC_DEF(deviceClassGetExtInfo, info)
}

static int32_t padRead(int32_t handle, OrbisPadData *data, int32_t count) {
    CHECK_ARG(data)
    PAD_FUNC_DEF(read, data, count)
}

static int32_t padReadState(int32_t handle, OrbisPadData *data) {
    CHECK_ARG(data)
    PAD_FUNC_DEF(readState, data)
}

static int32_t padGetHandle(int32_t userId, uint32_t controller_type, uint32_t controller_index) {
    int32_t handle = 0;
    (void) controller_index;
    scePthreadMutexLock(&rps.padMutex);
    for (int i = 0; i < REMOTE_PAD_MAX_PADS; i++) {
        if (rps.pads[i].userId == userId) {
            if (controller_type != ORBIS_PAD_PORT_TYPE_SPECIAL)
                handle = rps.pads[i].handle;
            else if (rps.pads[i].specialOpen)
                handle = rps.pads[i].handle + REMOTE_PAD_SPECIAL_HANDLE_OFFSET;
            break;
        }
    }
    scePthreadMutexUnlock(&rps.padMutex);
    if (handle != 0)
        return handle;
    return ORBIS_PAD_ERROR_DEVICE_NO_HANDLE;
}

// Unity opens a player's special port right after the standard one, compares the two controllers and closes
// one of them. If the special port can't be opened, the player gets no controller at all ("Sign in" in
// Tricky Towers). A remote pad has no special device: its special port is the same pad.
static int32_t openSpecialPort(int32_t userId, int32_t index) {
    scePthreadMutexLock(&rps.padMutex);
    RemotePad *pad = &rps.pads[index];
    if (pad->specialOpen || (pad->userId != 0 && pad->userId != userId)) {
        scePthreadMutexUnlock(&rps.padMutex);
        return ORBIS_PAD_ERROR_ALREADY_OPENED;
    }
    pad->specialOpen = true;
    pad->driver = &wsDriver;
    int32_t handle = pad->handle + REMOTE_PAD_SPECIAL_HANDLE_OFFSET;
    scePthreadMutexUnlock(&rps.padMutex);

    final_printf("[RemotePad] pad %d: special port opened for user 0x%08X (handle %d)\n", index + 1, userId, handle);
    return handle;
}

static int32_t padOpen(int32_t userId, int32_t type, int32_t index, void *param) {
    int32_t handle;
    (void) param;
    if (index < 0 || index >= REMOTE_PAD_MAX_PADS)
        return ORBIS_PAD_ERROR_INVALID_ARG;
    if (type == ORBIS_PAD_PORT_TYPE_SPECIAL)
        return openSpecialPort(userId, index);
    if (type != ORBIS_PAD_PORT_TYPE_STANDARD)
        return ORBIS_PAD_ERROR_INVALID_PORT; // the remote control port: a remote pad has none
    scePthreadMutexLock(&rps.padMutex);
    for (int i = 0; i < REMOTE_PAD_MAX_PADS; i++) {
        if (rps.pads[i].userId == userId) {
            scePthreadMutexUnlock(&rps.padMutex);
            return ORBIS_PAD_ERROR_ALREADY_OPENED;
        }
    }
    if (rps.pads[index].userId != 0) {
        // Every remote user owns the pad with its own index, so this means two users share an index
        scePthreadMutexUnlock(&rps.padMutex);
        final_printf("[RemotePad] pad %d is already used by user 0x%08X\n", index + 1, rps.pads[index].userId);
        return ORBIS_PAD_ERROR_ALREADY_OPENED;
    }

    // TODO: Choose driver based on configuration file
    rps.pads[index].userId = userId;
    rps.pads[index].driver = &wsDriver;
    handle = rps.pads[index].handle;
    // Don't replay input that arrived before the game opened this pad
    resetPadData(index);
    scePthreadMutexUnlock(&rps.padMutex);

    final_printf("[RemotePad] pad %d opened for user 0x%08X (handle %d)\n", index + 1, userId, handle);
    notifyStatus();
    return handle;
}

static int32_t padClose(int32_t handle) {
    GET_PAD(handle)
    if (handle != pad->handle) {
        // The special port: the pad itself stays open
        pad->specialOpen = false;
        scePthreadMutexUnlock(&rps.padMutex);
        final_printf("[RemotePad] pad %d: special port closed\n", pad->index + 1);
        return SCE_OK;
    }
    CALL_FUNCTION(close)
    if (ret == 0) {
        final_printf("[RemotePad] pad %d closed (user 0x%08X)\n", pad->index + 1, pad->userId);
        pad->userId = 0;
        resetPadData(pad->index);
    }
    scePthreadMutexUnlock(&rps.padMutex);
    if (ret == 0)
        notifyStatus();
    return ret;
}

RemotePadService rps = {
        .init = init,
        .term = term,
        .initDriver = initDriver,
        .getPad = getPad,
        .setLightBar = padSetLightBar,
        .resetLightBar = padResetLightBar,
        .setVibration = padSetVibration,
        .resetOrientation = padResetOrientation,
        .setMotionSensorState = padSetMotionSensorState,
        .setTiltCorrectionState = padSetTiltCorrectionState,
        .setAngularVelocityDeadbandState = padSetAngularVelocityDeadbandState,
        .getControllerInformation = padGetControllerInformation,
        .deviceClassParseData = padDeviceClassParseData,
        .deviceClassGetExtInfo = padDeviceClassGetExtInfo,
        .read = padRead,
        .readState = padReadState,
        .getHandle = padGetHandle,
        .open = padOpen,
        .close = padClose,
        .notifyStatus = notifyStatus,
        .setDeviceConnected = setDeviceConnected,
        .share = share,
        .unshare = unshare,
        .mergeShared = mergeShared,
        .shareVibration = shareVibration,
        .shareLightBar = shareLightBar,
        .shareResetLightBar = shareResetLightBar,
};

RemotePadService *initRemotePadService(void) {
    if (rps.init() != 0)
        return NULL;
    return &rps;
}

RemotePadService *getRemotePadService(void) {
    return &rps;
}

void termRemotePadService(RemotePadService *remotePadService) {
    if (remotePadService)
        remotePadService->term();
}

void emptyPadData(OrbisPadData *data) {
    memset(data, 0, sizeof(OrbisPadData));
    data->connected = 1;
    data->count = 1;
    data->leftStick.x = 128;
    data->leftStick.y = 128;
    data->rightStick.x = 128;
    data->rightStick.y = 128;
    // Identity orientation, like a real controller at rest (an all-zero quaternion can't be normalized)
    data->quat.w = 1.0f;
    data->timestamp = sceKernelGetProcessTime();
}

void emptyPadInfo(OrbisPadInformation *info) {
    memset(info, 0, sizeof(OrbisPadInformation));
    info->connected = 1;
    info->count = 1;
    info->stickDeadzoneL = 0x0d;
    info->stickDeadzoneR = 0x0d;
    info->touchResolutionX = 1920;
    info->touchResolutionY = 942;
    info->touchpadDensity = 44.86f;
}

void initPadData(size_t index) {
    if (index >= REMOTE_PAD_MAX_PADS) {
        return;
    }
    initData(&rps.pads[index].padData, sizeof(OrbisPadData), REMOTE_PAD_MAX_HISTORY, (void (*)(void *)) emptyPadData);
}

void resetPadData(size_t index) {
    if (index >= REMOTE_PAD_MAX_PADS) {
        return;
    }
    scePthreadMutexLock(&rps.dataMutex);
    resetData(rps.pads[index].padData);
    scePthreadMutexUnlock(&rps.dataMutex);
}

// Same buttons and the same fingers on the touchpad: the samples differ only in stick, trigger or finger positions
static bool sameEdges(const OrbisPadData *a, const OrbisPadData *b) {
    if (a->buttons != b->buttons || a->touch.fingers != b->touch.fingers)
        return false;
    for (int i = 0; i < a->touch.fingers && i < 2; i++) {
        if (a->touch.touch[i].finger != b->touch.touch[i].finger)
            return false;
    }
    return true;
}

void pushPadData(size_t index, OrbisPadData *data) {
    if (index >= REMOTE_PAD_MAX_PADS) {
        return;
    }
    scePthreadMutexLock(&rps.dataMutex);
    // A game that reads one sample per frame must not fall behind: a sample that only moves a stick replaces
    // the newest unread one. Presses, releases and touches still get their own sample, so none is missed.
    OrbisPadData *newest = peekNewestData(rps.pads[index].padData);
    if (newest != NULL && sameEdges(newest, data))
        memcpy(newest, data, sizeof(OrbisPadData));
    else
        pushData(rps.pads[index].padData, data);
    scePthreadMutexUnlock(&rps.dataMutex);
}

void getLatestPadData(size_t index, OrbisPadData *data) {
    if (index >= REMOTE_PAD_MAX_PADS) {
        emptyPadData(data);
        return;
    }
    scePthreadMutexLock(&rps.dataMutex);
    getLatestData(rps.pads[index].padData, data);
    scePthreadMutexUnlock(&rps.dataMutex);
    stampConnection(&rps.pads[index], data, 1);
}

int32_t getPadData(size_t index, OrbisPadData *data, int32_t count) {
    int32_t ret;
    if (index >= REMOTE_PAD_MAX_PADS || count <= 0) {
        return ORBIS_PAD_ERROR_INVALID_ARG;
    }
    scePthreadMutexLock(&rps.dataMutex);
    ret = getData(rps.pads[index].padData, data, count);
    if (ret == 0) {
        // A real controller keeps sending reports even when nothing changes, so games expect at least one
        // sample per read. Repeat the latest state instead of returning nothing (which looked like a release).
        getLatestData(rps.pads[index].padData, data);
        data->timestamp = sceKernelGetProcessTime();
        ret = 1;
    }
    scePthreadMutexUnlock(&rps.dataMutex);
    stampConnection(&rps.pads[index], data, ret);
    return ret;
}

bool isPadConnected(size_t index) {
    if (index >= REMOTE_PAD_MAX_PADS)
        return false;
    return rps.alwaysConnected || rps.pads[index].deviceConnected;
}

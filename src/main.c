#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "utils.h"
#include "pad.h"
#include "user.h"
#include "settings.h"

attr_public const char *g_pluginName = "RemotePad NG";
attr_public const char *g_pluginDesc = "Play your PS4 with phones, PCs and any controller";
attr_public const char *g_pluginAuth = "xfangfang, MoHadiShibli";
attr_public uint32_t g_pluginVersion = 0x00000100; // RemotePad NG 1.0.0

static RemoteUserService *remoteUserService;
static RemotePadService *remotePad;

static bool prxLoaded = false;
static bool hooksInstalled = false;

// Not hooked: only used to list the console's users on the controller page
int32_t sceUserServiceGetRegisteredUserIdList(OrbisUserServiceRegisteredUserIdList *list);

HOOK_DEFINE(scePadInit, void) {
    if (remotePad->initDriver() != SCE_OK) {
        final_printf("Failed to init remote pad driver\n");
    } else {
        final_printf("RemotePad driver loaded\n");
    }
    return HOOK_PASS(scePadInit);
}

HOOK_DEFINE(scePadOpen, int32_t userId, int32_t type, int32_t index, void *param) {
    RemoteUser *user = remoteUserService->getActiveUser(userId);
    if (user != NULL && sharesController(user) && userId == user->realId) {
        // Share mode: the real controller stays in use and this pad's input is added to it
        int32_t handle = HOOK_PASS(scePadOpen, userId, type, index, param);
        if (handle >= 0)
            remotePad->share(user->index, handle);
        return handle;
    }
    if (user != NULL)
        return remotePad->open(userId, type, user->index, param);

    return HOOK_PASS(scePadOpen, userId, type, index, param);
}

HOOK_DEFINE(scePadClose, int32_t handle) {
    if (remotePad->close(handle) == SCE_OK)
        return SCE_OK;
    remotePad->unshare(handle);
    return HOOK_PASS(scePadClose, handle);
}

HOOK_DEFINE(scePadGetHandle, int32_t userId, uint32_t controller_type, uint32_t controller_index) {
    RemoteUser *user = remoteUserService->getActiveUser(userId);
    if (user != NULL && !(sharesController(user) && userId == user->realId))
        return remotePad->getHandle(userId, controller_type, controller_index);
    return HOOK_PASS(scePadGetHandle, userId, controller_type, controller_index);
}

HOOK_DEFINE(scePadReadState, int32_t handle, OrbisPadData *data) {
    if (remotePad->readState(handle, data) == SCE_OK)
        return SCE_OK;
    int32_t ret = HOOK_PASS(scePadReadState, handle, data);
    if (ret == SCE_OK)
        remotePad->mergeShared(handle, data, 1);
    return ret;
}

HOOK_DEFINE(scePadRead, int32_t handle, OrbisPadData *data, int32_t count) {
    int32_t ret = remotePad->read(handle, data, count);
    if (ret >= 0)
        return ret;
    ret = HOOK_PASS(scePadRead, handle, data, count);
    if (ret > 0)
        remotePad->mergeShared(handle, data, ret);
    return ret;
}

HOOK_DEFINE(scePadGetControllerInformation, int32_t handle, OrbisPadInformation *info) {
    if (remotePad->getControllerInformation(handle, info) == SCE_OK)
        return SCE_OK;
    return HOOK_PASS(scePadGetControllerInformation, handle, info);
}

HOOK_DEFINE(scePadSetLightBar, int32_t handle, OrbisPadColor *inputColor) {
    if (remotePad->setLightBar(handle, inputColor) == SCE_OK)
        return SCE_OK;
    remotePad->shareLightBar(handle, inputColor);
    return HOOK_PASS(scePadSetLightBar, handle, inputColor);
}

HOOK_DEFINE(scePadResetLightBar, int32_t handle) {
    if (remotePad->resetLightBar(handle) == SCE_OK)
        return SCE_OK;
    remotePad->shareResetLightBar(handle);
    return HOOK_PASS(scePadResetLightBar, handle);
}

HOOK_DEFINE(scePadSetVibration, int32_t handle, const OrbisPadVibeParam *param) {
    if (remotePad->setVibration(handle, param) == SCE_OK)
        return SCE_OK;
    remotePad->shareVibration(handle, param);
    return HOOK_PASS(scePadSetVibration, handle, param);
}

HOOK_DEFINE(scePadResetOrientation, int32_t handle) {
    if (remotePad->resetOrientation(handle) == SCE_OK)
        return SCE_OK;
    return HOOK_PASS(scePadResetOrientation, handle);
}

HOOK_DEFINE(scePadSetMotionSensorState, int32_t handle, bool enable) {
    if (remotePad->setMotionSensorState(handle, enable) == SCE_OK)
        return SCE_OK;
    return HOOK_PASS(scePadSetMotionSensorState, handle, enable);
}

HOOK_DEFINE(scePadSetTiltCorrectionState, int32_t handle, bool enable) {
    if (remotePad->setTiltCorrectionState(handle, enable) == SCE_OK)
        return SCE_OK;
    return HOOK_PASS(scePadSetTiltCorrectionState, handle, enable);
}

HOOK_DEFINE(scePadSetAngularVelocityDeadbandState, int32_t handle, bool enable) {
    if (remotePad->setAngularVelocityDeadbandState(handle, enable) == SCE_OK)
        return SCE_OK;
    return HOOK_PASS(scePadSetAngularVelocityDeadbandState, handle, enable);
}

HOOK_DEFINE(scePadDeviceClassParseData, int32_t handle, const OrbisPadData *data, OrbisPadDeviceClassData *classData) {
    if (remotePad->deviceClassParseData(handle, data, classData) == SCE_OK)
        return SCE_OK;
    return HOOK_PASS(scePadDeviceClassParseData, handle, data, classData);
}

HOOK_DEFINE(scePadDeviceClassGetExtendedInformation, int32_t handle, OrbisPadDeviceClassExtInfo *info) {
    if (remotePad->deviceClassGetExtInfo(handle, info) == SCE_OK)
        return SCE_OK;
    return HOOK_PASS(scePadDeviceClassGetExtendedInformation, handle, info);
}

HOOK_DEFINE(sceUserServiceGetUserName, int32_t userId, char *username, size_t size) {
    if (remoteUserService->getUserName(userId, username, size) == SCE_OK)
        return SCE_OK;
    return HOOK_PASS(sceUserServiceGetUserName, userId, username, size);
}

HOOK_DEFINE(sceUserServiceGetUserColor, int32_t userId, OrbisUserServiceUserColor *color) {
    if (remoteUserService->getUserColor(userId, color) == SCE_OK)
        return SCE_OK;
    return HOOK_PASS(sceUserServiceGetUserColor, userId, color);
}

static const RemoteUserSystem userSystem;

static void updateLoginList(OrbisUserServiceLoginUserIdList *list) {
    if (remoteUserService->updateLoginList(list, &userSystem))
        remotePad->notifyStatus();
}

HOOK_DEFINE(sceUserServiceGetLoginUserIdList, OrbisUserServiceLoginUserIdList *list) {
    int32_t ret = HOOK_PASS(sceUserServiceGetLoginUserIdList, list);
    if (ret < 0 || list == NULL)
        return ret;

    // Fill empty positions with remote users
    updateLoginList(list);
    return ret;
}

HOOK_DEFINE(sceUserServiceGetEvent, OrbisUserServiceEvent *event) {
    int32_t ret = HOOK_PASS(sceUserServiceGetEvent, event);
    if (ret == SCE_OK) {
        // A real user logged in or out: the free positions may have changed
        remoteUserService->invalidateLoginList();
        return ret;
    }
    if (ret != (int32_t) ORBIS_USER_SERVICE_ERROR_NO_EVENT || event == NULL)
        return ret;

    // Games that follow users through events only never ask for the list, so look at it here
    if (!remoteUserService->hasLoginList()) {
        OrbisUserServiceLoginUserIdList list;
        if (HOOK_PASS(sceUserServiceGetLoginUserIdList, &list) == SCE_OK)
            updateLoginList(&list);
    }

    if (remoteUserService->nextEvent(event)) {
        final_printf("[RemotePad] %s event for user 0x%08X\n",
                     event->event == SCE_USER_SERVICE_EVENT_TYPE_LOGIN ? "login" : "logout", event->userId);
        return SCE_OK;
    }
    return ret;
}

// The original user service functions, for remotePad's own use
static int32_t systemGetUserColor(int32_t userId, OrbisUserServiceUserColor *color) {
    return HOOK_PASS(sceUserServiceGetUserColor, userId, color);
}

static int32_t systemGetUserName(int32_t userId, char *username, size_t size) {
    return HOOK_PASS(sceUserServiceGetUserName, userId, username, size);
}

static int32_t systemGetLoginUserIdList(OrbisUserServiceLoginUserIdList *list) {
    return HOOK_PASS(sceUserServiceGetLoginUserIdList, list);
}

static int32_t systemGetRegisteredUserIdList(OrbisUserServiceRegisteredUserIdList *list) {
    return sceUserServiceGetRegisteredUserIdList(list);
}

static const RemoteUserSystem userSystem = {
        .getUserColor = systemGetUserColor,
        .getUserName = systemGetUserName,
        .getLoginUserIdList = systemGetLoginUserIdList,
        .getRegisteredUserIdList = systemGetRegisteredUserIdList,
};

int32_t attr_public plugin_load(int32_t argc, const char *argv[]) {
    final_printf("[GoldHEN] %s Plugin Started.\n", g_pluginName);
    final_printf("[GoldHEN] Git version: %s\n", STR(BUILD_TAG_VERSION));
    final_printf("[GoldHEN] <%s\\Ver.0x%08x> %s\n", g_pluginName, g_pluginVersion, __func__);
    final_printf("[GoldHEN] Plugin Author(s): %s\n", g_pluginAuth);

    if (load_prx("libScePad.sprx", true))
        return -1;

    if (load_prx("libSceUserService.sprx", false))
        return -1;

    prxLoaded = true;

    remoteUserService = initRemoteUserService();
    if (!remoteUserService) {
        final_printf("Failed to init remote user service\n");
        return -1;
    }

    remotePad = initRemotePadService();
    if (!remotePad) {
        final_printf("Failed to init remote pad service\n");
        return -1;
    }

    struct proc_info procInfo;
    if (sys_sdk_proc_info(&procInfo) == 0) {
        print_proc_info();
        settingsInit(procInfo.titleid, NULL);
    } else {
        final_printf("Failed to get the process info, only [default] settings apply\n");
        settingsInit(NULL, NULL);
    }
    settingsLoad();

    HOOK32(scePadInit);
    HOOK32(scePadOpen);
    HOOK32(scePadGetHandle);
    HOOK32(scePadRead);
    HOOK32(scePadReadState);
    HOOK32(scePadGetControllerInformation);
    HOOK32(scePadSetLightBar);
    HOOK32(scePadResetLightBar);
    HOOK32(scePadSetVibration);
    HOOK32(scePadResetOrientation);
    HOOK32(scePadSetMotionSensorState);
    HOOK32(scePadSetTiltCorrectionState);
    HOOK32(scePadSetAngularVelocityDeadbandState);
    HOOK32(scePadDeviceClassParseData);
    HOOK32(scePadDeviceClassGetExtendedInformation);
    HOOK32(scePadClose);

    HOOK32(sceUserServiceGetLoginUserIdList);
    HOOK32(sceUserServiceGetUserColor);
    HOOK32(sceUserServiceGetUserName);
    HOOK32(sceUserServiceGetEvent);
    hooksInstalled = true;
    // The original functions are reachable now
    remoteUserService->system = &userSystem;

    return 0;
}

int32_t attr_public plugin_unload(int32_t argc, const char *argv[]) {
    final_printf("[GoldHEN] <%s\\Ver.0x%08x> %s\n", g_pluginName, g_pluginVersion, __func__);
    final_printf("[GoldHEN] %s Plugin Unloaded.\n", g_pluginName);
    Notify(TEX_ICON_SYSTEM, "RemotePad NG unloaded");

    // If the prx is not loaded correctly, then we exit directly
    if (!prxLoaded)
        return 0;

    // Unhook first so the game stops calling into the services torn down below
    if (hooksInstalled) {
        remoteUserService->system = NULL;
        UNHOOK(scePadInit);
        UNHOOK(scePadOpen);
        UNHOOK(scePadGetHandle);
        UNHOOK(scePadRead);
        UNHOOK(scePadReadState);
        UNHOOK(scePadGetControllerInformation);
        UNHOOK(scePadSetLightBar);
        UNHOOK(scePadResetLightBar);
        UNHOOK(scePadSetVibration);
        UNHOOK(scePadResetOrientation);
        UNHOOK(scePadSetMotionSensorState);
        UNHOOK(scePadSetTiltCorrectionState);
        UNHOOK(scePadSetAngularVelocityDeadbandState);
        UNHOOK(scePadDeviceClassParseData);
        UNHOOK(scePadDeviceClassGetExtendedInformation);
        UNHOOK(scePadClose);

        UNHOOK(sceUserServiceGetLoginUserIdList);
        UNHOOK(sceUserServiceGetUserColor);
        UNHOOK(sceUserServiceGetUserName);
        UNHOOK(sceUserServiceGetEvent);
        hooksInstalled = false;
    }

    termRemotePadService(remotePad);
    termRemoteUserService(remoteUserService);
    return 0;
}

int32_t attr_module_hidden module_start(int64_t argc, const void *args) {
    return 0;
}

int32_t attr_module_hidden module_stop(int64_t argc, const void *args) {
    return 0;
}

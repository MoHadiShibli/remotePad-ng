#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <orbis/_types/errors.h>
#include <orbis/libkernel.h>

#include "user.h"
#include "utils.h"

#define USER_COLOR_COUNT 4

static int32_t init(void);

static int32_t term(void);

static RemoteUser *getUser(int32_t);

static RemoteUser *getActiveUser(int32_t);

static RemoteUser *getUserByIndex(int32_t);

static int32_t getUserName(int32_t userId, char *username, size_t size);

static int32_t setUserName(int32_t index, const char *username);

static void setRealId(int32_t index, int32_t realId);

static void setEnabled(int32_t index, bool enabled);

static void setShare(int32_t index, bool share);

static int32_t getUserColor(int32_t userId, OrbisUserServiceUserColor *color);

static bool updateLoginList(OrbisUserServiceLoginUserIdList *list, const RemoteUserSystem *system);

static bool hasLoginList(void);

static void invalidateLoginList(void);

static bool nextEvent(OrbisUserServiceEvent *event);

static bool join(int32_t index);

static bool leave(int32_t index);

static RemoteUserService rus = {
        .init = init,
        .term = term,
        .getUser = getUser,
        .getActiveUser = getActiveUser,
        .getUserByIndex = getUserByIndex,
        .getUserName = getUserName,
        .setUserName = setUserName,
        .setRealId = setRealId,
        .setEnabled = setEnabled,
        .setShare = setShare,
        .getUserColor = getUserColor,
        .updateLoginList = updateLoginList,
        .hasLoginList = hasLoginList,
        .invalidateLoginList = invalidateLoginList,
        .nextEvent = nextEvent,
        .join = join,
        .leave = leave,
};

static void setDefaultName(RemoteUser *user) {
    // Numbered like the pads on the controller page: pad 1 plays as "Remote1"
    snprintf(user->userName, sizeof(user->userName), "Remote%d", user->index + 1);
}

static int32_t init(void) {
    int32_t ret = scePthreadMutexInit(&rus.mutex, 0, "userMtx");
    if (ret < 0) {
        final_printf("[RemotePad]: failed to init the user mutex, 0x%X\n", ret);
        return -1;
    }

    for (int i = 0; i < REMOTE_PAD_MAX_USERS; i++) {
        RemoteUser *user = &rus.users[i];
        user->index = i;
        user->guestId = REMOTE_PAD_VIRTUAL_USER_ID + i;
        user->userId = user->guestId;
        user->realId = 0;
        user->isLoggedIn = false;
        user->loggedInId = 0;
        user->isSystemUserId = false;
        user->isConfiguredId = false;
        user->shareController = false;
        user->hasCustomName = false;
        user->enabled = true;
        user->joined = false;
        user->left = false;
        user->slot = -1;
        user->color = (OrbisUserServiceUserColor) (i % USER_COLOR_COUNT);
        setDefaultName(user);
    }
    rus.hasSystemList = false;
    rus.joinOnStart = false;

    return 0;
}

static int32_t term(void) {
    scePthreadMutexDestroy(&rus.mutex);
    return 0;
}

static RemoteUser *getUser(int32_t userId) {
    for (int i = 0; i < REMOTE_PAD_MAX_USERS; i++) {
        RemoteUser *user = &rus.users[i];
        if (userId == user->guestId || (user->isConfiguredId && userId == user->realId)) {
            return user;
        }
    }
    return NULL;
}

static RemoteUser *getActiveUser(int32_t userId) {
    RemoteUser *user = getUser(userId);
    if (user == NULL || !user->enabled)
        return NULL;
    return user;
}

static RemoteUser *getUserByIndex(int32_t index) {
    if (index >= REMOTE_PAD_MAX_USERS || index < 0)
        return NULL;
    return &rus.users[index];
}

static int32_t getUserName(int32_t userId, char *username, size_t size) {
    RemoteUser *user = getActiveUser(userId);
    if (user == NULL || (playsAsRealUser(user) && !user->hasCustomName))
        return ORBIS_USER_SERVICE_ERROR_NOT_LOGGED_IN;

    if (username == NULL)
        return ORBIS_USER_SERVICE_ERROR_INVALID_ARGUMENT;

    if (size < strlen(user->userName) + 1)
        return ORBIS_USER_SERVICE_ERROR_BUFFER_TOO_SHORT;

    strcpy(username, user->userName);

    return 0;
}

static int32_t findInList(const OrbisUserServiceLoginUserIdList *list, int32_t userId);

static int32_t setUserName(int32_t index, const char *username) {
    RemoteUser *user = getUserByIndex(index);
    if (user == NULL)
        return ORBIS_USER_SERVICE_ERROR_INVALID_ARGUMENT;

    size_t len = username == NULL ? 0 : strlen(username);
    if (len != 0 && (len < 3 || len > ORBIS_USER_SERVICE_MAX_USER_NAME_LENGTH)) {
        return ORBIS_USER_SERVICE_ERROR_INVALID_ARGUMENT;
    }

    scePthreadMutexLock(&rus.mutex);
    if (len == 0) {
        // Back to the default; a real user's console name comes back on the next list update
        user->hasCustomName = false;
        setDefaultName(user);
        rus.hasSystemList = false;
    } else {
        strcpy(user->userName, username);
        user->hasCustomName = true;
    }
    scePthreadMutexUnlock(&rus.mutex);

    return 0;
}

static void setRealId(int32_t index, int32_t realId) {
    RemoteUser *user = getUserByIndex(index);
    if (user == NULL)
        return;
    scePthreadMutexLock(&rus.mutex);
    user->realId = realId;
    user->isConfiguredId = realId != 0;
    // Signed in? Known once the console's list was seen; until then assume so
    user->isSystemUserId = user->isConfiguredId &&
                           (!rus.hasSystemList || findInList(&rus.systemList, realId) >= 0);
    user->userId = playsAsRealUser(user) ? realId : user->guestId;
    rus.hasSystemList = false;
    scePthreadMutexUnlock(&rus.mutex);
}

static void setEnabled(int32_t index, bool enabled) {
    RemoteUser *user = getUserByIndex(index);
    if (user == NULL)
        return;
    scePthreadMutexLock(&rus.mutex);
    if (user->enabled != enabled) {
        user->enabled = enabled;
        rus.hasSystemList = false;
    }
    scePthreadMutexUnlock(&rus.mutex);
}

static void setShare(int32_t index, bool share) {
    RemoteUser *user = getUserByIndex(index);
    if (user != NULL)
        user->shareController = share;
}

static int32_t getUserColor(int32_t userId, OrbisUserServiceUserColor *color) {
    RemoteUser *user = getActiveUser(userId);
    if (user == NULL || playsAsRealUser(user))
        return ORBIS_USER_SERVICE_ERROR_NOT_LOGGED_IN;

    if (color == NULL)
        return ORBIS_USER_SERVICE_ERROR_INVALID_ARGUMENT;

    *color = user->color;

    return 0;
}

static int32_t findInList(const OrbisUserServiceLoginUserIdList *list, int32_t userId) {
    for (int i = 0; i < ORBIS_USER_SERVICE_MAX_LOGIN_USERS; i++) {
        if (list->userId[i] != ORBIS_USER_SERVICE_USER_ID_INVALID && list->userId[i] == userId)
            return i;
    }
    return -1;
}

// A guest is in the game once a device picked its pad, or from the start with join_on_start
static inline bool wantsSlot(const RemoteUser *user) {
    return user->enabled && !playsAsRealUser(user) && (user->joined || (rus.joinOnStart && !user->left));
}

// Colors: real users keep the console's colors, guests get the unused ones in slot order.
// Called with the mutex held; the console was queried beforehand (realColors[slot], -1 when unknown).
static void assignColors(const OrbisUserServiceLoginUserIdList *list, const int32_t realColors[]) {
    uint32_t used = 0;
    for (int i = 0; i < ORBIS_USER_SERVICE_MAX_LOGIN_USERS; i++) {
        if (realColors[i] < 0)
            continue;
        used |= 1u << realColors[i];
        RemoteUser *user = getUser(rus.systemList.userId[i]);
        if (user != NULL && playsAsRealUser(user))
            user->color = (OrbisUserServiceUserColor) realColors[i];
    }
    for (int i = 0; i < ORBIS_USER_SERVICE_MAX_LOGIN_USERS; i++) {
        RemoteUser *user = getUser(list->userId[i]);
        if (user == NULL || playsAsRealUser(user) || user->slot != i)
            continue;
        for (uint32_t color = 0; color < USER_COLOR_COUNT; color++) {
            if (!(used & (1u << color))) {
                user->color = (OrbisUserServiceUserColor) color;
                used |= 1u << color;
                break;
            }
        }
    }
}

static bool updateLoginList(OrbisUserServiceLoginUserIdList *list, const RemoteUserSystem *system) {
    const OrbisUserServiceLoginUserIdList systemList = *list;
    int32_t slots[REMOTE_PAD_MAX_USERS];
    bool changed = false;

    scePthreadMutexLock(&rus.mutex);

    bool systemChanged = !rus.hasSystemList || memcmp(&rus.systemList, &systemList, sizeof(systemList)) != 0;
    rus.systemList = systemList;
    rus.hasSystemList = true;

    // Configured users that are signed in keep the slot the console gave them, the others play as guests
    for (int i = 0; i < REMOTE_PAD_MAX_USERS; i++) {
        RemoteUser *user = &rus.users[i];
        int32_t slot = user->isConfiguredId ? findInList(&systemList, user->realId) : -1;
        user->isSystemUserId = slot >= 0;
        int32_t userId = playsAsRealUser(user) ? user->realId : user->guestId;
        if (user->userId != userId) {
            user->userId = userId;
            if (!user->hasCustomName)
                setDefaultName(user);
            changed = true;
        }
        slots[i] = (user->enabled && playsAsRealUser(user)) ? slot : -1;
    }

    // Guests fill the empty slots, in config order
    int next = 0;
    for (int slot = 0; slot < ORBIS_USER_SERVICE_MAX_LOGIN_USERS; slot++) {
        if (list->userId[slot] != ORBIS_USER_SERVICE_USER_ID_INVALID)
            continue;
        while (next < REMOTE_PAD_MAX_USERS && !wantsSlot(&rus.users[next]))
            next++;
        if (next >= REMOTE_PAD_MAX_USERS)
            break;
        list->userId[slot] = rus.users[next].userId;
        slots[next] = slot;
        next++;
    }

    for (int i = 0; i < REMOTE_PAD_MAX_USERS; i++) {
        if (rus.users[i].slot != slots[i]) {
            rus.users[i].slot = slots[i];
            changed = true;
        }
    }
    scePthreadMutexUnlock(&rus.mutex);

    if (!systemChanged && !changed)
        return false;

    // Ask the console about its users without holding the mutex
    const OrbisUserServiceLoginUserIdList gameList = *list;
    int32_t realColors[ORBIS_USER_SERVICE_MAX_LOGIN_USERS];
    char realNames[REMOTE_PAD_MAX_USERS][ORBIS_USER_SERVICE_MAX_USER_NAME_LENGTH + 1];
    bool hasRealName[REMOTE_PAD_MAX_USERS] = {false};
    for (int i = 0; i < ORBIS_USER_SERVICE_MAX_LOGIN_USERS; i++) {
        OrbisUserServiceUserColor color;
        realColors[i] = -1;
        if (systemList.userId[i] != ORBIS_USER_SERVICE_USER_ID_INVALID && system && system->getUserColor &&
            system->getUserColor(systemList.userId[i], &color) == 0 && color < USER_COLOR_COUNT)
            realColors[i] = color;
    }
    for (int i = 0; i < REMOTE_PAD_MAX_USERS; i++) {
        const RemoteUser *user = &rus.users[i];
        if (playsAsRealUser(user) && !user->hasCustomName && system && system->getUserName &&
            system->getUserName(user->realId, realNames[i], sizeof(realNames[i])) == 0) {
            realNames[i][sizeof(realNames[i]) - 1] = '\0';
            hasRealName[i] = true;
        }
    }

    scePthreadMutexLock(&rus.mutex);
    assignColors(&gameList, realColors);
    for (int i = 0; i < REMOTE_PAD_MAX_USERS; i++) {
        RemoteUser *user = &rus.users[i];
        if (hasRealName[i])
            strcpy(user->userName, realNames[i]);
        final_printf("[RemotePad] pad %d: %s 0x%08X%s, %s, player %d, color %d\n", i + 1,
                     playsAsRealUser(user) ? "user" : "guest", user->userId,
                     user->isConfiguredId && !playsAsRealUser(user) ? " (configured user not signed in)" : "",
                     user->enabled ? "enabled" : "disabled", user->slot + 1, user->color);
    }
    scePthreadMutexUnlock(&rus.mutex);
    return true;
}

static bool hasLoginList(void) {
    return rus.hasSystemList;
}

static void invalidateLoginList(void) {
    rus.hasSystemList = false;
}

static bool nextEvent(OrbisUserServiceEvent *event) {
    bool found = false;
    scePthreadMutexLock(&rus.mutex);
    for (int i = 0; i < REMOTE_PAD_MAX_USERS; i++) {
        RemoteUser *user = &rus.users[i];
        bool visible = user->enabled && !playsAsRealUser(user) && user->slot >= 0;
        if (user->isLoggedIn && (!visible || user->loggedInId != user->userId)) {
            // The guest left, or its pad now plays as the real user: log out the id the game knows
            user->isLoggedIn = false;
            event->event = SCE_USER_SERVICE_EVENT_TYPE_LOGOUT;
            event->userId = user->loggedInId;
            found = true;
            break;
        }
        if (!user->isLoggedIn && visible) {
            user->isLoggedIn = true;
            user->loggedInId = user->userId;
            event->event = SCE_USER_SERVICE_EVENT_TYPE_LOGIN;
            event->userId = user->userId;
            found = true;
            break;
        }
    }
    scePthreadMutexUnlock(&rus.mutex);
    return found;
}

static bool join(int32_t index) {
    bool changed = false;
    RemoteUser *user = getUserByIndex(index);
    if (user == NULL)
        return false;
    scePthreadMutexLock(&rus.mutex);
    if (user->enabled && !playsAsRealUser(user) && (!user->joined || user->left)) {
        user->joined = true;
        user->left = false;
        // The free positions have to be worked out again: the guest takes one
        rus.hasSystemList = false;
        changed = true;
    }
    scePthreadMutexUnlock(&rus.mutex);
    if (changed)
        final_printf("[RemotePad] guest of pad %d (0x%08X) joins the game\n", index + 1, user->guestId);
    return changed;
}

static bool leave(int32_t index) {
    bool changed = false;
    RemoteUser *user = getUserByIndex(index);
    if (user == NULL)
        return false;
    scePthreadMutexLock(&rus.mutex);
    if (user->enabled && !playsAsRealUser(user) && !user->left && (user->joined || rus.joinOnStart)) {
        user->joined = false;
        user->left = true;
        rus.hasSystemList = false;
        changed = true;
    }
    scePthreadMutexUnlock(&rus.mutex);
    if (changed)
        final_printf("[RemotePad] guest of pad %d (0x%08X) leaves the game\n", index + 1, user->guestId);
    return changed;
}

RemoteUserService *initRemoteUserService(void) {
    if (rus.init() != 0)
        return NULL;
    return &rus;
}

RemoteUserService *getRemoteUserService(void) {
    return &rus;
}

void termRemoteUserService(RemoteUserService *remoteUserService) {
    if (remoteUserService)
        remoteUserService->term();
}

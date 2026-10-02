#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <orbis/libkernel.h>

#include "config.h"
#include "pad.h"
#include "settings.h"
#include "user.h"
#include "utils.h"

#define DEFAULT_SECTION "default"
#define FILE_O_WRONLY 0x0001
#define FILE_O_CREAT 0x0200
#define FILE_O_TRUNC 0x0400

static char titleId[16];
static char settingsPath[128] = SETTINGS_PATH_DEFAULT;
// The file has a section for the running game
static bool gameSection = false;
// lock_settings: the controller page may show the settings but not change them
static bool locked = false;

void settingsInit(const char *title, const char *path) {
    snprintf(titleId, sizeof(titleId), "%s", title ? title : "");
    if (path != NULL)
        snprintf(settingsPath, sizeof(settingsPath), "%s", path);
}

const char *settingsTitleId(void) {
    return titleId;
}

static void loadSection(ini_table_s *table, const char *section) {
    RemoteUserService *users = getRemoteUserService();
    RemotePadService *pads = getRemotePadService();
    char key[24];

    // Guests join when the game starts (old behavior) instead of when someone picks their pad
    ini_table_get_entry_as_bool(table, section, "join_on_start", &users->joinOnStart);
    // Pads look connected even when no device drives them (old behavior)
    ini_table_get_entry_as_bool(table, section, "always_connected", &pads->alwaysConnected);
    // For a server reachable from outside the home: nobody can change the settings from the page
    ini_table_get_entry_as_bool(table, section, "lock_settings", &locked);

    for (int i = 0; i < REMOTE_PAD_MAX_USERS; i++) {
        const char *value;
        bool enabled;

        snprintf(key, sizeof(key), "user%d_enabled", i);
        if (ini_table_get_entry_as_bool(table, section, key, &enabled))
            users->setEnabled(i, enabled);

        // The real user's own controller keeps working, this pad adds to it
        snprintf(key, sizeof(key), "user%d_share", i);
        if (ini_table_get_entry_as_bool(table, section, key, &enabled))
            users->setShare(i, enabled);

        // Ids may be written in hex (as Apollo shows them) or in decimal; empty = no PS4 user
        snprintf(key, sizeof(key), "user%d_id", i);
        value = ini_table_get_entry(table, section, key);
        if (value != NULL) {
            int32_t userId;
            if (value[0] == '\0') {
                users->setRealId(i, 0);
            } else if (ini_parse_user_id(value, &userId)) {
                users->setRealId(i, userId);
            } else {
                final_printf("[%s] %s=\"%s\" is not a user id (use the hex id shown by Apollo, like 1A2B3C4D)\n",
                             section, key, value);
                Notify(TEX_ICON_SYSTEM, "RemotePad NG: %s is not a valid user id", key);
            }
        }

        snprintf(key, sizeof(key), "user%d_name", i);
        value = ini_table_get_entry(table, section, key);
        if (value != NULL && users->setUserName(i, value) != 0)
            final_printf("[%s] %s=\"%s\" ignored: a name needs 3 to 16 characters\n", section, key, value);
    }
}

// Two pads with the same user id can't both work: keep the first one
static void checkUsers(void) {
    RemoteUserService *users = getRemoteUserService();
    for (int i = 0; i < REMOTE_PAD_MAX_USERS; i++) {
        RemoteUser *user = users->getUserByIndex(i);
        for (int j = 0; j < i; j++) {
            RemoteUser *other = users->getUserByIndex(j);
            if (user->enabled && other->enabled && user->isConfiguredId && other->isConfiguredId &&
                user->realId == other->realId) {
                final_printf("user%d_id is the same as user%d_id (0x%08X), user%d disabled\n", i, j, user->realId, i);
                Notify(TEX_ICON_SYSTEM, "RemotePad NG: user%d_id is the same as user%d_id, pad %d disabled", i, j, i + 1);
                users->setEnabled(i, false);
            }
        }
    }
    for (int i = 0; i < REMOTE_PAD_MAX_USERS; i++) {
        RemoteUser *user = users->getUserByIndex(i);
        if (user->isConfiguredId)
            final_printf("Pad %d: PS4 user 0x%08X (%s) when signed in, else guest 0x%08X, name \"%s\"%s\n", i + 1,
                         user->realId, user->shareController ? "with its own controller" : "replacing its controller",
                         user->guestId, user->userName, user->enabled ? "" : ", turned off");
        else
            final_printf("Pad %d: guest 0x%08X, name \"%s\"%s\n", i + 1, user->guestId, user->userName,
                         user->enabled ? "" : ", turned off");
    }
    final_printf("join_on_start=%d always_connected=%d lock_settings=%d\n", users->joinOnStart,
                 getRemotePadService()->alwaysConnected, locked);
}

bool settingsLocked(void) {
    return locked;
}

void settingsLoad(void) {
    gameSection = false;
    locked = false;
    if (!file_exists(settingsPath)) {
        final_printf("No settings file at \"%s\", using the defaults\n", settingsPath);
        checkUsers();
        return;
    }

    ini_table_s *table = ini_table_create();
    if (table == NULL) {
        final_printf("Config parser failed to initialise\n");
        return;
    }

    if (!ini_table_read_from_file(table, settingsPath)) {
        final_printf("Config parser failed to parse config: %s\n", settingsPath);
        ini_table_destroy(table);
        return;
    }

    // [default] first, so the section of the running game can override it
    if (_ini_section_find(table, DEFAULT_SECTION) != NULL) {
        final_printf("Using section [%s]\n", DEFAULT_SECTION);
        loadSection(table, DEFAULT_SECTION);
    }
    if (titleId[0] != '\0' && _ini_section_find(table, titleId) != NULL) {
        final_printf("Using section [%s]\n", titleId);
        loadSection(table, titleId);
        gameSection = true;
    }

    ini_table_destroy(table);
    checkUsers();
}

void settingsGet(Settings *settings, bool *gameOnly) {
    RemoteUserService *users = getRemoteUserService();
    memset(settings, 0, sizeof(*settings));
    settings->joinOnStart = users->joinOnStart;
    settings->alwaysConnected = getRemotePadService()->alwaysConnected;
    scePthreadMutexLock(&users->mutex);
    for (int i = 0; i < REMOTE_PAD_MAX_USERS; i++) {
        const RemoteUser *user = &users->users[i];
        settings->pads[i].enabled = user->enabled;
        settings->pads[i].realId = user->isConfiguredId ? user->realId : 0;
        settings->pads[i].share = user->shareController;
        if (user->hasCustomName)
            snprintf(settings->pads[i].name, sizeof(settings->pads[i].name), "%s", user->userName);
    }
    scePthreadMutexUnlock(&users->mutex);
    if (gameOnly != NULL)
        *gameOnly = gameSection;
}

// ---------------------------------------------------------------- saving

typedef struct Text {
    char *data;
    size_t length;
    size_t capacity;
} Text;

static void textAdd(Text *text, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void textAdd(Text *text, const char *fmt, ...) {
    va_list args;
    if (text->data == NULL)
        return;
    for (;;) {
        va_start(args, fmt);
        int n = vsnprintf(text->data + text->length, text->capacity - text->length, fmt, args);
        va_end(args);
        if (n < 0)
            return;
        if (text->length + (size_t) n < text->capacity) {
            text->length += (size_t) n;
            return;
        }
        size_t capacity = (text->capacity + (size_t) n) * 2;
        char *grown = realloc(text->data, capacity);
        if (grown == NULL) {
            free(text->data);
            text->data = NULL;
            return;
        }
        text->data = grown;
        text->capacity = capacity;
    }
}

static bool isSettingsKey(const char *key) {
    int index;
    char rest[16];
    if (strcmp(key, "join_on_start") == 0 || strcmp(key, "always_connected") == 0)
        return true;
    return sscanf(key, "user%d_%15s", &index, rest) == 2 && index >= 0 && index < REMOTE_PAD_MAX_USERS &&
           (strcmp(rest, "enabled") == 0 || strcmp(rest, "id") == 0 || strcmp(rest, "name") == 0 ||
            strcmp(rest, "share") == 0);
}

// Our keys, then whatever else the section had. A game section lists everything so it fully replaces [default].
static void addSettingsSection(Text *text, const char *name, const Settings *settings, const ini_section_s *old,
                               bool complete) {
    textAdd(text, "\n[%s]\n", name);
    textAdd(text, "join_on_start=%d\n", settings->joinOnStart ? 1 : 0);
    textAdd(text, "always_connected=%d\n", settings->alwaysConnected ? 1 : 0);
    for (int i = 0; i < REMOTE_PAD_MAX_USERS; i++) {
        const PadSettings *pad = &settings->pads[i];
        textAdd(text, "user%d_enabled=%d\n", i, pad->enabled ? 1 : 0);
        if (pad->share || complete)
            textAdd(text, "user%d_share=%d\n", i, pad->share ? 1 : 0);
        // 0x: an id like 10000001 would otherwise read back as decimal
        if (pad->realId != 0)
            textAdd(text, "user%d_id=0x%08X\n", i, (uint32_t) pad->realId);
        else if (complete)
            textAdd(text, "user%d_id=\n", i);
        if (pad->name[0] != '\0')
            textAdd(text, "user%d_name=%s\n", i, pad->name);
        else if (complete)
            textAdd(text, "user%d_name=\n", i);
    }
    for (int i = 0; old != NULL && i < old->size; i++) {
        if (!isSettingsKey(old->entry[i].key))
            textAdd(text, "%s=%s\n", old->entry[i].key, old->entry[i].value);
    }
}

static void addSection(Text *text, const ini_section_s *section) {
    textAdd(text, "\n[%s]\n", section->name);
    for (int i = 0; i < section->size; i++)
        textAdd(text, "%s=%s\n", section->entry[i].key, section->entry[i].value);
}

// Write to a temporary file, then rename it over the old one, so a failure never leaves half a file
static bool writeFile(const char *path, const char *data, size_t length) {
    char temp[sizeof(settingsPath) + 8];
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    int32_t fd = sceKernelOpen(temp, FILE_O_WRONLY | FILE_O_CREAT | FILE_O_TRUNC, 0777);
    if (fd < 0) {
        final_printf("Could not create %s (0x%08X)\n", temp, fd);
        return false;
    }
    size_t written = sceKernelWrite(fd, data, length);
    sceKernelFsync(fd);
    sceKernelClose(fd);
    if (written != length) {
        final_printf("Could not write %s\n", temp);
        sceKernelUnlink(temp);
        return false;
    }
    int32_t ret = sceKernelRename(temp, path);
    if (ret < 0) {
        final_printf("Could not replace %s (0x%08X)\n", path, ret);
        sceKernelUnlink(temp);
        return false;
    }
    return true;
}

static bool saveSettings(const Settings *settings, bool gameOnly) {
    ini_table_s *table = ini_table_create();
    if (table == NULL)
        return false;
    if (file_exists(settingsPath))
        ini_table_read_from_file(table, settingsPath);

    const char *target = gameOnly ? titleId : DEFAULT_SECTION;
    Text text = {malloc(2048), 0, 2048};
    textAdd(&text, "; RemotePad NG settings, written by the controller page (Settings).\n");
    textAdd(&text, "; [default] applies to every game. A section named after a game's title id applies to that\n");
    textAdd(&text, "; game only. User ids are PS4 user ids in hex, as Apollo shows them.\n");

    bool wroteTarget = false;
    const ini_section_s *defaults = _ini_section_find(table, DEFAULT_SECTION);
    if (!gameOnly) {
        addSettingsSection(&text, DEFAULT_SECTION, settings, defaults, false);
        wroteTarget = true;
    } else if (defaults != NULL) {
        addSection(&text, defaults);
    }
    for (int i = 0; i < table->size; i++) {
        const ini_section_s *section = &table->section[i];
        if (section->name[0] == '\0' || strcmp(section->name, DEFAULT_SECTION) == 0)
            continue;
        if (titleId[0] != '\0' && strcmp(section->name, titleId) == 0) {
            // "All games" replaces this game's own settings
            if (gameOnly) {
                addSettingsSection(&text, target, settings, section, true);
                wroteTarget = true;
            }
            continue;
        }
        addSection(&text, section);
    }
    if (!wroteTarget)
        addSettingsSection(&text, target, settings, NULL, gameOnly);
    ini_table_destroy(table);

    bool ok = text.data != NULL && writeFile(settingsPath, text.data, text.length);
    free(text.data);
    if (ok) {
        gameSection = gameOnly;
        final_printf("Settings saved to %s [%s]\n", settingsPath, target);
    }
    return ok;
}

// ---------------------------------------------------------------- changing

static bool validName(const char *name) {
    size_t length = strlen(name);
    if (length == 0)
        return true;
    if (length < 3 || length > ORBIS_USER_SERVICE_MAX_USER_NAME_LENGTH)
        return false;
    for (const char *p = name; *p; p++) {
        // ';' starts a comment in the file, control characters would break its lines
        if ((unsigned char) *p < 0x20 || *p == ';' || *p == 0x7F)
            return false;
    }
    return true;
}

bool settingsSet(const Settings *settings, bool gameOnly, bool *saved, char *error, size_t errorSize) {
    RemoteUserService *users = getRemoteUserService();
    RemotePadService *pads = getRemotePadService();
    *saved = false;

    if (locked) {
        snprintf(error, errorSize, "The settings are locked (lock_settings=1 in remote_pad.ini)");
        return false;
    }
    if (gameOnly && titleId[0] == '\0') {
        snprintf(error, errorSize, "No game is running, so settings can only be saved for all games");
        return false;
    }
    for (int i = 0; i < REMOTE_PAD_MAX_USERS; i++) {
        const PadSettings *pad = &settings->pads[i];
        if (!validName(pad->name)) {
            snprintf(error, errorSize, "Pad %d: a name needs 3 to 16 characters and no ';'", i + 1);
            return false;
        }
        if (pad->realId == -1) {
            snprintf(error, errorSize, "Pad %d: that is not a PS4 user id", i + 1);
            return false;
        }
        for (int j = 0; j < i; j++) {
            if (pad->realId != 0 && pad->realId == settings->pads[j].realId) {
                snprintf(error, errorSize, "Pads %d and %d can't use the same PS4 user", j + 1, i + 1);
                return false;
            }
        }
    }

    users->joinOnStart = settings->joinOnStart;
    pads->alwaysConnected = settings->alwaysConnected;
    for (int i = 0; i < REMOTE_PAD_MAX_USERS; i++) {
        const PadSettings *pad = &settings->pads[i];
        users->setEnabled(i, pad->enabled);
        users->setShare(i, pad->share);
        users->setRealId(i, pad->realId);
        users->setUserName(i, pad->name);
    }
    users->invalidateLoginList();
    pads->notifyStatus();
    final_printf("Settings changed from the controller page\n");
    checkUsers();

    *saved = saveSettings(settings, gameOnly);
    return true;
}

// ---------------------------------------------------------------- console users

static void addConsoleUser(ConsoleUser *users, int *count, int max, int32_t userId, bool signedIn,
                           const RemoteUserSystem *system) {
    if (userId == ORBIS_USER_SERVICE_USER_ID_INVALID || userId == 0 || *count >= max)
        return;
    for (int i = 0; i < *count; i++) {
        if (users[i].id == userId) {
            users[i].signedIn |= signedIn;
            return;
        }
    }
    ConsoleUser *user = &users[(*count)++];
    memset(user, 0, sizeof(*user));
    user->id = userId;
    user->signedIn = signedIn;
    if (system != NULL && system->getUserName != NULL &&
        system->getUserName(userId, user->name, sizeof(user->name)) == 0)
        user->name[sizeof(user->name) - 1] = '\0';
    else
        user->name[0] = '\0';
}

int settingsListConsoleUsers(ConsoleUser *users, int max) {
    RemoteUserService *service = getRemoteUserService();
    const RemoteUserSystem *system = service->system;
    OrbisUserServiceLoginUserIdList login;
    OrbisUserServiceRegisteredUserIdList registered;
    bool hasLogin = false;
    int count = 0;

    if (system != NULL && system->getLoginUserIdList != NULL && system->getLoginUserIdList(&login) == 0) {
        hasLogin = true;
    } else if (service->hasLoginList()) {
        login = service->systemList;
        hasLogin = true;
    }
    if (system != NULL && system->getRegisteredUserIdList != NULL &&
        system->getRegisteredUserIdList(&registered) == 0) {
        for (int i = 0; i < ORBIS_USER_SERVICE_MAX_REGISTER_USERS; i++) {
            bool signedIn = false;
            for (int j = 0; hasLogin && j < ORBIS_USER_SERVICE_MAX_LOGIN_USERS; j++)
                signedIn |= login.userId[j] == registered.userId[i];
            addConsoleUser(users, &count, max, registered.userId[i], signedIn, system);
        }
    }
    for (int j = 0; hasLogin && j < ORBIS_USER_SERVICE_MAX_LOGIN_USERS; j++)
        addConsoleUser(users, &count, max, login.userId[j], true, system);
    return count;
}

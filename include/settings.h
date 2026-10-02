#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "user.h"

#ifndef REMOTE_PAD_SETTINGS_H
#define REMOTE_PAD_SETTINGS_H

#define SETTINGS_PATH_DEFAULT "/data/GoldHEN/remote_pad.ini"
#define SETTINGS_MAX_CONSOLE_USERS 16

// Everything the controller page can change. Stored in remote_pad.ini: [default] for all games, or a
// section named after the running game's title id for that game only.
typedef struct PadSettings {
    bool enabled;
    // A PS4 user this pad plays as while that user is signed in; 0 = always a guest
    int32_t realId;
    // With realId: that user's own controller keeps working and this pad's input is added to it
    bool share;
    // Name shown in games; "" = the default (the console name of the real user, or "RemoteN")
    char name[ORBIS_USER_SERVICE_MAX_USER_NAME_LENGTH + 1];
} PadSettings;

typedef struct Settings {
    bool joinOnStart;
    bool alwaysConnected;
    PadSettings pads[REMOTE_PAD_MAX_USERS];
} Settings;

typedef struct ConsoleUser {
    int32_t id;
    char name[ORBIS_USER_SERVICE_MAX_USER_NAME_LENGTH + 1];
    bool signedIn;
} ConsoleUser;

// Remember the running game and the file to use (NULL: SETTINGS_PATH_DEFAULT)
void settingsInit(const char *titleId, const char *path);

// Read the file: [default] first, then the game's own section. Applies the result to the services.
void settingsLoad(void);

// The settings in effect, and whether they come from the game's own section
void settingsGet(Settings *settings, bool *gameOnly);

// Check, apply right away and save. gameOnly: store them for this game only (otherwise for all games, which
// also drops this game's own section). Returns false with a message when the settings are not valid;
// *saved tells whether the file could be written.
bool settingsSet(const Settings *settings, bool gameOnly, bool *saved, char *error, size_t errorSize);

// remote_pad.ini lock_settings=1: settingsSet refuses every change
bool settingsLocked(void);

// Users of this console for the "PS4 user" choice: the registered ones if the system lets a game see them,
// at least the signed-in ones. Returns the count.
int settingsListConsoleUsers(ConsoleUser *users, int max);

const char *settingsTitleId(void);

#endif //REMOTE_PAD_SETTINGS_H

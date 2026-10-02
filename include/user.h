#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <orbis/_types/user.h>
#include <orbis/_types/pthread.h>

#ifndef REMOTE_PAD_USER_H
#define REMOTE_PAD_USER_H

#define REMOTE_PAD_MAX_USERS 4
#define REMOTE_PAD_VIRTUAL_USER_ID 0x20000000 // + index: the guest id of each pad

typedef struct RemoteUser {
    int32_t index;
    // The id games see now: realId while that user is signed in on the console, guestId otherwise
    int32_t userId;
    // Id of this pad's guest player (REMOTE_PAD_VIRTUAL_USER_ID + index)
    int32_t guestId;
    // remote_pad.ini userN_id: a real PS4 user whose controller this pad takes over while that user is
    // signed in. When the user isn't signed in, the pad plays as a guest.
    int32_t realId;
    char userName[ORBIS_USER_SERVICE_MAX_USER_NAME_LENGTH + 1];
    bool enabled;
    // realId is set
    bool isConfiguredId;
    // userName comes from remote_pad.ini (otherwise real users keep their console name)
    bool hasCustomName;
    // realId is in the console's own login user list, so the pad plays as that user
    bool isSystemUserId;
    // remote_pad.ini userN_share: the real user's own controller keeps working and this pad's input is
    // added to it, instead of this pad replacing it
    bool shareController;
    // position in the login user list the game sees (player number - 1), -1 when the game doesn't see this user
    int32_t slot;
    OrbisUserServiceUserColor color;
    // Guests only: a device picked this pad, so the guest joins the game (see joinOnStart)
    bool joined;
    // Guests only: the guest left the game and stays out until it joins again
    bool left;

    /// private
    // whether a login event has been returned for the guest, and with which id
    bool isLoggedIn;
    int32_t loggedInId;
} RemoteUser;

// The original (unhooked) user service functions
typedef struct RemoteUserSystem {
    int32_t (*getUserColor)(int32_t userId, OrbisUserServiceUserColor *color);

    int32_t (*getUserName)(int32_t userId, char *username, size_t size);

    // Console's own list, without remote users (optional)
    int32_t (*getLoginUserIdList)(OrbisUserServiceLoginUserIdList *list);

    // Every user registered on the console; games may not be allowed to read it (optional)
    int32_t (*getRegisteredUserIdList)(OrbisUserServiceRegisteredUserIdList *list);
} RemoteUserSystem;

typedef struct RemoteUserService {
    int32_t (*init)(void);

    int32_t (*term)(void);

    // User with this guest id or real id
    RemoteUser *(*getUser)(int32_t userId);

    // Enabled user with this guest id or real id, NULL when the id is not handled by remotePad
    RemoteUser *(*getActiveUser)(int32_t userId);

    RemoteUser *(*getUserByIndex)(int32_t index);

    // Fail when the system should answer (unknown user or a signed-in real user without a configured name)
    int32_t (*getUserName)(int32_t userId, char *username, size_t size);

    // Name for the user with this index (3 to 16 characters); NULL or "" goes back to the default name
    int32_t (*setUserName)(int32_t index, const char *username);

    // The pad with this index takes over this real user while it is signed in; 0 = always a guest
    void (*setRealId)(int32_t index, int32_t realId);

    void (*setEnabled)(int32_t index, bool enabled);

    void (*setShare)(int32_t index, bool share);

    // Fail when the system should answer (unknown user or a signed-in real user)
    int32_t (*getUserColor)(int32_t userId, OrbisUserServiceUserColor *color);

    // Takes the console's login list, marks the signed-in real users and puts guests into the empty slots.
    // Returns true when the users the game sees changed.
    bool (*updateLoginList)(OrbisUserServiceLoginUserIdList *list, const RemoteUserSystem *system);

    // Whether the slots are known (updateLoginList ran since the last invalidateLoginList)
    bool (*hasLoginList)(void);

    void (*invalidateLoginList)(void);

    // Next login/logout event for a guest, false when there is nothing to report
    bool (*nextEvent)(OrbisUserServiceEvent *event);

    // The guest player of this pad joins / leaves the game. Returns true when that changes anything.
    bool (*join)(int32_t index);

    bool (*leave)(int32_t index);

    RemoteUser users[REMOTE_PAD_MAX_USERS];
    OrbisPthreadMutex mutex;
    OrbisUserServiceLoginUserIdList systemList;
    bool hasSystemList;
    // The original user service functions (set by the plugin)
    const RemoteUserSystem *system;
    // remote_pad.ini join_on_start: guest players are in the game from the start (the old behavior)
    // instead of joining when a device picks their pad
    bool joinOnStart;
} RemoteUserService;

// Whether the pad plays as its real (configured and signed-in) user rather than as a guest
static inline bool playsAsRealUser(const RemoteUser *user) {
    return user->isConfiguredId && user->isSystemUserId;
}

// Whether the pad adds its input to the real user's own controller (share mode) rather than replacing it
static inline bool sharesController(const RemoteUser *user) {
    return user->shareController && playsAsRealUser(user);
}

RemoteUserService *initRemoteUserService(void);

RemoteUserService *getRemoteUserService(void);

void termRemoteUserService(RemoteUserService *);

#endif //REMOTE_PAD_USER_H

#include <string.h>
#include <mongoose.h>
#include <orbis/libkernel.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "config.h"
#include "pad.h"
#include "settings.h"
#include "user.h"
#include "utils.h"

#define REMOTE_PAD_WS_PORT "4263"
// 1: u/notify/info. 2: adds claim/status requests and the "s" status notification
#define WS_PROTOCOL_VERSION 2
// Skip non-essential output to a client with this much unsent data (it stopped reading)
#define WS_MAX_PENDING_SEND (64 * 1024)
// Drop a client with this much unsent data
#define WS_MAX_SEND_BUFFER (1024 * 1024)
// Clients that claim pads send something at least every second; release their pads when they go quiet
#define WS_CLIENT_TIMEOUT_MS 3000
#define WS_THREAD_STACK_SIZE (512 * 1024)
#define PAD_VALID_BUTTONS 0x0010FFFE
// Connections at once (page loads and WebSocket clients); more are closed right away
#define WS_MAX_CONNECTIONS 16
// Sent with the page: nothing loads from elsewhere, other sites can't frame it, no referrer leaves it
#define WS_PAGE_HEADERS \
    "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; " \
    "img-src data:; media-src data:; connect-src ws: wss:; frame-ancestors 'none'; base-uri 'none'; " \
    "form-action 'none'\r\n" \
    "X-Content-Type-Options: nosniff\r\n" \
    "Referrer-Policy: no-referrer\r\n"

typedef struct wsDriverData {
    struct mg_mgr mgr;
    struct mg_rpc *rpc_head;
    OrbisPadVibeParam lastVibration[REMOTE_PAD_MAX_PADS];
    OrbisPadColor lastColor[REMOTE_PAD_MAX_PADS];
    bool lightBarReset[REMOTE_PAD_MAX_PADS];
    bool lightBarSet[REMOTE_PAD_MAX_PADS];
    OrbisPthreadMutex wakeupMutex;
    // Sending side of the wakeup socket pair, -1 while the server is not running (guarded by wakeupMutex)
    int wakeupFd;
    bool mutexReady;
    bool threadStarted;
    bool running;
} wsDriverData;

// Per connection state, stored in mg_connection::data
typedef struct wsClient {
    uint8_t pads;      // pads controlled by this client, one bit per pad
    bool claimed;      // uses the "claim" request, and so sends something at least every second
    bool stale;        // went quiet: its pads were released
    bool canConfigure; // may change settings and show notifications (see wsAccessFor)
    uint32_t lastSeen; // mg_millis() of the last message
} wsClient;

// What a WebSocket handshake may do
enum wsAccess {
    WS_ACCESS_DENIED = 0, // a page from another site
    WS_ACCESS_PLAY,       // pads only
    WS_ACCESS_FULL,       // pads, settings and notifications
};

_Static_assert(sizeof(wsClient) <= MG_DATA_SIZE, "wsClient must fit in mg_connection::data");

typedef struct wsPadStatus {
    int32_t userId;
    int32_t realId; // configured PS4 user, 0 when none
    int32_t player;
    uint8_t color[3];
    uint8_t clients;
    bool open;
    bool enabled;
    bool real;
    bool logged;
    bool joined;
    bool connected;
    bool full;
    bool shared;
    char name[ORBIS_USER_SERVICE_MAX_USER_NAME_LENGTH + 1];
} wsPadStatus;

enum wsWakeupEvent {
    WS_WAKEUP_VIBRATION = 0,
    WS_WAKEUP_LIGHTBAR,
    WS_WAKEUP_RESET_LIGHTBAR,
    WS_WAKEUP_STATUS,
};

// Lightbar colors for the PS4 user colors: blue, red, green, pink
static const uint8_t userLightBarColors[4][3] = {
        {32, 96, 255},
        {255, 48, 64},
        {48, 200, 96},
        {255, 80, 180},
};

union usa {
    struct sockaddr sa;
    struct sockaddr_in sin;
#if MG_ENABLE_IPV6
    struct sockaddr_in6 sin6;
#endif
};

static bool mg_socketpair(MG_SOCKET_TYPE sp[2], union usa usa[2]) {
    socklen_t n = sizeof(usa[0].sin);
    bool success = false;

    sp[0] = sp[1] = MG_INVALID_SOCKET;
    (void) memset(&usa[0], 0, sizeof(usa[0]));
    usa[0].sin.sin_family = AF_INET;
    *(uint32_t *) &usa->sin.sin_addr = mg_htonl(0x7f000001U);  // 127.0.0.1
    usa[1] = usa[0];

    if ((sp[0] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) != MG_INVALID_SOCKET &&
        (sp[1] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) != MG_INVALID_SOCKET &&
        bind(sp[0], &usa[0].sa, n) == 0 &&          //
        bind(sp[1], &usa[1].sa, n) == 0 &&          //
        getsockname(sp[0], &usa[0].sa, &n) == 0 &&  //
        getsockname(sp[1], &usa[1].sa, &n) == 0 &&  //
        connect(sp[0], &usa[1].sa, n) == 0 &&       //
        connect(sp[1], &usa[0].sa, n) == 0) {       //
        success = true;
    }
    if (!success) {
        if (sp[0] != MG_INVALID_SOCKET) close(sp[0]);
        if (sp[1] != MG_INVALID_SOCKET) close(sp[1]);
        sp[0] = sp[1] = MG_INVALID_SOCKET;
    }
    return success;
}

static void tomgaddr(union usa *usa, struct mg_addr *a, bool is_ip6) {
    a->is_ip6 = is_ip6;
    a->port = usa->sin.sin_port;
    memcpy(&a->ip, &usa->sin.sin_addr, sizeof(uint32_t));
#if MG_ENABLE_IPV6
    if (is_ip6) {
    memcpy(a->ip, &usa->sin6.sin6_addr, sizeof(a->ip));
    a->port = usa->sin6.sin6_port;
    a->scope_id = (uint8_t) usa->sin6.sin6_scope_id;
  }
#endif
}

static inline wsClient *clientOf(struct mg_connection *c) {
    return (wsClient *) c->data;
}

// "http://192.168.1.20:4263" -> "192.168.1.20:4263" (empty when there's no scheme)
static struct mg_str originHost(struct mg_str origin) {
    for (size_t i = 0; i + 2 < origin.len; i++) {
        if (origin.buf[i] == ':' && origin.buf[i + 1] == '/' && origin.buf[i + 2] == '/')
            return mg_str_n(origin.buf + i + 3, origin.len - i - 3);
    }
    return mg_str_n(NULL, 0);
}

// A Host header naming the console by IP address (or localhost), not by a domain name
static bool isAddressHost(struct mg_str host) {
    size_t length = host.len;
    int dots = 0;
    if (length > 0 && host.buf[0] == '[')
        return true; // IPv6 literal
    for (size_t i = 0; i < host.len; i++) {
        if (host.buf[i] == ':') {
            length = i;
            break;
        }
    }
    if (mg_strcasecmp(mg_str_n(host.buf, length), mg_str("localhost")) == 0)
        return true;
    if (length == 0)
        return false;
    for (size_t i = 0; i < length; i++) {
        if (host.buf[i] == '.')
            dots++;
        else if (host.buf[i] < '0' || host.buf[i] > '9')
            return false;
    }
    return dots == 3;
}

// Browsers send Origin with every WebSocket handshake, and any web page may try to open one to the console:
// - a page from another site is refused (cross-site WebSocket hijacking);
// - the page saved as a file ("null") can play but not change settings;
// - settings need the console's own page opened by IP address. A site using DNS rebinding reaches the console
//   under its own domain name, so it only gets to play.
// Clients that aren't browsers send no Origin and may do everything.
static enum wsAccess wsAccessFor(struct mg_http_message *hm) {
    struct mg_str *origin = mg_http_get_header(hm, "Origin");
    struct mg_str *host = mg_http_get_header(hm, "Host");
    if (origin == NULL)
        return WS_ACCESS_FULL;
    if (mg_strcmp(*origin, mg_str("null")) == 0)
        return WS_ACCESS_PLAY;
    struct mg_str site = originHost(*origin);
    if (host == NULL || site.len == 0 || mg_strcasecmp(site, *host) != 0)
        return WS_ACCESS_DENIED;
    return isAddressHost(*host) ? WS_ACCESS_FULL : WS_ACCESS_PLAY;
}

// For requests that change the console: refuse them from connections that may only play
static bool mayConfigure(struct mg_rpc_req *r) {
    struct mg_connection *c = (struct mg_connection *) r->req_data;
    if (c != NULL && clientOf(c)->canConfigure)
        return true;
    mg_rpc_err(r, 403, "%m", MG_ESC("Open the page at the console's IP address to change settings"));
    return false;
}

static void getStatus(wsDriverData *ctx, wsPadStatus status[REMOTE_PAD_MAX_PADS]) {
    RemotePadService *padService = getRemotePadService();
    RemoteUserService *userService = getRemoteUserService();
    memset(status, 0, sizeof(wsPadStatus) * REMOTE_PAD_MAX_PADS);

    scePthreadMutexLock(&padService->padMutex);
    for (int i = 0; i < REMOTE_PAD_MAX_PADS; i++) {
        RemoteUser *user = userService->getUserByIndex(i);
        status[i].open = padService->pads[i].userId != 0 || padService->pads[i].sharedHandle >= 0;
        if (ctx->lightBarSet[i] && !ctx->lightBarReset[i]) {
            status[i].color[0] = ctx->lastColor[i].r;
            status[i].color[1] = ctx->lastColor[i].g;
            status[i].color[2] = ctx->lastColor[i].b;
        } else {
            memcpy(status[i].color, userLightBarColors[user->color % 4], 3);
        }
    }
    scePthreadMutexUnlock(&padService->padMutex);

    for (int i = 0; i < REMOTE_PAD_MAX_PADS; i++) {
        RemoteUser *user = userService->getUserByIndex(i);
        bool real = playsAsRealUser(user);
        status[i].userId = user->userId;
        status[i].realId = user->isConfiguredId ? user->realId : 0;
        status[i].enabled = user->enabled;
        status[i].real = real;
        status[i].logged = real || user->slot >= 0;
        status[i].joined = real || user->joined || (userService->joinOnStart && !user->left);
        status[i].connected = isPadConnected(i);
        status[i].shared = sharesController(user);
        // A guest that wants to play but found every player position taken
        status[i].full = !real && user->enabled && status[i].joined && userService->hasLoginList() && user->slot < 0;
        status[i].player = user->slot + 1;
        snprintf(status[i].name, sizeof(status[i].name), "%.16s", user->userName);
    }

    for (struct mg_connection *t = ctx->mgr.conns; t != NULL; t = t->next) {
        if (!t->is_websocket || t->is_closing)
            continue;
        for (int i = 0; i < REMOTE_PAD_MAX_PADS; i++) {
            if (clientOf(t)->pads & (1 << i))
                status[i].clients++;
        }
    }
}

// %M printer for the status object
static size_t printStatus(void (*out)(char, void *), void *ptr, va_list *ap) {
    const wsPadStatus *status = va_arg(*ap, const wsPadStatus *);
    size_t n = mg_xprintf(out, ptr, "{%m:%m,%m:%d,%m:[", MG_ESC("version"), MG_ESC(STR(BUILD_TAG_VERSION)),
                          MG_ESC("protocol"), WS_PROTOCOL_VERSION, MG_ESC("pads"));
    for (int i = 0; i < REMOTE_PAD_MAX_PADS; i++) {
        const wsPadStatus *s = &status[i];
        char uid[12];
        char cfg[12] = "";
        snprintf(uid, sizeof(uid), "%08X", (uint32_t) s->userId);
        if (s->realId != 0)
            snprintf(cfg, sizeof(cfg), "%08X", (uint32_t) s->realId);
        n += mg_xprintf(out, ptr, "%s{%m:%d,%m:%s,%m:%s,%m:%s,%m:%s,%m:%s,%m:%s,%m:%s,%m:%s,%m:%d,%m:%m,%m:%m,%m:%m,%m:[%d,%d,%d],%m:%d}",
                        i == 0 ? "" : ",",
                        MG_ESC("pad"), i,
                        MG_ESC("open"), s->open ? "true" : "false",
                        MG_ESC("enabled"), s->enabled ? "true" : "false",
                        MG_ESC("real"), s->real ? "true" : "false",
                        MG_ESC("logged"), s->logged ? "true" : "false",
                        MG_ESC("joined"), s->joined ? "true" : "false",
                        MG_ESC("connected"), s->connected ? "true" : "false",
                        MG_ESC("full"), s->full ? "true" : "false",
                        MG_ESC("shared"), s->shared ? "true" : "false",
                        MG_ESC("player"), s->player,
                        MG_ESC("uid"), MG_ESC(uid),
                        MG_ESC("cfg"), MG_ESC(cfg),
                        MG_ESC("name"), MG_ESC(s->name),
                        MG_ESC("color"), s->color[0], s->color[1], s->color[2],
                        MG_ESC("clients"), s->clients);
    }
    n += mg_xprintf(out, ptr, "]}");
    return n;
}

static void broadcastStatus(wsDriverData *ctx) {
    wsPadStatus status[REMOTE_PAD_MAX_PADS];
    getStatus(ctx, status);
    for (struct mg_connection *t = ctx->mgr.conns; t != NULL; t = t->next) {
        if (!t->is_websocket || t->is_closing)
            continue;
        mg_ws_printf(t, WEBSOCKET_OP_TEXT, "{%m:%m,%m:%M}", MG_ESC("method"), MG_ESC("s"), MG_ESC("params"),
                     printStatus, status);
    }
}

static bool padControlledByOthers(wsDriverData *ctx, struct mg_connection *self, int pad) {
    for (struct mg_connection *t = ctx->mgr.conns; t != NULL; t = t->next) {
        if (t != self && t->is_websocket && !t->is_closing && (clientOf(t)->pads & (1 << pad)))
            return true;
    }
    return false;
}

// Nobody holds these pads anymore: let go of every button so nothing stays pressed
static void releasePads(wsDriverData *ctx, struct mg_connection *self, uint8_t pads) {
    for (int i = 0; i < REMOTE_PAD_MAX_PADS; i++) {
        if (!(pads & (1 << i)) || padControlledByOthers(ctx, self, i))
            continue;
        OrbisPadData data;
        emptyPadData(&data);
        pushPadData(i, &data);
    }
}

// Tell the pad/user services which pads have a device now (the game sees the others as disconnected,
// and a guest player joins the game when its pad gets its first device), then update every client
static void refreshDevices(wsDriverData *ctx) {
    RemotePadService *padService = getRemotePadService();
    RemoteUserService *userService = getRemoteUserService();
    for (int i = 0; i < REMOTE_PAD_MAX_PADS; i++) {
        bool used = padControlledByOthers(ctx, NULL, i);
        padService->setDeviceConnected(i, used);
        if (used)
            userService->join(i);
    }
    broadcastStatus(ctx);
}

static void ws_wakeup_fn(struct mg_connection *c, int ev, void *ev_data) {
    wsDriverData *ctx = (wsDriverData *) c->fn_data;
    if (ev == MG_EV_READ) {
        // MG_INFO(("Got data"));
        // mg_hexdump(c->recv.buf, c->recv.len);
        if (c->recv.len >= 2 && c->recv.buf[0] == WS_WAKEUP_STATUS) {
            broadcastStatus(ctx);
        } else if (c->recv.len >= 2) {
            struct mg_connection *t;
            for (t = c->mgr->conns; t != NULL; t = t->next) {
                if (t->is_websocket && !t->is_closing) {
                    struct mg_str data = mg_str_n((char *) c->recv.buf, c->recv.len);
                    mg_call(t, MG_EV_WAKEUP, &data);
                }
            }
        }
        c->recv.len = 0;  // Consume received data
    } else if (ev == MG_EV_CLOSE) {
        // Game threads must stop using the sending side before it is closed
        scePthreadMutexLock(&ctx->wakeupMutex);
        ctx->wakeupFd = -1;
        scePthreadMutexUnlock(&ctx->wakeupMutex);
        close(c->mgr->pipe);         // When we're closing, close the other
        c->mgr->pipe = MG_INVALID_SOCKET;  // side of the socketpair, too
    }
    (void) ev_data;
}

static bool ws_wakeup_init(wsDriverData *ctx) {
    struct mg_mgr *mgr = &ctx->mgr;
    bool ok = false;
    if (mgr->pipe == MG_INVALID_SOCKET) {
        union usa usa[2];
        MG_SOCKET_TYPE sp[2] = {MG_INVALID_SOCKET, MG_INVALID_SOCKET};
        struct mg_connection *c = NULL;
        if (!mg_socketpair(sp, usa)) {
            MG_ERROR(("Cannot create socket pair"));
        } else if ((c = mg_wrapfd(mgr, (int) sp[1], ws_wakeup_fn, ctx)) == NULL) {
            close(sp[0]);
            close(sp[1]);
            sp[0] = sp[1] = MG_INVALID_SOCKET;
        } else {
            tomgaddr(&usa[0], &c->rem, false);
            MG_DEBUG(("%lu %p pipe %lu", c->id, c->fd, (unsigned long) sp[0]));
            mgr->pipe = sp[0];
            scePthreadMutexLock(&ctx->wakeupMutex);
            ctx->wakeupFd = (int) sp[0];
            scePthreadMutexUnlock(&ctx->wakeupMutex);
            ok = true;
        }
    }
    return ok;
}

// Hand an event from a game thread to the server thread
static bool ws_wakeup(wsDriverData *ctx, uint8_t type, uint8_t index, const void *buf, size_t len) {
    char msg[2 + sizeof(OrbisPadColor) + sizeof(OrbisPadVibeParam)];
    bool sent = false;
    if (!__atomic_load_n(&ctx->mutexReady, __ATOMIC_ACQUIRE) || len > sizeof(msg) - 2)
        return false;
    msg[0] = (char) type;
    msg[1] = (char) index;
    if (len > 0)
        memcpy(msg + 2, buf, len);
    scePthreadMutexLock(&ctx->wakeupMutex);
    if (ctx->wakeupFd >= 0)
        sent = send(ctx->wakeupFd, msg, len + 2, 0) >= 0;
    scePthreadMutexUnlock(&ctx->wakeupMutex);
    return sent;
}

// Needed by mongoose errno() function
int *attr_public __errno_location(void) {
    int *__error(void);
    return __error();
}

static const char *s_listen_on = "ws://0.0.0.0:" REMOTE_PAD_WS_PORT;

static OrbisPthread serverThread;
static wsDriverData globalDriverData = {
        .rpc_head = NULL,
        .wakeupFd = -1,
        .mutexReady = false,
        .threadStarted = false,
        .running = false
};

static void notifyServerAddress(void) {
    struct ifaddrs *ifaddr;
    char ip[INET_ADDRSTRLEN];
    if (getifaddrs(&ifaddr) == -1) {
        final_printf("Failed to get network interfaces\n");
        return;
    }

    // Enumerate all AF_INET IPs
    for (struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) {
            continue;
        }

        if (ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }

        struct sockaddr_in *in = (struct sockaddr_in *) ifa->ifa_addr;
        inet_ntop(AF_INET, &(in->sin_addr), ip, sizeof(ip));

        if (strncmp(ip, "127.0.0.1", sizeof(ip)) == 0) {
            continue;
        }

        // The full address to type, for people who have never used the plugin
        Notify(TEX_ICON_SYSTEM, "RemotePad NG\nOpen http://%s:" REMOTE_PAD_WS_PORT " on your phone or PC", ip);
    }
    freeifaddrs(ifaddr);
}

static inline long clampLong(long val, long min, long max) {
    if (val < min) return min;
    if (val > max) return max;
    return val;
}

static void rpc_update(struct mg_rpc_req *r) {
    wsDriverData *ctx = (wsDriverData *) r->rpc->fn_data;
    struct mg_connection *c = (struct mg_connection *) r->req_data;
    long index = mg_json_get_long(r->frame, "$.params[0]", -1);
    if (index < 0 || index >= REMOTE_PAD_MAX_PADS)
        return;

    uint32_t button = (uint32_t) mg_json_get_long(r->frame, "$.params[1]", 0) & PAD_VALID_BUTTONS;
    long leftStickX = clampLong(mg_json_get_long(r->frame, "$.params[2]", 128), 0, 255);
    long leftStickY = clampLong(mg_json_get_long(r->frame, "$.params[3]", 128), 0, 255);
    long rightStickX = clampLong(mg_json_get_long(r->frame, "$.params[4]", 128), 0, 255);
    long rightStickY = clampLong(mg_json_get_long(r->frame, "$.params[5]", 128), 0, 255);
    long left2 = clampLong(mg_json_get_long(r->frame, "$.params[6]", 0), 0, 255);
    long right2 = clampLong(mg_json_get_long(r->frame, "$.params[7]", 0), 0, 255);
    long touchNum = clampLong(mg_json_get_long(r->frame, "$.params[8]", 0), 0, 2);

    OrbisPadData padData;
    emptyPadData(&padData);
    padData.buttons = button;
    padData.leftStick.x = (uint8_t) leftStickX;
    padData.leftStick.y = (uint8_t) leftStickY;
    padData.rightStick.x = (uint8_t) rightStickX;
    padData.rightStick.y = (uint8_t) rightStickY;
    padData.analogButtons.l2 = (uint8_t) left2;
    padData.analogButtons.r2 = (uint8_t) right2;
    padData.touch.fingers = (uint8_t) touchNum;
    for (int i = 0; i < touchNum; i++) {
        char path[16];
        snprintf(path, sizeof(path), "$.params[%d]", 9 + i * 3);
        padData.touch.touch[i].finger = (uint8_t) clampLong(mg_json_get_long(r->frame, path, 0), 0, 127);
        snprintf(path, sizeof(path), "$.params[%d]", 10 + i * 3);
        padData.touch.touch[i].x = (uint16_t) clampLong(mg_json_get_long(r->frame, path, 0), 0, 1919);
        snprintf(path, sizeof(path), "$.params[%d]", 11 + i * 3);
        padData.touch.touch[i].y = (uint16_t) clampLong(mg_json_get_long(r->frame, path, 0), 0, 941);
    }

    // Push the data to the pad circular buffer
    pushPadData(index, &padData);

    // Sending input for a pad means controlling it (protocol 1 clients never send "claim")
    if (c != NULL && !(clientOf(c)->pads & (1 << index))) {
        clientOf(c)->pads |= (uint8_t) (1 << index);
        refreshDevices(ctx);
    }
}

// {"id":1,"method":"claim","params":[0,2]}: this client controls exactly these pads ([] for none).
// Pads nobody controls anymore are released. Replies with the status object.
static void rpc_claim(struct mg_rpc_req *r) {
    wsDriverData *ctx = (wsDriverData *) r->rpc->fn_data;
    struct mg_connection *c = (struct mg_connection *) r->req_data;
    uint8_t pads = 0;
    if (c == NULL)
        return;
    for (int i = 0; i < REMOTE_PAD_MAX_PADS; i++) {
        char path[16];
        snprintf(path, sizeof(path), "$.params[%d]", i);
        long pad = mg_json_get_long(r->frame, path, -1);
        if (pad < 0)
            break;
        if (pad < REMOTE_PAD_MAX_PADS)
            pads |= (uint8_t) (1 << pad);
    }

    wsClient *client = clientOf(c);
    uint8_t released = client->pads & (uint8_t) ~pads;
    client->pads = pads;
    client->claimed = true;
    client->stale = false;
    releasePads(ctx, c, released);
    refreshDevices(ctx);

    wsPadStatus status[REMOTE_PAD_MAX_PADS];
    getStatus(ctx, status);
    mg_rpc_ok(r, "%M", printStatus, status);
}

// {"id":1,"method":"leave","params":[2]}: the guest player of pad 2 leaves the game (sign-out). This client
// stops controlling the pad. Refused while another device still uses it. Replies with the status object.
static void rpc_leave(struct mg_rpc_req *r) {
    wsDriverData *ctx = (wsDriverData *) r->rpc->fn_data;
    struct mg_connection *c = (struct mg_connection *) r->req_data;
    long pad = mg_json_get_long(r->frame, "$.params[0]", -1);
    if (c == NULL)
        return;
    if (pad < 0 || pad >= REMOTE_PAD_MAX_PADS) {
        mg_rpc_err(r, 400, "%m", MG_ESC("no such pad"));
        return;
    }
    if (padControlledByOthers(ctx, c, (int) pad)) {
        mg_rpc_err(r, 409, "%m", MG_ESC("another device is using this pad"));
        return;
    }

    uint8_t bit = (uint8_t) (1 << pad);
    if (clientOf(c)->pads & bit) {
        clientOf(c)->pads &= (uint8_t) ~bit;
        releasePads(ctx, c, bit);
    }
    getRemoteUserService()->leave((int32_t) pad);
    refreshDevices(ctx);

    wsPadStatus status[REMOTE_PAD_MAX_PADS];
    getStatus(ctx, status);
    mg_rpc_ok(r, "%M", printStatus, status);
}

// %M printer for the settings object (with the console's users, for choosing a pad's PS4 user)
static size_t printSettings(void (*out)(char, void *), void *ptr, va_list *ap) {
    const Settings *settings = va_arg(*ap, const Settings *);
    bool gameOnly = (bool) va_arg(*ap, int);
    const ConsoleUser *users = va_arg(*ap, const ConsoleUser *);
    int userCount = va_arg(*ap, int);
    size_t n = mg_xprintf(out, ptr, "{%m:%m,%m:%s,%m:%s,%m:%s,%m:%s,%m:[", MG_ESC("title"), MG_ESC(settingsTitleId()),
                          MG_ESC("locked"), settingsLocked() ? "true" : "false",
                          MG_ESC("gameOnly"), gameOnly ? "true" : "false",
                          MG_ESC("joinOnStart"), settings->joinOnStart ? "true" : "false",
                          MG_ESC("alwaysConnected"), settings->alwaysConnected ? "true" : "false",
                          MG_ESC("pads"));
    for (int i = 0; i < REMOTE_PAD_MAX_PADS; i++) {
        const PadSettings *pad = &settings->pads[i];
        char id[12] = "";
        if (pad->realId != 0)
            snprintf(id, sizeof(id), "%08X", (uint32_t) pad->realId);
        n += mg_xprintf(out, ptr, "%s{%m:%s,%m:%m,%m:%s,%m:%m}", i == 0 ? "" : ",",
                        MG_ESC("enabled"), pad->enabled ? "true" : "false",
                        MG_ESC("id"), MG_ESC(id),
                        MG_ESC("share"), pad->share ? "true" : "false",
                        MG_ESC("name"), MG_ESC(pad->name));
    }
    n += mg_xprintf(out, ptr, "],%m:[", MG_ESC("users"));
    for (int i = 0; i < userCount; i++) {
        char id[12];
        snprintf(id, sizeof(id), "%08X", (uint32_t) users[i].id);
        n += mg_xprintf(out, ptr, "%s{%m:%m,%m:%m,%m:%s}", i == 0 ? "" : ",",
                        MG_ESC("id"), MG_ESC(id),
                        MG_ESC("name"), MG_ESC(users[i].name),
                        MG_ESC("signedIn"), users[i].signedIn ? "true" : "false");
    }
    n += mg_xprintf(out, ptr, "]}");
    return n;
}

static void replySettings(struct mg_rpc_req *r, const char *extraKey, bool extraValue) {
    Settings settings;
    bool gameOnly;
    ConsoleUser users[SETTINGS_MAX_CONSOLE_USERS];
    settingsGet(&settings, &gameOnly);
    int userCount = settingsListConsoleUsers(users, SETTINGS_MAX_CONSOLE_USERS);
    if (extraKey != NULL)
        mg_rpc_ok(r, "{%m:%s,%m:%M}", MG_ESC(extraKey), extraValue ? "true" : "false", MG_ESC("settings"),
                  printSettings, &settings, (int) gameOnly, users, userCount);
    else
        mg_rpc_ok(r, "%M", printSettings, &settings, (int) gameOnly, users, userCount);
}

// {"id":1,"method":"settings"}: the settings in effect and the console's users
static void rpc_settings(struct mg_rpc_req *r) {
    replySettings(r, NULL, false);
}

// {"id":1,"method":"settings.set","params":{"gameOnly":false,"joinOnStart":false,"alwaysConnected":false,
//   "pads":[{"enabled":true,"id":"0x1A2B3C4D","share":false,"name":"Alex"}, ...]}}
// Leaving a field out keeps its value. Applies right away and saves remote_pad.ini.
// Replies {"saved":true,"settings":{...}}.
static void rpc_settings_set(struct mg_rpc_req *r) {
    wsDriverData *ctx = (wsDriverData *) r->rpc->fn_data;
    Settings settings;
    bool gameOnly;
    bool saved = false;
    char error[128];
    char path[40];

    if (!mayConfigure(r))
        return;
    settingsGet(&settings, &gameOnly);
    mg_json_get_bool(r->frame, "$.params.gameOnly", &gameOnly);
    mg_json_get_bool(r->frame, "$.params.joinOnStart", &settings.joinOnStart);
    mg_json_get_bool(r->frame, "$.params.alwaysConnected", &settings.alwaysConnected);
    for (int i = 0; i < REMOTE_PAD_MAX_PADS; i++) {
        PadSettings *pad = &settings.pads[i];
        char *value;

        snprintf(path, sizeof(path), "$.params.pads[%d].enabled", i);
        mg_json_get_bool(r->frame, path, &pad->enabled);
        snprintf(path, sizeof(path), "$.params.pads[%d].share", i);
        mg_json_get_bool(r->frame, path, &pad->share);

        snprintf(path, sizeof(path), "$.params.pads[%d].id", i);
        if ((value = mg_json_get_str(r->frame, path)) != NULL) {
            int32_t userId = 0;
            if (value[0] != '\0' && !ini_parse_user_id(value, &userId))
                userId = -1; // rejected by settingsSet
            pad->realId = userId;
            free(value);
        }

        snprintf(path, sizeof(path), "$.params.pads[%d].name", i);
        if ((value = mg_json_get_str(r->frame, path)) != NULL) {
            if (strlen(value) >= sizeof(pad->name)) {
                free(value);
                mg_rpc_err(r, 400, "%m", MG_ESC("A name can have at most 16 characters"));
                return;
            }
            strcpy(pad->name, value);
            free(value);
        }
    }

    if (!settingsSet(&settings, gameOnly, &saved, error, sizeof(error))) {
        mg_rpc_err(r, 400, "%m", MG_ESC(error));
        return;
    }
    replySettings(r, "saved", saved);
    broadcastStatus(ctx);
}

static void rpc_status(struct mg_rpc_req *r) {
    wsPadStatus status[REMOTE_PAD_MAX_PADS];
    getStatus((wsDriverData *) r->rpc->fn_data, status);
    mg_rpc_ok(r, "%M", printStatus, status);
}

static void rpc_notify(struct mg_rpc_req *r) {
    if (!mayConfigure(r))
        return;
    char *text = mg_json_get_str(r->frame, "$.params[0]");
    if (text != NULL)
        Notify(TEX_ICON_SYSTEM, "%s", text);
    free(text);
}

static void rpc_info(struct mg_rpc_req *r) {
    mg_rpc_ok(r, "{%m:%m,%m:%d,%m:%d}", MG_ESC("version"), MG_ESC(STR(BUILD_TAG_VERSION)),
              MG_ESC("protocol"), WS_PROTOCOL_VERSION, MG_ESC("pads"), REMOTE_PAD_MAX_PADS);
}

// Vibration only goes to the clients controlling the pad (and to protocol 1 clients that sent no input yet)
static bool wantsPad(struct mg_connection *c, uint8_t pad) {
    wsClient *client = clientOf(c);
    if (client->pads & (1 << pad))
        return true;
    return !client->claimed && client->pads == 0;
}

static void fn(struct mg_connection *c, int ev, void *ev_data) {
    wsDriverData *ctx = (wsDriverData *) c->fn_data;
    if (ev == MG_EV_ACCEPT) {
        size_t connections = 0;
        for (struct mg_connection *t = c->mgr->conns; t != NULL; t = t->next) {
            if (t->is_accepted && !t->is_closing)
                connections++;
        }
        if (connections > WS_MAX_CONNECTIONS) {
            final_printf("[RemotePad] more than %d connections, closing the new one\n", WS_MAX_CONNECTIONS);
            c->is_closing = 1;
            return;
        }
        // Small frames both ways: don't let Nagle hold vibration and lightbar messages back
        int on = 1;
        setsockopt((int) (size_t) c->fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    } else if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;
        // Header values are case-insensitive: some clients send "connection: upgrade"
        struct mg_str *upgrade = mg_http_get_header(hm, "Upgrade");
        if (upgrade && mg_strcasecmp(*upgrade, mg_str("websocket")) == 0) {
            if (wsAccessFor(hm) == WS_ACCESS_DENIED) {
                mg_http_reply(c, 403, "", "This page comes from another site\n");
                c->is_draining = 1;
                return;
            }
            mg_ws_upgrade(c, hm, NULL);
            return;
        }
        // Serve static files
        struct mg_http_serve_opts opts = {.root_dir = "/client", .fs = &mg_fs_packed, .extra_headers = WS_PAGE_HEADERS};
        mg_http_serve_dir(c, ev_data, &opts);
    } else if (ev == MG_EV_WS_OPEN) {
        // ev_data is the handshake request
        memset(clientOf(c), 0, sizeof(wsClient));
        clientOf(c)->lastSeen = (uint32_t) mg_millis();
        clientOf(c)->canConfigure = wsAccessFor((struct mg_http_message *) ev_data) == WS_ACCESS_FULL;
    } else if (ev == MG_EV_WS_MSG) {
        // Got websocket frame. Received data is wm->data
        struct mg_ws_message *wm = (struct mg_ws_message *) ev_data;
        clientOf(c)->lastSeen = (uint32_t) mg_millis();
        clientOf(c)->stale = false;
        struct mg_iobuf io = {0, 0, 0, 512};
        struct mg_rpc_req r = {&ctx->rpc_head, 0, mg_pfn_iobuf, &io, c, wm->data};
        mg_rpc_process(&r);
        if (io.buf) mg_ws_send(c, (char *) io.buf, io.len, WEBSOCKET_OP_TEXT);
        mg_iobuf_free(&io);
    } else if (ev == MG_EV_WAKEUP) {
        struct mg_str *data = (struct mg_str *) ev_data;
        uint8_t type = *((uint8_t *) data->buf);
        uint8_t index = *((uint8_t *) (data->buf + 1));
        void *buf = data->buf + 2;
        if (index >= REMOTE_PAD_MAX_PADS || c->send.len > WS_MAX_PENDING_SEND)
            return;
        switch (type) {
            case WS_WAKEUP_VIBRATION: {
                if (data->len < 2 + sizeof(OrbisPadVibeParam) || !wantsPad(c, index))
                    break;
                OrbisPadVibeParam *param = (OrbisPadVibeParam *) buf;
                mg_ws_printf(c, WEBSOCKET_OP_TEXT, "{%m:%m,%m:[%d,%d,%d]}",
                             MG_ESC("method"), MG_ESC("v"), MG_ESC("params"),
                             index, param->lgMotor, param->smMotor);
                break;
            }
            case WS_WAKEUP_LIGHTBAR: {
                if (data->len < 2 + sizeof(OrbisPadColor))
                    break;
                OrbisPadColor *color = (OrbisPadColor *) buf;
                mg_ws_printf(c, WEBSOCKET_OP_TEXT, "{%m:%m,%m:[%d,%d,%d,%d]}",
                             MG_ESC("method"), MG_ESC("l"), MG_ESC("params"),
                             index, color->r, color->g, color->b);
                break;
            }
            case WS_WAKEUP_RESET_LIGHTBAR: {
                // The pad goes back to its player color; protocol 1 clients ignore the color
                RemoteUser *user = getRemoteUserService()->getUserByIndex(index);
                const uint8_t *color = userLightBarColors[user->color % 4];
                mg_ws_printf(c, WEBSOCKET_OP_TEXT, "{%m:%m,%m:[%d,%d,%d,%d]}",
                             MG_ESC("method"), MG_ESC("rl"), MG_ESC("params"),
                             index, color[0], color[1], color[2]);
                break;
            }
            default:
                break;
        }
    } else if (ev == MG_EV_CLOSE) {
        if (c->is_websocket) {
            uint8_t pads = clientOf(c)->pads;
            clientOf(c)->pads = 0;
            if (pads != 0) {
                releasePads(ctx, c, pads);
                refreshDevices(ctx);
            }
        }
    }
}

static void wsWatchdog(void *arg) {
    wsDriverData *ctx = (wsDriverData *) arg;
    uint32_t now = (uint32_t) mg_millis();
    bool changed = false;
    for (struct mg_connection *t = ctx->mgr.conns; t != NULL; t = t->next) {
        if (!t->is_websocket || t->is_closing)
            continue;
        if (t->send.len > WS_MAX_SEND_BUFFER) {
            final_printf("[RemotePad] client %lu stopped reading, closing it\n", t->id);
            t->is_closing = 1;
            continue;
        }
        wsClient *client = clientOf(t);
        if (client->claimed && client->pads != 0 && !client->stale &&
            now - client->lastSeen > WS_CLIENT_TIMEOUT_MS) {
            // e.g. a phone that went to sleep with a button held
            final_printf("[RemotePad] client %lu went quiet, releasing its pads\n", t->id);
            uint8_t pads = client->pads;
            client->pads = 0;
            client->stale = true;
            releasePads(ctx, t, pads);
            changed = true;
        }
    }
    if (changed)
        refreshDevices(ctx);
}

static void *websocketThread(void *thread_arg) {
    wsDriverData *ctx = (wsDriverData *) thread_arg;
    mg_mgr_init(&ctx->mgr);
    mg_log_set(MG_LL_ERROR);
    if (!ws_wakeup_init(ctx))
        final_printf("[RemotePad] wakeup pipe failed: no vibration/lightbar/status updates\n");

    mg_rpc_add(&ctx->rpc_head, mg_str("u"), rpc_update, ctx);
    mg_rpc_add(&ctx->rpc_head, mg_str("claim"), rpc_claim, ctx);
    mg_rpc_add(&ctx->rpc_head, mg_str("leave"), rpc_leave, ctx);
    mg_rpc_add(&ctx->rpc_head, mg_str("settings.set"), rpc_settings_set, ctx);
    mg_rpc_add(&ctx->rpc_head, mg_str("settings"), rpc_settings, ctx);
    mg_rpc_add(&ctx->rpc_head, mg_str("status"), rpc_status, ctx);
    mg_rpc_add(&ctx->rpc_head, mg_str("notify"), rpc_notify, ctx);
    mg_rpc_add(&ctx->rpc_head, mg_str("info"), rpc_info, ctx);
    mg_rpc_add(&ctx->rpc_head, mg_str("rpc.list"), mg_rpc_list, &ctx->rpc_head);

    final_printf("Starting WS listener on %s\n", s_listen_on);
    if (mg_http_listen(&ctx->mgr, s_listen_on, fn, ctx) == NULL) {
        final_printf("[RemotePad] cannot listen on %s\n", s_listen_on);
        Notify(TEX_ICON_SYSTEM, "RemotePad NG: port " REMOTE_PAD_WS_PORT " is not available");
        __atomic_store_n(&ctx->running, false, __ATOMIC_RELEASE);
    } else {
        mg_timer_add(&ctx->mgr, 1000, MG_TIMER_REPEAT, wsWatchdog, ctx);
        notifyServerAddress();
    }
    while (__atomic_load_n(&ctx->running, __ATOMIC_ACQUIRE)) {
        mg_mgr_poll(&ctx->mgr, 4);
    }

    scePthreadMutexLock(&ctx->wakeupMutex);
    ctx->wakeupFd = -1;
    scePthreadMutexUnlock(&ctx->wakeupMutex);
    mg_mgr_free(&ctx->mgr);
    mg_rpc_del(&ctx->rpc_head, NULL);
    return NULL;
}

static int32_t wsSetLightBar(RemotePad *pad, OrbisPadColor *inputColor) {
    wsDriverData *ctx = pad->driver->data;
    if (ctx->lightBarSet[pad->index] && !ctx->lightBarReset[pad->index] &&
        memcmp(inputColor, &ctx->lastColor[pad->index], sizeof(OrbisPadColor)) == 0) {
        return 0;
    }
    memcpy(&ctx->lastColor[pad->index], inputColor, sizeof(OrbisPadColor));
    ctx->lightBarSet[pad->index] = true;
    ctx->lightBarReset[pad->index] = false;
    ws_wakeup(ctx, WS_WAKEUP_LIGHTBAR, pad->index, inputColor, sizeof(OrbisPadColor));
    return 0;
}

static int32_t wsResetLightBar(RemotePad *pad) {
    wsDriverData *ctx = pad->driver->data;
    if (ctx->lightBarReset[pad->index]) {
        return 0;
    }
    ctx->lightBarReset[pad->index] = true;
    ws_wakeup(ctx, WS_WAKEUP_RESET_LIGHTBAR, pad->index, NULL, 0);
    return 0;
}

static int32_t wsSetVibration(RemotePad *pad, const OrbisPadVibeParam *param) {
    wsDriverData *ctx = pad->driver->data;
    if (memcmp(param, &ctx->lastVibration[pad->index], sizeof(OrbisPadVibeParam)) == 0 &&
        param->lgMotor == 0 && param->smMotor == 0) {
        // Some games keep sending empty vibration data. Ignore it to reduce traffic.
        return 0;
    }
    memcpy(&ctx->lastVibration[pad->index], param, sizeof(OrbisPadVibeParam));
    ws_wakeup(ctx, WS_WAKEUP_VIBRATION, pad->index, param, sizeof(OrbisPadVibeParam));
    return 0;
}

static void wsStatusChanged(RemotePadDriverPtr driver) {
    ws_wakeup(driver->data, WS_WAKEUP_STATUS, 0, NULL, 0);
}

static int32_t wsInit(RemotePadDriverPtr driver) {
    wsDriverData *ctx = driver->data;
    OrbisPthreadAttr attr;
    int ret = 0;
    if (ctx->threadStarted) {
        final_printf("[RemotePad]: ws driver is already initialized\n");
        return 0;
    }

    if (!ctx->mutexReady) {
        ret = scePthreadMutexInit(&ctx->wakeupMutex, 0, "wsWakeMtx");
        if (ret < 0) {
            final_printf("[RemotePad]: failed to init the wakeup mutex, 0x%X\n", ret);
            return ret;
        }
        __atomic_store_n(&ctx->mutexReady, true, __ATOMIC_RELEASE);
    }

    __atomic_store_n(&ctx->running, true, __ATOMIC_RELEASE);
    scePthreadAttrInit(&attr);
    scePthreadAttrSetstacksize(&attr, WS_THREAD_STACK_SIZE);
    ret = scePthreadCreate(&serverThread, &attr, websocketThread, ctx, "WsSrvThr");
    scePthreadAttrDestroy(&attr);
    if (ret < 0) {
        __atomic_store_n(&ctx->running, false, __ATOMIC_RELEASE);
        final_printf("[RemotePad]: Failed to spawn a ws server thread, 0x%X\n", ret);
        return ret;
    }
    ctx->threadStarted = true;
    return 0;
}

static int32_t wsTerm(RemotePadDriverPtr driver) {
    wsDriverData *ctx = driver->data;
    if (!ctx)
        return -1;

    if (ctx->threadStarted) {
        __atomic_store_n(&ctx->running, false, __ATOMIC_RELEASE);
        scePthreadJoin(serverThread, 0);
        ctx->threadStarted = false;
    }
    if (ctx->mutexReady) {
        __atomic_store_n(&ctx->mutexReady, false, __ATOMIC_RELEASE);
        scePthreadMutexDestroy(&ctx->wakeupMutex);
    }

    return 0;
}

const struct RemotePadDriver wsDriver = {
        .name = "websocket",
        .init = wsInit,
        .term = wsTerm,
        .setLightBar = wsSetLightBar,
        .resetLightBar = wsResetLightBar,
        .setVibration = wsSetVibration,
        .statusChanged = wsStatusChanged,
        .data = &globalDriverData
};

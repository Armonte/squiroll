#if __INTELLISENSE__
#undef _HAS_CXX20
#define _HAS_CXX20 0
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits>
#include <charconv>
#include <system_error>
#include <atomic>
#include <unordered_map>

#include <windows.h>

#include "util.h"
#include "config.h"
#include "log.h"
#include "shared.h"

static bool use_config = false;
static char CONFIG_FILE_PATH[MAX_PATH];

static constexpr char CONFIG_FILE_NAME[] = "netcode.ini";
static constexpr char TASOFRO_CONFIG_FILE_NAME[] = "config.ini";

#define CONFIG_STR(SECTION, KEY, NAME, DEFAULT) MACRO_VOID2(__COUNTER__) \
static inline constexpr const char MACRO_CAT4(SECTION,_,KEY,_KEY)[] = NAME; \
static inline constexpr const char MACRO_CAT4(SECTION,_,KEY,_DEFAULT)[] = DEFAULT; \
static inline constexpr const char MACRO_CAT4(SECTION,_,KEY,_DEFAULT_STR)[] = DEFAULT
#define CONFIG_BOL(SECTION, KEY, NAME, DEFAULT) MACRO_VOID2(__COUNTER__) \
static inline constexpr const char MACRO_CAT4(SECTION,_,KEY,_KEY)[] = NAME; \
static inline constexpr bool MACRO_CAT4(SECTION,_,KEY,_DEFAULT) = DEFAULT; \
static inline constexpr const char MACRO_CAT4(SECTION,_,KEY,_DEFAULT_STR)[] = MACRO_STR(DEFAULT)
#define CONFIG_TST(SECTION, KEY, NAME) MACRO_VOID2(__COUNTER__) \
static inline constexpr const char MACRO_CAT4(SECTION,_,KEY,_KEY)[] = NAME; \
static inline constexpr const char MACRO_CAT4(SECTION,_,KEY,_DEFAULT)[] = "maybe"; \
static inline constexpr const char MACRO_CAT4(SECTION,_,KEY,_DEFAULT_STR)[] = "maybe"
#define CONFIG_INT(SECTION, KEY, NAME, DEFAULT) MACRO_VOID2(__COUNTER__) \
static inline constexpr const char MACRO_CAT4(SECTION,_,KEY,_KEY)[] = NAME; \
static inline constexpr int32_t MACRO_CAT4(SECTION,_,KEY,_DEFAULT) = DEFAULT; \
static inline constexpr const char MACRO_CAT4(SECTION,_,KEY,_DEFAULT_STR)[] = MACRO_STR(DEFAULT)
#define CONFIG_HEX(SECTION, KEY, NAME, DEFAULT) MACRO_VOID2(__COUNTER__) \
static inline constexpr const char MACRO_CAT4(SECTION,_,KEY,_KEY)[] = NAME; \
static inline constexpr int32_t MACRO_CAT4(SECTION,_,KEY,_DEFAULT) = MACRO_CAT(0x,DEFAULT); \
static inline constexpr const char MACRO_CAT4(SECTION,_,KEY,_DEFAULT_STR)[] = MACRO_STR(DEFAULT)
#define CONFIG_FLT(SECTION, KEY, NAME, DEFAULT) MACRO_VOID2(__COUNTER__) \
static inline constexpr const char MACRO_CAT4(SECTION,_,KEY,_KEY)[] = NAME; \
static inline constexpr float MACRO_CAT4(SECTION,_,KEY,_DEFAULT) = MACRO_CAT(DEFAULT,f); \
static inline constexpr const char MACRO_CAT4(SECTION,_,KEY,_DEFAULT_STR)[] = MACRO_STR(DEFAULT)

static inline constexpr auto START_COUNTER = __COUNTER__;
// DO NOT DEFINE NORMAL CONFIGS ABOVE THIS LINE

#define LOBBY_SECTION_NAME "lobby"
#if !USE_DEV_SERVER
CONFIG_STR(LOBBY, HOST, "lobby_server", "squiroll.justapenguin.ca");
CONFIG_STR(LOBBY, PORT, "lobby_port", "5001");
#else
CONFIG_STR(LOBBY, HOST, "lobby_server", "waluigistacostand.ddns.net");
CONFIG_STR(LOBBY, PORT, "lobby_port", "1550");
#endif
CONFIG_STR(LOBBY, PASS, "lobby_password", "kzxmckfqbpqieh8rw<rczuturKfnsjxhauhybttboiuuzmWdmnt5mnlczpythaxf");

#define HITBOX_VIS_SECTION_NAME "hitbox_vis"
CONFIG_BOL(HITBOX_VIS, ENABLED, "enabled", false);
CONFIG_INT(HITBOX_VIS, BORDER_WIDTH, "border_width", 2);
CONFIG_FLT(HITBOX_VIS, INNER_ALPHA, "inner_alpha", 0.25);
CONFIG_FLT(HITBOX_VIS, BORDER_ALPHA, "border_alpha", 1.0);
CONFIG_HEX(HITBOX_VIS, COLLISION_COLOR, "collision_color", FF1E90FF);
CONFIG_HEX(HITBOX_VIS, HIT_COLOR, "hit_color", FFFF0000);
CONFIG_HEX(HITBOX_VIS, PLAYER_HURT_COLOR, "player_hurt_color", FF00FF00);
CONFIG_HEX(HITBOX_VIS, PLAYER_UNHIT_COLOR, "player_unhit_color", FFFF5F1F);
CONFIG_HEX(HITBOX_VIS, PLAYER_UNGRAB_COLOR, "player_ungrab_color", FFFF00FF);
CONFIG_HEX(HITBOX_VIS, PLAYER_UNHIT_UNGRAB_COLOR, "player_unhit_ungrab_color", FFFFFFFF);
CONFIG_HEX(HITBOX_VIS, MISC_HURT_COLOR, "misc_hurt_color", FFFFFF00);

#define NETWORK_SECTION_NAME "network"
CONFIG_TST(NETWORK, IPV6, "enable_ipv6");
CONFIG_BOL(NETWORK, HIDE_IP, "hide_ip", false);
CONFIG_BOL(NETWORK, SHARE_WATCH_IP, "share_watch_ip", false);
CONFIG_BOL(NETWORK, HIDE_NAME, "hide_name", false);
CONFIG_BOL(NETWORK, HIDE_PROFILE_PICTURES, "hide_profile_pictures", false);
// CONFIG_BOL(NETWORK, AUTO_SWITCH,"auto_seach_host",true);
CONFIG_STR(NETWORK, BLACKLIST,"blacklist","");
// Auto-connect: "host" / "client" / "" — boot.nut drives StartupServer/
// StartupClient on its own when set, so two windows can pair up without
// menu navigation. Used for rollback local two-client testing.
CONFIG_STR(NETWORK, AUTO_CONNECT, "auto_connect", "");
CONFIG_STR(NETWORK, PEER_IP, "peer_ip", "127.0.0.1");
CONFIG_INT(NETWORK, PEER_PORT, "peer_port", 10800);
// Which local input device to bind to this instance (0 = first
// keyboard/controller). Two instances on one machine should use
// different IDs so they don't both grab the same controller.
CONFIG_INT(NETWORK, DEVICE_ID, "device_id", 0);
// Gate Gekko rollback. Default off so the two-client auto-connect path
// runs pure squiroll delay-based netcode (proven, character-accurate).
// Flip on (config.ini [network] gekko_enabled=true OR env var
// SQUIROLL_GEKKO_ENABLED=1) to layer Gekko on top.
CONFIG_BOL(NETWORK, GEKKO_ENABLED, "gekko_enabled", false);
CONFIG_BOL(NETWORK, AUTO_ACCEPT, "auto_accept",false);

#define PERF_SECTION_NAME "performance"
CONFIG_BOL(PERF, CACHE_RSA, "cache_rsa", true);
CONFIG_FLT(PERF, TIMER_LENIENCY, "timer_leniency", 4.0);

#define MISC_SECTION_NAME "misc"
CONFIG_BOL(MISC, HIDE_WIP, "hide_wip", true);
CONFIG_BOL(MISC, SKIP_INTRO, "skip_intro", true);
CONFIG_BOL(MISC, SKIP_TO_BATTLE, "skip_to_battle", false);
CONFIG_TST(MISC, DISCORD, "discord_integration");

// DO NOT DEFINE NORMAL CONFIGS BELOW THIS LINE
static inline constexpr auto END_COUNTER = __COUNTER__ - 1;

// This config is "hidden", it won't get added to people's config files automatically
CONFIG_BOL(MISC, DEV_MODE, "dev", false);

#define CONFIG_DEFAULT(SECTION, KEY) { MACRO_CAT(SECTION,_SECTION_NAME), MACRO_CAT4(SECTION,_,KEY,_KEY), MACRO_CAT4(SECTION,_,KEY,_DEFAULT_STR) }

static inline constexpr const char
    *const DEFAULT_CONFIGS[END_COUNTER - START_COUNTER][3] = {
        CONFIG_DEFAULT(LOBBY, HOST),
        CONFIG_DEFAULT(LOBBY, PORT),
        CONFIG_DEFAULT(LOBBY, PASS),

        CONFIG_DEFAULT(HITBOX_VIS, ENABLED),
        CONFIG_DEFAULT(HITBOX_VIS, BORDER_WIDTH),
        CONFIG_DEFAULT(HITBOX_VIS, INNER_ALPHA),
        CONFIG_DEFAULT(HITBOX_VIS, BORDER_ALPHA),
        CONFIG_DEFAULT(HITBOX_VIS, COLLISION_COLOR),
        CONFIG_DEFAULT(HITBOX_VIS, HIT_COLOR),
        CONFIG_DEFAULT(HITBOX_VIS, PLAYER_HURT_COLOR),
        CONFIG_DEFAULT(HITBOX_VIS, PLAYER_UNHIT_COLOR),
        CONFIG_DEFAULT(HITBOX_VIS, PLAYER_UNGRAB_COLOR),
        CONFIG_DEFAULT(HITBOX_VIS, PLAYER_UNHIT_UNGRAB_COLOR),
        CONFIG_DEFAULT(HITBOX_VIS, MISC_HURT_COLOR),

        CONFIG_DEFAULT(NETWORK, IPV6),
        CONFIG_DEFAULT(NETWORK, HIDE_IP),
        CONFIG_DEFAULT(NETWORK, SHARE_WATCH_IP),
        CONFIG_DEFAULT(NETWORK, HIDE_NAME),
        CONFIG_DEFAULT(NETWORK, HIDE_PROFILE_PICTURES),
        // CONFIG_DEFAULT(NETWORK, AUTO_SWITCH),
        CONFIG_DEFAULT(NETWORK,BLACKLIST),
        CONFIG_DEFAULT(NETWORK, AUTO_CONNECT),
        CONFIG_DEFAULT(NETWORK, PEER_IP),
        CONFIG_DEFAULT(NETWORK, PEER_PORT),
        CONFIG_DEFAULT(NETWORK, DEVICE_ID),
        CONFIG_DEFAULT(NETWORK, GEKKO_ENABLED),
        CONFIG_DEFAULT(NETWORK, AUTO_ACCEPT),

        CONFIG_DEFAULT(PERF, CACHE_RSA),
        CONFIG_DEFAULT(PERF, TIMER_LENIENCY),

        CONFIG_DEFAULT(MISC, HIDE_WIP),
        CONFIG_DEFAULT(MISC, SKIP_INTRO),
        CONFIG_DEFAULT(MISC, SKIP_TO_BATTLE),
        CONFIG_DEFAULT(MISC, DISCORD)};

static inline constexpr size_t VALIDATE_DEFAULT_CONFIGS() {
    size_t expected_count = countof(DEFAULT_CONFIGS);
    size_t count = 0;
    for (size_t i = 0; i < countof(DEFAULT_CONFIGS); ++i) {
        count += DEFAULT_CONFIGS[i][0] && DEFAULT_CONFIGS[i][1] && DEFAULT_CONFIGS[i][2];
    }
    return expected_count - count;
}

static_assert(VALIDATE_DEFAULT_CONFIGS() == 0, "Missing default config entries!");

// The string pointers are intentionally used as the key because the config functions are always called with const strings
template<typename T, typename U>
struct ptr_pair_hash {
    size_t operator()(const std::pair<T, U>& x) const {
        return (size_t)x.first ^ std::rotl((size_t)x.second, 16);
    }
};
static std::unordered_map<std::pair<const char*, const char*>, std::vector<char>, ptr_pair_hash<const char*, const char*>> config_cache;

static inline bool create_dummy_file(const char* path) {
    HANDLE handle = CreateFileA(
        path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL
    );
    if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        return true;
    }
    return false;
}

void set_config_string(const char* section, const char* key, const char* value) {
    WritePrivateProfileStringA(section, key, value, CONFIG_FILE_PATH);
    config_cache.erase({section, key});
}

enum ConfigTruncType {
    TruncationIsFail,
    AllowTruncation
};

template <ConfigTruncType trunc = TruncationIsFail, size_t N = 0>
static inline size_t get_config_string(const char* section, const char* key, char(&value_buffer)[N]) {
    auto it = config_cache.find({section, key});

    if (it != config_cache.end()) {
        auto& entry = it->second;
        memcpy(value_buffer, entry.data(), entry.size());
        value_buffer[entry.size()] = '\0';
        return entry.size();
    } else {
        size_t written = GetPrivateProfileStringA(section, key, NULL, value_buffer, N, CONFIG_FILE_PATH);
        if constexpr (trunc != AllowTruncation) {
            if (expect(written == N - 1, false)) {
                written = 0;
            }
        }
        config_cache.emplace(std::make_pair(section, key), std::vector(value_buffer, &value_buffer[written]));
        return written;
    }
}

static inline void fill_default_config_string(const char* section, const char* key, const char* default_value) {
    static char DUMMY[2]{ '\0' };
    if (!GetPrivateProfileStringA(section, key, NULL, DUMMY, 2, CONFIG_FILE_PATH)) {
        WritePrivateProfileStringA(section, key, default_value, CONFIG_FILE_PATH);
    }
}

// ===================
// TEMPLATES
// ===================
template <bool needs_config_enabled = true, size_t N>
static bool get_bool_setting(const char* section, const char* key, char(&buffer)[N], bool default_val) {
    size_t length;
    bool ret = default_val;
    if (
        (!needs_config_enabled || use_config) &&
        (length = get_config_string(section, key, buffer))
    ) {
        if (!stricmp(buffer, "true")) {
            ret = true;
        }
        else if (!stricmp(buffer, "false")) {
            ret = false;
        }
    }
    return ret;
}

template <bool needs_config_enabled = true, size_t N>
static ConfigTestState get_test_setting(const char* section, const char* key, char(&buffer)[N]) {
    size_t length;
    if (
        (!needs_config_enabled || use_config) &&
        (length = get_config_string(section, key, buffer))
    ) {
        if (!stricmp(buffer, "true")) {
            return ConfigEnabled;
        }
        else if (!stricmp(buffer, "false")) {
            return ConfigDisabled;
        }
    }
    return ConfigNeedsTest;
}

template <bool needs_config_enabled = true, size_t N>
static int32_t get_int_setting(const char* section, const char* key, char(&buffer)[N], int32_t default_val) {
    size_t length;
    int32_t ret = default_val;
    if (
        (!needs_config_enabled || use_config) &&
        (length = get_config_string(section, key, buffer))
    ) {
        std::from_chars(buffer, &buffer[length], ret, 10);
    }
    return ret;
}

template <bool needs_config_enabled = true, size_t N>
static uint32_t get_hex_setting(const char* section, const char* key, char(&buffer)[N], uint32_t default_val) {
    size_t length;
    uint32_t ret = default_val;
    if (
        (!needs_config_enabled || use_config) &&
        (length = get_config_string(section, key, buffer))
    ) {
        std::from_chars(buffer, &buffer[length], ret, 16);
    }
    return ret;
}

template <bool needs_config_enabled = true, size_t N>
static float get_float_setting(const char* section, const char* key, char(&buffer)[N], float default_val) {
    size_t length;
    float ret = default_val;
    if (
        (!needs_config_enabled || use_config) &&
        (length = get_config_string<AllowTruncation>(section, key, buffer))
    ) {
        std::from_chars(buffer, &buffer[length], ret);
    }
    return ret;
}

#define GET_BOOL_CONFIG(SECTION, KEY) get_bool_setting(MACRO_CAT(SECTION,_SECTION_NAME), MACRO_CAT4(SECTION,_,KEY,_KEY), MACRO_CAT4(SECTION,_,KEY,_BUFFER), MACRO_CAT4(SECTION,_,KEY,_DEFAULT))
#define GET_TEST_CONFIG(SECTION, KEY) get_test_setting(MACRO_CAT(SECTION,_SECTION_NAME), MACRO_CAT4(SECTION,_,KEY,_KEY), MACRO_CAT4(SECTION,_,KEY,_BUFFER))
#define GET_INT_CONFIG(SECTION, KEY) get_int_setting(MACRO_CAT(SECTION,_SECTION_NAME), MACRO_CAT4(SECTION,_,KEY,_KEY), MACRO_CAT4(SECTION,_,KEY,_BUFFER), MACRO_CAT4(SECTION,_,KEY,_DEFAULT))
#define GET_HEX_CONFIG(SECTION, KEY) get_hex_setting(MACRO_CAT(SECTION,_SECTION_NAME), MACRO_CAT4(SECTION,_,KEY,_KEY), MACRO_CAT4(SECTION,_,KEY,_BUFFER), MACRO_CAT4(SECTION,_,KEY,_DEFAULT))
#define GET_FLOAT_CONFIG(SECTION, KEY) get_float_setting(MACRO_CAT(SECTION,_SECTION_NAME), MACRO_CAT4(SECTION,_,KEY,_KEY), MACRO_CAT4(SECTION,_,KEY,_BUFFER), MACRO_CAT4(SECTION,_,KEY,_DEFAULT))

// ====================
// LOBBY
// ====================

static char LOBBY_HOST_BUFFER[1024]{ '\0' };
const char* get_lobby_host(const char* host) {
    if (
        use_config &&
        get_config_string(LOBBY_SECTION_NAME, LOBBY_HOST_KEY, LOBBY_HOST_BUFFER)
    ) {
        host = LOBBY_HOST_BUFFER;
    }
    return host;
}

static char LOBBY_PORT_BUFFER[256]{ '\0' };
const char* get_lobby_port(const char* port) {
    if (
        use_config &&
        get_config_string(LOBBY_SECTION_NAME, LOBBY_PORT_KEY, LOBBY_PORT_BUFFER)
    ) {
        port = LOBBY_PORT_BUFFER;
    }
    return port;
}

static char LOBBY_PASS_BUFFER[1024]{ '\0' };
const char* get_lobby_pass(const char* pass) {
    if (
        use_config &&
        get_config_string(LOBBY_SECTION_NAME, LOBBY_PASS_KEY, LOBBY_PASS_BUFFER)
    ) {
        pass = LOBBY_PASS_BUFFER;
    }
    return pass;
}

// ====================
// HITBOX VISUALIZER
// ====================

static char HITBOX_VIS_ENABLED_BUFFER[8]{ '\0' };
bool get_hitbox_vis_enabled() {
    return GET_BOOL_CONFIG(HITBOX_VIS, ENABLED);
}

static char HITBOX_VIS_BORDER_WIDTH_BUFFER[INTEGER_BUFFER_SIZE<int32_t>]{ '\0' };
int32_t get_hitbox_border_width() {
    return GET_INT_CONFIG(HITBOX_VIS, BORDER_WIDTH);
}

static char HITBOX_VIS_INNER_ALPHA_BUFFER[FLOAT_BUFFER_SIZE<float>]{ '\0' };
float get_hitbox_inner_alpha() {
    return GET_FLOAT_CONFIG(HITBOX_VIS, INNER_ALPHA);
}

static char HITBOX_VIS_BORDER_ALPHA_BUFFER[FLOAT_BUFFER_SIZE<float>]{ '\0' };
float get_hitbox_border_alpha() {
    return GET_FLOAT_CONFIG(HITBOX_VIS, BORDER_ALPHA);
}

static char HITBOX_VIS_COLLISION_COLOR_BUFFER[INTEGER_BUFFER_SIZE<uint32_t>]{ '\0' };
uint32_t get_hitbox_collision_color() {
    return GET_HEX_CONFIG(HITBOX_VIS, COLLISION_COLOR);
}

static char HITBOX_VIS_HIT_COLOR_BUFFER[INTEGER_BUFFER_SIZE<uint32_t>]{ '\0' };
uint32_t get_hitbox_hit_color() {
    return GET_HEX_CONFIG(HITBOX_VIS, HIT_COLOR);
}

static char HITBOX_VIS_PLAYER_HURT_COLOR_BUFFER[INTEGER_BUFFER_SIZE<uint32_t>]{ '\0' };
uint32_t get_hitbox_player_hurt_color() {
    return GET_HEX_CONFIG(HITBOX_VIS, PLAYER_HURT_COLOR);
}

static char HITBOX_VIS_PLAYER_UNHIT_COLOR_BUFFER[INTEGER_BUFFER_SIZE<uint32_t>]{ '\0' };
uint32_t get_hitbox_player_unhit_color() {
    return GET_HEX_CONFIG(HITBOX_VIS, PLAYER_UNHIT_COLOR);
}

static char HITBOX_VIS_PLAYER_UNGRAB_COLOR_BUFFER[INTEGER_BUFFER_SIZE<uint32_t>]{ '\0' };
uint32_t get_hitbox_player_ungrab_color() {
    return GET_HEX_CONFIG(HITBOX_VIS, PLAYER_UNGRAB_COLOR);
}

static char HITBOX_VIS_PLAYER_UNHIT_UNGRAB_COLOR_BUFFER[INTEGER_BUFFER_SIZE<uint32_t>]{ '\0' };
uint32_t get_hitbox_player_unhit_ungrab_color() {
    return GET_HEX_CONFIG(HITBOX_VIS, PLAYER_UNHIT_UNGRAB_COLOR);
}

static char HITBOX_VIS_MISC_HURT_COLOR_BUFFER[INTEGER_BUFFER_SIZE<uint32_t>]{ '\0' };
uint32_t get_hitbox_misc_hurt_color() {
    return GET_HEX_CONFIG(HITBOX_VIS, MISC_HURT_COLOR);
}

// ====================
// NETWORK
// ====================

static char NETWORK_IPV6_BUFFER[8]{ '\0' };
int8_t get_ipv6_state() {
    return GET_TEST_CONFIG(NETWORK, IPV6);
}

static char NETWORK_HIDE_IP_BUFFER[8]{ '\0' };
bool get_hide_ip_enabled() {
    return GET_BOOL_CONFIG(NETWORK, HIDE_IP);
}

static char NETWORK_SHARE_WATCH_IP_BUFFER[8]{ '\0' };
bool get_share_watch_ip_enabled() {
    return GET_BOOL_CONFIG(NETWORK, SHARE_WATCH_IP);
}

static char NETWORK_HIDE_NAME_BUFFER[8]{ '\0' };
bool get_hide_name_enabled() {
    return GET_BOOL_CONFIG(NETWORK, HIDE_NAME);
}

static char NETWORK_HIDE_PROFILE_PICTURES_BUFFER[8]{ '\0' };
bool get_hide_profile_pictures_enabled() {
    return GET_BOOL_CONFIG(NETWORK, HIDE_PROFILE_PICTURES);
}

static char NETWORK_AUTO_ACCEPT_BUFFER[8]{ '\0' };
bool get_auto_accept_enabled() {
    return GET_BOOL_CONFIG(NETWORK, AUTO_ACCEPT);
}

void set_ipv6_state(bool state) {
    set_config_string(NETWORK_SECTION_NAME, NETWORK_IPV6_KEY, bool_str(state));
}

static char NETWORK_BLACKLIST_BUFFER[1024]{ '\0' };
const char* get_network_blacklist() {
    // Init to empty so callers (e.g. plugin.cpp's sq_setstring) never
    // see a stack-garbage pointer when config lookup misses. Latent UB
    // before; /Od exposed it via a NULL pointer crashing
    // ::setting.network.blacklist consumers in Squirrel.
    const char* blacklist = NETWORK_BLACKLIST_DEFAULT;
    if (
        use_config &&
        get_config_string(NETWORK_SECTION_NAME, NETWORK_BLACKLIST_KEY, NETWORK_BLACKLIST_BUFFER)
    ) {
        blacklist = NETWORK_BLACKLIST_BUFFER;
    }
    return blacklist;
}

// Env-var overrides let one config.ini drive two instances in the same
// folder: host.bat and client.bat each set SQUIROLL_AUTO_CONNECT plus
// SQUIROLL_PEER_PORT (and SQUIROLL_PEER_IP for the client). Values are
// cached per-process on first read.
static const char* read_env_static(const char* name, char* buf, size_t buf_len) {
    DWORD n = GetEnvironmentVariableA(name, buf, (DWORD)buf_len);
    if (n == 0 || n >= buf_len) return nullptr;
    return buf;
}

static char NETWORK_AUTO_CONNECT_BUFFER[32]{ '\0' };
const char* get_auto_connect() {
    static char env_buf[32];
    static int env_state = 0;  // 0=unread, 1=found, 2=missing
    if (env_state == 0) {
        env_state = read_env_static("SQUIROLL_AUTO_CONNECT", env_buf, sizeof(env_buf))
            ? 1 : 2;
    }
    if (env_state == 1) return env_buf;

    if (
        use_config &&
        get_config_string(NETWORK_SECTION_NAME, NETWORK_AUTO_CONNECT_KEY, NETWORK_AUTO_CONNECT_BUFFER)
    ) {
        return NETWORK_AUTO_CONNECT_BUFFER;
    }
    return NETWORK_AUTO_CONNECT_DEFAULT;
}

static char NETWORK_PEER_IP_BUFFER[256]{ '\0' };
const char* get_peer_ip() {
    static char env_buf[256];
    static int env_state = 0;
    if (env_state == 0) {
        env_state = read_env_static("SQUIROLL_PEER_IP", env_buf, sizeof(env_buf))
            ? 1 : 2;
    }
    if (env_state == 1) return env_buf;

    if (
        use_config &&
        get_config_string(NETWORK_SECTION_NAME, NETWORK_PEER_IP_KEY, NETWORK_PEER_IP_BUFFER)
    ) {
        return NETWORK_PEER_IP_BUFFER;
    }
    return NETWORK_PEER_IP_DEFAULT;
}

static char NETWORK_PEER_PORT_BUFFER[16]{ '\0' };
int32_t get_peer_port() {
    static char env_buf[16];
    static int env_cached = 0;
    static int32_t env_val = 0;
    if (env_cached == 0) {
        if (read_env_static("SQUIROLL_PEER_PORT", env_buf, sizeof(env_buf))) {
            env_val = atoi(env_buf);
            env_cached = 1;
        } else {
            env_cached = 2;
        }
    }
    if (env_cached == 1) return env_val;
    return GET_INT_CONFIG(NETWORK, PEER_PORT);
}

static char NETWORK_GEKKO_ENABLED_BUFFER[8]{ '\0' };
bool get_gekko_enabled() {
    static char env_buf[8];
    static int env_cached = 0;
    static bool env_val = false;
    if (env_cached == 0) {
        if (read_env_static("SQUIROLL_GEKKO_ENABLED", env_buf, sizeof(env_buf))) {
            env_val = (env_buf[0] == '1' || env_buf[0] == 't' || env_buf[0] == 'T'
                       || env_buf[0] == 'y' || env_buf[0] == 'Y');
            env_cached = 1;
        } else {
            env_cached = 2;
        }
    }
    if (env_cached == 1) return env_val;
    return GET_BOOL_CONFIG(NETWORK, GEKKO_ENABLED);
}

static char NETWORK_DEVICE_ID_BUFFER[16]{ '\0' };
int32_t get_device_id() {
    static char env_buf[16];
    static int env_cached = 0;
    static int32_t env_val = 0;
    if (env_cached == 0) {
        if (read_env_static("SQUIROLL_DEVICE_ID", env_buf, sizeof(env_buf))) {
            env_val = atoi(env_buf);
            env_cached = 1;
        } else {
            env_cached = 2;
        }
    }
    if (env_cached == 1) return env_val;
    return GET_INT_CONFIG(NETWORK, DEVICE_ID);
}

// ====================
// MISC
// ====================

static char MISC_HIDE_WIP_BUFFER[8]{ '\0' };
bool get_hide_wip_enabled() {
    return GET_BOOL_CONFIG(MISC, HIDE_WIP);
}

// Parse a SQUIROLL_* boolean env var the same way get_gekko_enabled does:
// leading 1/t/T/y/Y = true, 0/f/n = false. Returns -1 when the var is
// unset (caller falls back to netcode.ini) so a launch can drive this
// setting per-instance without editing the shared ini. env_cached/env_val
// are the caller's per-getter static latches.
static int read_env_bool(const char* name, char* env_buf, size_t buf_len,
                         int& env_cached, bool& env_val) {
    if (env_cached == 0) {
        if (read_env_static(name, env_buf, buf_len)) {
            char c = env_buf[0];
            env_val = (c == '1' || c == 't' || c == 'T' || c == 'y' || c == 'Y');
            env_cached = 1;
        } else {
            env_cached = 2;
        }
    }
    return env_cached == 1 ? (env_val ? 1 : 0) : -1;
}

static char MISC_SKIP_INTRO_BUFFER[8]{ '\0' };
bool get_skip_intro_enabled() {
    static char env_buf[8];
    static int env_cached = 0;
    static bool env_val = false;
    int e = read_env_bool("SQUIROLL_SKIP_INTRO", env_buf, sizeof(env_buf),
                          env_cached, env_val);
    if (e >= 0) return e != 0;
    return GET_BOOL_CONFIG(MISC, SKIP_INTRO);
}

static char MISC_SKIP_TO_BATTLE_BUFFER[8]{ '\0' };
bool get_skip_to_battle_enabled() {
    static char env_buf[8];
    static int env_cached = 0;
    static bool env_val = false;
    int e = read_env_bool("SQUIROLL_SKIP_TO_BATTLE", env_buf, sizeof(env_buf),
                          env_cached, env_val);
    if (e >= 0) return e != 0;
    return GET_BOOL_CONFIG(MISC, SKIP_TO_BATTLE);
}

static char MISC_DISCORD_BUFFER[8]{ '\0' };
int8_t get_discord_enabled() {
    return GET_TEST_CONFIG(MISC, DISCORD);
}

static char MISC_DEV_MODE_BUFFER[8]{ '\0' };
bool get_dev_mode_enabled() {
    return GET_BOOL_CONFIG(MISC, DEV_MODE);
}

void set_discord_enabled(bool state) {
    set_config_string(MISC_SECTION_NAME, MISC_DISCORD_KEY, bool_str(state));
}

// ====================
// Performance
// ====================
static char PERF_CACHE_RSA_BUFFER[8]{ '\0' };
bool get_cache_rsa_enabled() {
    return GET_BOOL_CONFIG(PERF, CACHE_RSA);
}

static char PERF_TIMER_LENIENCY_BUFFER[FLOAT_BUFFER_SIZE<float>]{ '\0' };
float get_timer_leniency() {
    return GET_FLOAT_CONFIG(PERF, TIMER_LENIENCY);
}


static HANDLE config_watcher_thread = NULL;
static std::atomic_bool config_watcher_exit = false;
static std::atomic_bool config_flush_queued = false;
static DWORD stdcall config_watcher_proc(void*) {
    HANDLE file = CreateFileA(
        CONFIG_FILE_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL
    );
    if (file == INVALID_HANDLE_VALUE)
        return 0;

    // Using the proper file change watcher API seems overkill for this
    FILETIME last_last_write = {};
    FILETIME last_write = {};
    while (!config_watcher_exit) {
        if (
            GetFileTime(file, NULL, NULL, &last_write) &&
            memcmp(&last_last_write, &last_write, sizeof(FILETIME)) &&
            last_last_write.dwLowDateTime != 0 && last_last_write.dwHighDateTime != 0)
        {
            config_flush_queued = true;
        }
        last_last_write = last_write;
        Sleep(64);
    }

    CloseHandle(file);
    return 0;
}

void config_watcher_check() {
    if (config_flush_queued) {
        config_cache.clear();
        config_flush_queued = false;
    }
}

void config_watcher_stop() {
    config_watcher_exit = true;
    if (config_watcher_thread)
        WaitForSingleObject(config_watcher_thread, 1000);
}


static char TASOFRO_GAME_VERSION_BUFFER[INTEGER_BUFFER_SIZE<int32_t>]{ '\0' };
int32_t GAME_VERSION = 1211;

void init_config_file() {

    for (
        size_t filename_length = GetModuleFileNameA(NULL, CONFIG_FILE_PATH, countof(CONFIG_FILE_PATH));
        filename_length;
        --filename_length
    ) {
        switch (CONFIG_FILE_PATH[filename_length]) {
            case '\\': case '/':
                if (filename_length < countof(CONFIG_FILE_PATH) - countof(TASOFRO_CONFIG_FILE_NAME)) {
                    memcpy(&CONFIG_FILE_PATH[filename_length + 1], TASOFRO_CONFIG_FILE_NAME, sizeof(TASOFRO_CONFIG_FILE_NAME));

                    if (file_exists(CONFIG_FILE_PATH)) {
                        GAME_VERSION = get_int_setting<false>("updater", "version", TASOFRO_GAME_VERSION_BUFFER, GAME_VERSION);
                    }
                }
                if (filename_length < countof(CONFIG_FILE_PATH) - countof(CONFIG_FILE_NAME)) {

                    memcpy(&CONFIG_FILE_PATH[filename_length + 1], CONFIG_FILE_NAME, sizeof(CONFIG_FILE_NAME));

                    if (
                        file_exists(CONFIG_FILE_PATH) || create_dummy_file(CONFIG_FILE_PATH)
                    ) {
                        use_config = true;

                        nounroll for (size_t i = 0; i < countof(DEFAULT_CONFIGS); ++i) {
                            fill_default_config_string(DEFAULT_CONFIGS[i][0], DEFAULT_CONFIGS[i][1], DEFAULT_CONFIGS[i][2]);
                        }

                        config_watcher_thread = CreateThread(NULL, 0, config_watcher_proc, NULL, 0, NULL);
                    }
                }
                return;
        }
    }
}

#if 0   // disabled until GekkoNet is wired into the build

// GekkoNet bridge — skeleton. See gekko_bridge.h for the public API and
// design notes. Each TODO below is a concrete next step.

#include <stdint.h>
#include <string.h>
#include <windows.h>

#include "gekko_bridge.h"
#include "patch_utils.h"
#include "netcode.h"
#include "util.h"
#include "log.h"
#include "Actor2D.h"
#include "live_actors.h"

#define GEKKONET_STATIC
#include <gekkonet.h>

// --- Engine entry points (RVAs in canonical scheme, used with _R macro) ---

// Per-frame logic update: runs update_related(dword_AFB01C) +
// Act::ScriptAPI_ptr->vftable->Update(...). update_logic also polls the
// PrintScreen key — fine to skip during rollback resim.
typedef void cdecl update_logic_t();
#define update_logic ((update_logic_t*)0xE1A0_R)

typedef void thiscall update_related_t(void* this_);
#define update_related ((update_related_t*)0x2FAD0_R)

typedef char cdecl drawing_related_t();
#define drawing_related ((drawing_related_t*)0xE330_R)

// dword_AFB01C = the input_update_list head used by run_update_list.
#define INPUT_UPDATE_LIST_PTR ((void**)0xAFB01C_R)

// Act::ScriptAPI dispatcher pointer — drives the Squirrel root frame.
struct ScriptAPI_vtbl {
    void* field_0;
    void* field_4;   // render preprocess
    void* field_8;   // render
    void* field_C;
    void (thiscall* Update)(void* self);  // ::loop pump that drives the Squirrel side
};
struct ScriptAPI { ScriptAPI_vtbl* vftable; };
#define Act_ScriptAPI_ptr (*(ScriptAPI**)0xB3ACFC_R)

namespace gekko_bridge {

// ------------------------------------------------------------------ state --

static GekkoSession* g_session = nullptr;
static uint8_t       g_local_idx = 0;
static bool          g_active = false;

uint16_t forced_inputs[2] = {0, 0};
bool     forced_inputs_active = false;

// ----------------------------------------------------------------- helpers --

static uint16_t read_local_input_bits() {
    // TODO: pull from Manbow::InputRecorder. The existing SyncInput_hook in
    // netcode.cpp already reads the local-player input via
    //   ((uint16_t(thiscall*)(TF4InputDeviceState*))(0x169D80_R))(...)
    // Expose that as a helper and call it here.
    return 0;
}

static uint32_t fletcher32(const uint8_t* data, size_t len) {
    // TODO: use whatever checksum GekkoNet's desync detector prefers.
    uint32_t a = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) a = (a >> 8) ^ (a + data[i]);
    return a;
}

// ---------------------------------------------------------------- save/load --

// Header magic + version: bump version whenever the layout changes.
static constexpr uint32_t SAVE_MAGIC   = 0x46414B47; // 'GKAF'
static constexpr uint32_t SAVE_VERSION = 1;

struct SaveHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t frame;
    uint32_t rand_state;
    uint32_t actor_count;
    // followed by actor_count × sizeof(ManbowActor2D) raw bytes.
    // TODO v2: per-actor sq_addref'd flag1..5 SQObjects (8 bytes each).
    // TODO v3: Squirrel diff buffer length + bytes.
    // TODO v4: b2ParticleSystem state.
};

uint32_t save_state_to_buf(void* buf, uint32_t cap, uint32_t* out_checksum) {
    if (cap < sizeof(SaveHeader)) return 0;

    uint8_t* p = static_cast<uint8_t*>(buf);
    SaveHeader* hdr = reinterpret_cast<SaveHeader*>(p);
    hdr->magic      = SAVE_MAGIC;
    hdr->version    = SAVE_VERSION;
    hdr->frame      = 0; // TODO: pull current frame counter
    hdr->rand_state = 0; // TODO: read acrt_ptd->rand_state (see rollback.cpp::acrt_getptd)

    // Snapshot live actor pointers under a fixed cap.
    static constexpr size_t MAX_ACTORS = 1024;
    ManbowActor2D* actors[MAX_ACTORS];
    size_t n = live_actors::snapshot(actors, MAX_ACTORS);
    hdr->actor_count = (uint32_t)n;

    p += sizeof(SaveHeader);
    uint32_t remaining = cap - sizeof(SaveHeader);
    uint32_t need = (uint32_t)(n * sizeof(ManbowActor2D));
    if (remaining < need) {
        log_printf("gekko_bridge::save_state: buffer too small (%u/%u)\n",
                   remaining, need);
        return 0;
    }

    // Raw memcpy per actor. Caveats:
    //   - shared_ptr<AnimationController2D> @ 0x3C copied without addref
    //     (TODO: bump refcount manually so load_state can find it valid)
    //   - SqratFunction @ 0xA8 + 0xBC holds HSQOBJECT we should sq_addref
    //   - flag1..5 SQObjects on the SQInstance side are NOT in this struct
    //     and need separate snapshotting (see Actor2D.h)
    for (size_t i = 0; i < n; ++i) {
        memcpy(p, actors[i], sizeof(ManbowActor2D));
        p += sizeof(ManbowActor2D);
    }

    uint32_t written = (uint32_t)(p - static_cast<uint8_t*>(buf));
    if (out_checksum) {
        // Cheap rolling hash of the actor bytes only (header excluded so the
        // frame counter doesn't churn the checksum each frame).
        const uint8_t* abeg = static_cast<uint8_t*>(buf) + sizeof(SaveHeader);
        *out_checksum = fletcher32(abeg, written - sizeof(SaveHeader));
    }
    return written;
}

void load_state_from_buf(const void* buf, uint32_t len) {
    if (len < sizeof(SaveHeader)) {
        log_printf("gekko_bridge::load_state: undersized blob (%u)\n", len);
        return;
    }
    const SaveHeader* hdr = static_cast<const SaveHeader*>(buf);
    if (hdr->magic != SAVE_MAGIC || hdr->version != SAVE_VERSION) {
        log_printf("gekko_bridge::load_state: bad header magic=%08x ver=%u\n",
                   hdr->magic, hdr->version);
        return;
    }

    // TODO: restore acrt_ptd->rand_state = hdr->rand_state
    // TODO: restore frame counter if/where it lives

    const uint8_t* p = static_cast<const uint8_t*>(buf) + sizeof(SaveHeader);
    uint32_t need = hdr->actor_count * (uint32_t)sizeof(ManbowActor2D);
    if (len - sizeof(SaveHeader) < need) {
        log_printf("gekko_bridge::load_state: short actor blob\n");
        return;
    }

    // Naive restore: walk current live set in the same order as save_state
    // emitted (snapshot returns whatever order the std::unordered_set
    // happened to be in — that's NOT stable across reallocations).
    //
    // TODO: emit a stable key (Actor2D::id at offset 0x18) alongside each
    // saved actor and match on id rather than position. Without that, this
    // will desync the moment an actor was created/destroyed between
    // save and load.
    static constexpr size_t MAX_ACTORS = 1024;
    ManbowActor2D* actors[MAX_ACTORS];
    size_t n = live_actors::snapshot(actors, MAX_ACTORS);
    if (n != hdr->actor_count) {
        log_printf("gekko_bridge::load_state: actor count mismatch (live=%zu saved=%u) — implement id-keyed match\n",
                   n, hdr->actor_count);
        // Bail rather than corrupt half the pool with mismatched memcpy.
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        memcpy(actors[i], p, sizeof(ManbowActor2D));
        p += sizeof(ManbowActor2D);
    }
}

// ----------------------------------------------------------- frame drivers --

void advance_one_frame() {
    // Drive one logic tick WITHOUT rendering. Inputs are forced via
    // SyncInput_hook reading forced_inputs[] when forced_inputs_active.
    update_related(*INPUT_UPDATE_LIST_PTR);
    Act_ScriptAPI_ptr->vftable->Update(Act_ScriptAPI_ptr);
}

void render_one_frame() {
    drawing_related();
}

// ---------------------------------------------------------------- session --

bool init(uint16_t local_port, uint16_t remote_port,
          uint8_t local_player_idx, const char* remote_ip)
{
    if (g_session) return false;

    gekko_create(&g_session, GekkoGameSession);

    GekkoConfig config = {};
    config.desync_detection = true;
    config.input_size = sizeof(uint16_t);
    config.state_size = 200 * 1024;  // TODO: measure exact size; conservative for now
    config.max_spectators = 0;
    config.input_prediction_window = 10;
    config.num_players = 2;

    gekko_start(g_session, &config);
    gekko_net_adapter_set(g_session, gekko_default_adapter(local_port));
    gekko_set_runahead(g_session, 8);

    g_local_idx = local_player_idx;
    char remote_addr[64];
    snprintf(remote_addr, sizeof(remote_addr), "%s:%u", remote_ip, (unsigned)remote_port);

    for (int i = 0; i < 2; ++i) {
        if (i == local_player_idx) {
            gekko_add_actor(g_session, GekkoLocalPlayer, nullptr);
            gekko_set_local_delay(g_session, i, 2);
        } else {
            GekkoNetAddress addr = {};
            addr.data = remote_addr;
            addr.size = (uint32_t)strlen(remote_addr);
            gekko_add_actor(g_session, GekkoRemotePlayer, &addr);
        }
    }

    g_active = true;
    log_printf("gekko_bridge: session up. local=%u port=%u remote=%s\n",
               local_player_idx, local_port, remote_addr);
    return true;
}

void shutdown() {
    if (g_session) {
        gekko_default_adapter_destroy();
        gekko_destroy(&g_session);
        g_session = nullptr;
    }
    g_active = false;
}

bool is_active() { return g_active; }

// ------------------------------------------------------------------- tick --

bool tick() {
    if (!g_active) return false;

    gekko_network_poll(g_session);

    uint16_t my_input = read_local_input_bits();
    gekko_add_local_input(g_session, g_local_idx, &my_input);

    // Drain connection/desync events.
    int count = 0;
    GekkoSessionEvent** sevents = gekko_session_events(g_session, &count);
    for (int i = 0; i < count; ++i) {
        // TODO: log Connected/Disconnected/Syncing/DesyncDetected via log_printf
        (void)sevents[i];
    }

    // Drain save/load/advance events.
    count = 0;
    GekkoGameEvent** uevents = gekko_update_session(g_session, &count);
    bool advanced = false;
    for (int i = 0; i < count; ++i) {
        GekkoGameEvent* e = uevents[i];
        switch (e->type) {
            case GekkoSaveEvent: {
                uint32_t cs = 0;
                uint32_t n = save_state_to_buf(
                    e->data.save.state,
                    /*cap=*/ 200 * 1024,  // matches GekkoConfig::state_size
                    &cs
                );
                *e->data.save.state_len = n;
                *e->data.save.checksum = cs;
                break;
            }
            case GekkoLoadEvent:
                load_state_from_buf(e->data.load.state,
                                    *e->data.load.state_len);
                break;
            case GekkoAdvanceEvent: {
                const uint16_t* inputs = (const uint16_t*)e->data.adv.inputs;
                forced_inputs[0] = inputs[0];
                forced_inputs[1] = inputs[1];
                forced_inputs_active = true;
                advance_one_frame();
                forced_inputs_active = false;
                advanced = true;
                break;
            }
            default: break;
        }
    }

    if (advanced) render_one_frame();
    return advanced;
}

} // namespace gekko_bridge

#endif // 0

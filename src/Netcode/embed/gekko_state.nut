// Squirrel state serializer for GekkoNet rollback (identity-tagged graph).
//
// Why identity tagging: AoCF actors hold references to other actors
// (this.target, this.team.current, ...). A naive deep walk turns each
// reference into a value-copy → on load, restoring "target" with a bare
// table replaces a live actor reference with a struct that lacks the
// engine's expected methods/instance type, crashing on `this.target.x`.
//
// Format:
//   n;             — null
//   iN;            — integer
//   fN;            — float
//   b0; / b1;      — bool
//   sLEN:DATA;     — string
//   aN:[...]       — array of N elements
//   tN:{kv...}     — table with N pairs
//   ID:N:{kv...}   — instance, first sight: assigns saved-id D, then walks N class members
//   RD;            — reference to instance saved-id D (placeholder, resolved on apply)
//   ?;             — skipped value
//
// Save assigns ids on first-sight walk order; load applies in the same
// order so saved-id ↔ live-instance maps cleanly.

::__gekko_state <- {};

// Process-local fields that MUST NOT be rolled back. The walker emits
// these as `?;` (skip), and load_into ignores them. These are per-peer
// configuration (which slot is "ours", which controller drives it) —
// rolling back across peers would either flip them (causing input
// routing to break) or DESYNC gekko's checksum forever.
::__gekko_state._skip_keys <- {
    device_id   = true,  // engine input-device slot index (0 local, -1 remote)
    input       = true,  // TF4InputDevice ref — bound to local input recorder
    last_snap   = true,  // rollback.nut plugin cache — injected into
                         // PlayerTeamData + actor classes; process-local,
                         // not part of the gekko snapshot. Skipping it
                         // keeps the depth-6 walk out of plugin internals.
    // [M4] `count` on INSTANCES (draw/effect Actor2D-derived objects) is a
    // per-object FORWARD-ONLY animation counter, not sim state — load_into
    // restores it fine, but it advances non-deterministically across a headless
    // re-sim (engine-frame drift), so it's cosmetic render state that must be
    // excluded from the structural checksum. This ONLY affects the INSTANCE walk
    // (line ~182) — the sim-critical root ::battle.count rides the TABLE walk
    // (line ~111), which does NOT honor _skip_keys, so it's still checksummed.
    count       = true,
    // `__setTable` / `_staticTable` are the actor class's _set-dispatch + static
    // property tables (mostly setter functions). Not sim state, and they carry a
    // DUPLICATE cosmetic `count` the instance skip above can't reach (they're
    // TABLES, walked by the table branch which ignores _skip_keys). Skip whole.
    __setTable   = true,
    _staticTable = true,
};

// Singleton sentinel that the deserializer returns for `?;` (skip)
// markers in the saved blob. load_into checks for this exact identity
// and leaves the live slot alone — without this, methods serialized as
// `?;` would deserialize to null and load_into would stomp them with
// null, making `this.method()` calls fail forever after.
::__gekko_state._skip_marker <- { __skip = true };

// Caps recursion only on the EXPENSIVE branch (instance walk). Tables
// and arrays don't gate — they cost what their source describes.
::__gekko_state._max_depth <- 6;
::__gekko_state._cur_depth <- 0;

// Per-save state: list of [instance, assigned_id], cleared at save start.
::__gekko_state._seen     <- null;
::__gekko_state._next_id  <- 1;
::__gekko_state._cnt_log  <- 30;   // [M4-cntdiag] quota for the actor-count restore log

// Per-load state: saved-id -> live-instance (filled during apply).
::__gekko_state._live_by_id <- null;

// Live-value side table. battleUpdate (a closure — possibly an inline
// anonymous one), infoActor (array of actor weakrefs) and ::battle.task
// (table of task instances) cannot survive the text blob: a closure has
// no serializable form and a weakref/instance ref would round-trip to a
// bare table. But the objects they point at outlive the rollback window
// (actors are pinned by live_actors defer-release; a parked closure is
// kept alive by the strong ref below). So save_battle parks them here in
// a ring keyed by the Gekko frame, and load_battle reads the same slot.
// Process-local, like _skip_keys — nothing here enters the checksummed
// blob, so it cannot cause a desync.
::__gekko_state._keep_ring <- 256;
::__gekko_state._keep      <- array(256, null);

::__gekko_state._pretty_type <- function (t) {
    if (typeof t != "string") return "" + t;
    local i = t.find("@V");
    if (i != null) {
        local j = t.find("@", i + 2);
        if (j != null && j > i + 2) return t.slice(i + 2, j);
    }
    if (t.len() > 4 && t.slice(0, 4) == ".?AV") {
        local j = t.find("@", 4);
        if (j != null) return t.slice(4, j);
        return t.slice(4);
    }
    return t;
};

// _seen maps instance -> assigned id. A Squirrel table keys instances
// by reference identity, so this is an O(1) lookup — the old linear
// scan over an array of [instance,id] pairs was O(instances^2) across a
// full walk.
::__gekko_state._find_seen_id <- function (v) {
    return (v in ::__gekko_state._seen) ? ::__gekko_state._seen[v] : null;
};

::__gekko_state.ser <- function (v) {
    local t = typeof v;
    if (t == "null")    return "n;";
    if (t == "integer") return "i" + v + ";";
    if (t == "float")   return "f" + v + ";";
    if (t == "bool")    return "b" + (v ? "1" : "0") + ";";
    if (t == "string")  return "s" + v.len() + ":" + v + ";";
    if (t == "array") {
        // Arrays don't bump _cur_depth: only instance descents do.
        // Otherwise the table/array nesting around teams already trips
        // the cap before reaching any Sqrat-bound actor.
        local s = "a" + v.len() + ":[";
        foreach (e in v) s += ::__gekko_state.ser(e);
        return s + "]";
    }
    if (t == "table") {
        // Sort keys before emission: foreach on a table iterates the
        // underlying SQTable's hash buckets, and the bucket layout can
        // differ across two MSVC PEs even with identical insertion
        // order if the hash seed isn't strictly fixed. Sorted emission
        // guarantees identical bytes for identical content cross-peer.
        local keys = [];
        foreach (k, _ in v) keys.append(k);
        keys.sort();
        local s = "t" + v.len() + ":{";
        foreach (k in keys) {
            // [M4] In NESTED tables (depth > 0 — inside an actor's property /
            // snapshot / _set-dispatch tables) honor _skip_keys: the same
            // cosmetic/forward-only fields (count, device_id, ...) recur there and
            // load_into skips them on read anyway. Depth 0 is the ROOT ::battle
            // table, whose count/state/etc. ARE sim state and are never skipped.
            // [M4] Honor _skip_keys in tables too — the cosmetic/forward-only
            // fields (count, ...) recur inside actor property/snapshot/clone
            // tables (reached via all-table paths, so instance-depth tracking
            // doesn't see them). The root ::battle.count is deterministic (never
            // diverges) and the engine restores it via engine_snap .data, so
            // dropping its structural copy is safe. load_into skips these on read.
            if (k in ::__gekko_state._skip_keys) {
                s += ::__gekko_state.ser(k) + "?;";
                continue;
            }
            s += ::__gekko_state.ser(k) + ::__gekko_state.ser(v[k]);
        }
        return s + "}";
    }
    if (t == "weakref") {
        local target;
        try { target = v.ref(); } catch (_) { target = null; }
        return ::__gekko_state.ser(target);
    }
    if (t == "function" || t == "closure" || t == "nativeclosure" ||
        t == "userdata" || t == "thread" || t == "class")
    {
        return "?;";
    }
    // Anything else: treat as instance (Squirrel class instance —
    // includes AoCF actors, which are DerivedClass(Actor2D): a Squirrel
    // subclass over a Sqrat-bound C++ base).
    local existing = ::__gekko_state._find_seen_id(v);
    if (existing != null) return "R" + existing + ";";

    if (::__gekko_state._cur_depth >= ::__gekko_state._max_depth) {
        local my_id = ::__gekko_state._next_id++;
        ::__gekko_state._seen[v] <- my_id;
        return "I" + my_id + ":0:{}";
    }

    local cls;
    try { cls = v.getclass(); } catch (_) { cls = null; }
    if (cls == null) {
        local mid = ::__gekko_state._next_id++;
        ::__gekko_state._seen[v] <- mid;
        return "I" + mid + ":0:{}";
    }

    local my_id = ::__gekko_state._next_id++;
    ::__gekko_state._seen[v] <- my_id;

    // Enumerate members from the class. For each member, the iteration
    // value `cdef` is the class-level default/binding. If `cdef` is a
    // function (a method, OR a Sqrat-bound C++ accessor) we must NOT
    // read v[k]: for Sqrat-bound classes that routes through the _get
    // metamethod into the native accessor, which has engine-corrupting
    // side effects (the L3 bisect crash). Real data fields have a
    // non-function class default; reading those via v[k] hits the
    // instance's raw _values slot — no metamethod, safe. The C++ core
    // of Sqrat actors is captured separately by save_state_to_buf's
    // memcpy; this walk picks up the Squirrel derived members.
    local keys = [];
    local is_fn = {};
    foreach (k, cdef in cls) {
        keys.append(k);
        local ct = typeof cdef;
        is_fn[k] <- (ct == "function" || ct == "closure" ||
                     ct == "nativeclosure" || ct == "class" ||
                     ct == "userdata" || ct == "thread");
    }
    keys.sort();
    local s = "I" + my_id + ":" + keys.len() + ":{";
    ::__gekko_state._cur_depth++;
    foreach (k in keys) {
        // Blacklisted keys + function-typed members: emit `?;` (skip
        // sentinel) without reading v[k]. load_into skips it on read.
        if ((k in ::__gekko_state._skip_keys) || is_fn[k]) {
            s += ::__gekko_state.ser(k) + "?;";
            continue;
        }
        local val;
        try { val = v[k]; } catch (_) { val = null; }
        local vt = typeof val;
        if (vt == "function" || vt == "closure" || vt == "nativeclosure" ||
            vt == "userdata" || vt == "thread" || vt == "class")
        {
            s += ::__gekko_state.ser(k) + "?;";
        } else {
            s += ::__gekko_state.ser(k) + ::__gekko_state.ser(val);
        }
    }
    ::__gekko_state._cur_depth--;
    return s + "}";
};

// Deserializer. Returns [value, new_pos]. Instance is returned as a
// table tagged { __instance = true, __id = N, ... }. Reference is
// { __ref = N }.
::__gekko_state.deser <- function (str, pos = 0) {
    if (pos >= str.len()) return [null, pos];
    local c = str[pos];
    pos++;
    switch (c) {
    case 'n':
        return [null, pos + 1];
    case 'i': {
        local end = str.find(";", pos);
        return [str.slice(pos, end).tointeger(), end + 1];
    }
    case 'f': {
        local end = str.find(";", pos);
        return [str.slice(pos, end).tofloat(), end + 1];
    }
    case 'b':
        return [str[pos] == '1', pos + 2];
    case 's': {
        local colon = str.find(":", pos);
        local len = str.slice(pos, colon).tointeger();
        local data = str.slice(colon + 1, colon + 1 + len);
        return [data, colon + 1 + len + 1];
    }
    case 'a': {
        local colon = str.find(":", pos);
        local count = str.slice(pos, colon).tointeger();
        local arr = [];
        pos = colon + 2;
        for (local i = 0; i < count; ++i) {
            local r = ::__gekko_state.deser(str, pos);
            arr.append(r[0]);
            pos = r[1];
        }
        return [arr, pos + 1];
    }
    case 't': {
        local colon = str.find(":", pos);
        local count = str.slice(pos, colon).tointeger();
        local tbl = {};
        pos = colon + 2;
        for (local i = 0; i < count; ++i) {
            local kr = ::__gekko_state.deser(str, pos);
            local vr = ::__gekko_state.deser(str, kr[1]);
            if (kr[0] != null) tbl[kr[0]] <- vr[0];
            pos = vr[1];
        }
        return [tbl, pos + 1];
    }
    case 'I': {
        // I<id>:<count>:{...}
        local colon1 = str.find(":", pos);
        local id = str.slice(pos, colon1).tointeger();
        local colon2 = str.find(":", colon1 + 1);
        local count = str.slice(colon1 + 1, colon2).tointeger();
        pos = colon2 + 2;  // skip ":{"
        local tbl = {};
        tbl.__instance <- true;
        tbl.__id <- id;
        for (local i = 0; i < count; ++i) {
            local kr = ::__gekko_state.deser(str, pos);
            local vr = ::__gekko_state.deser(str, kr[1]);
            if (kr[0] != null) tbl[kr[0]] <- vr[0];
            pos = vr[1];
        }
        return [tbl, pos + 1];
    }
    case 'R': {
        // R<id>;
        local end = str.find(";", pos);
        local id = str.slice(pos, end).tointeger();
        return [{ __ref = id }, end + 1];
    }
    case '?':
        // Skip sentinel: ser emitted `?;` for blacklisted keys, methods,
        // and unsupported types. load_into sees this exact table and
        // leaves the live slot untouched. Genuine null is encoded as
        // `n;` (handled above) — these are distinguishable.
        return [::__gekko_state._skip_marker, pos + 1];
    }
    return [null, pos];
};

// True if x is a class instance (not a scalar / table / array / func).
::__gekko_state._is_instance <- function (x) {
    local t = typeof x;
    return !(t == "null" || t == "integer" || t == "float" || t == "bool" ||
             t == "string" || t == "array" || t == "table" ||
             t == "function" || t == "closure" || t == "nativeclosure" ||
             t == "class" || t == "userdata" || t == "thread" ||
             t == "weakref" || t == "generator");
};

// True if t (a typeof string) is a scalar type.
::__gekko_state._is_scalar_t <- function (t) {
    return t == "integer" || t == "float" || t == "bool" ||
           t == "string" || t == "null";
};

// Apply a deserialized value `data` onto a live target `inst`.
//
// STRICT TYPE MATCHING — the cardinal rule: never write a value whose
// type doesn't match the live slot's type. The deserializer turns
// captured instances into {__instance,...} tables and references into
// {__ref}; a naive copy would write those tables into numeric slots and
// the engine would then do `table - table` arithmetic and crash. So:
//   - {__ref}      → resolve to a live instance; assign ONLY if the live
//                    slot currently holds an instance
//   - {__instance} → recurse into the live instance (don't assign the
//                    table); only if the live slot holds an instance
//   - plain table  → recurse (only if live slot is a table)
//   - array        → element-copy scalars only (skip instance/table elems)
//   - scalar       → assign ONLY onto a scalar slot
// Anything that doesn't match is left alone — the live value wins.
::__gekko_state.load_into <- function (inst, data) {
    if (data == null) return;
    if (typeof data != "table") return;
    foreach (k, v in data) {
        if (k == "__instance" || k == "__id" || k == "__ref") continue;
        // `?;` skip sentinel — walker chose not to capture this slot.
        if (v == ::__gekko_state._skip_marker) continue;
        try {
            if (!(k in inst)) continue;
            local target = inst[k];
            local tt = typeof target;
            local vt = typeof v;

            if (vt == "table" && "__ref" in v) {
                local live = (::__gekko_state._live_by_id != null &&
                              v.__ref in ::__gekko_state._live_by_id)
                    ? ::__gekko_state._live_by_id[v.__ref] : null;
                if (live != null && ::__gekko_state._is_instance(target)) {
                    inst[k] = live;
                }
                continue;
            }
            if (vt == "table" && "__instance" in v) {
                if (::__gekko_state._is_instance(target)) {
                    if (::__gekko_state._live_by_id != null) {
                        ::__gekko_state._live_by_id[v.__id] <- target;
                    }
                    ::__gekko_state.load_into(target, v);
                }
                continue;
            }
            if (vt == "table") {
                // Plain table — recurse so nested values are themselves
                // type-matched (NOT a blind target[kk]=vv copy, which
                // would write deserialized sub-tables into scalar slots).
                if (tt == "table") ::__gekko_state.load_into(target, v);
                continue;
            }
            if (vt == "array") {
                if (tt == "array") {
                    local n = (v.len() < target.len()) ? v.len() : target.len();
                    for (local i = 0; i < n; ++i) {
                        if (::__gekko_state._is_scalar_t(typeof v[i])) {
                            target[i] = v[i];
                        }
                    }
                }
                continue;
            }
            // Scalar — assign only onto a scalar live slot.
            if (::__gekko_state._is_scalar_t(vt) &&
                ::__gekko_state._is_scalar_t(tt))
            {
                inst[k] = v;
            }
        } catch (_) {}
    }
};

::__gekko_state._dumped_actor_type <- false;
::__gekko_state._save_tick <- 0;

// Battle-level slots captured/restored by save_battle/load_battle.
// These drive the round phase machine + timer; everything else on
// ::battle is either a method, a Sqrat-bound engine object, or covered
// by the team_data walk.
::__gekko_state._battle_fields <- [
    "state", "round", "time", "win",
    "demoCount", "count", "winner", "match_num",
    "time_stop_count", "slow_count", "is_time_stop",
    "endWinDemo", "endLoseDemo", "time_unit",
    // Round-phase gates mutated at every round transition (Round_Begin /
    // Round_Fight / KO / TimeUP / RoundReset). Without these, a rollback
    // across a round boundary would restore the timer-enable and
    // contact-test flags to stale values and the round machine diverges.
    "enableTimeCount", "enableTimeUp", "enable_contact_test", "skipDemo",
];

// DIAGNOSTIC: bisect mode. Higher = walks deeper into Sqrat-bound state.
//   0 = save_battle returns "" immediately
//   1 = walk battle scalars only (state/round/time/win)
//   2 = walk teams; actor refs emit I<id>:0:{} (no field enumeration)
//   3 = walk teams; actor depth cap 1 (enumerate Reimu/Marisa fields, but
//       sub-instances like Reimu.team -> team_data emit empty bodies)
//   4 = depth cap 2 (Reimu.team -> team_data walked; team_data.combo ->
//       Combo emitted empty)
//   5 = depth cap 4
//   6 = depth cap 6 (current production target)
::__gekko_state._bisect_level <- 6;

// Snapshot ::battle. Wraps everything in a root table so the serializer
// can walk it with identity tagging. The first instance encountered
// (typically team[0].master) gets saved-id 1, second 2, etc. References
// to already-seen instances become R<id>;.
// Serialize `out`. Prefers the native walker ::__gekko_cpp_ser (bound by
// gekko_bridge — ~20x faster than the Squirrel ser); falls back to the
// Squirrel ser() if the native one is not registered. Both emit the same
// text format, so deser() is unchanged either way.
::__gekko_state._ser_out <- function (out) {
    if ("__gekko_cpp_ser" in ::getroottable()) {
        return ::__gekko_cpp_ser(out, ::__gekko_state._max_depth);
    }
    return ::__gekko_state._ser_out(out);
};

::__gekko_state._vec3_registered <- false;

// checksum_only: raw mode uses this text ONLY as the pointer-independent
// cross-peer checksum (restore comes from snapshot_ring, load_battle is never
// called), so skip the load-support _keep ring — cloning infoActor+task every
// save is pure sq_arena churn that inflates the dirty-page capture cost.
::__gekko_state.save_battle <- function (frame = 0, checksum_only = false) {
    if (::__gekko_state._bisect_level == 0) return "";
    // One-shot: hand the native walker a live Vector3 sample so it can
    // recognize every Vector3 by class pointer and emit x/y/z VALUES
    // (they're Sqrat native accessors — invisible to the member walk —
    // but they carry the actors' velocity vectors: real gameplay state
    // the cross-peer checksum must cover).
    if (!::__gekko_state._vec3_registered &&
        "__gekko_vec3_register" in ::getroottable())
    {
        try {
            local m = ::battle.team[0].master;
            if (m != null && m.va != null) {
                local ok = ::__gekko_vec3_register(m.va, 0);
                // InputGlobal: the per-player decoded device the command
                // reservations key on (device.bN==2 press edges) — emit its
                // C++ payload so an input-edge divergence shows in the text
                // diff at the frame it happens.
                if (m.command != null && m.command.device != null) {
                    ::__gekko_vec3_register(m.command.device, 1);
                }
                // P1's device: Dr0 write-watch target (writer attribution).
                local m1 = ::battle.team[1].master;
                if (m1 != null && m1.command != null && m1.command.device != null) {
                    ::__gekko_vec3_register(m1.command.device, 2);
                }
                ::__gekko_state._vec3_registered = ok;
            }
        } catch (_e) {}
    }
    ::__gekko_state._seen = {};
    ::__gekko_state._next_id = 1;
    ::__gekko_state._cur_depth = 0;
    // Depth cap drives how far ser() recurses into instance branches.
    // 0 = first-sight instance emits empty body (L0..L2).
    local lvl = ::__gekko_state._bisect_level;
    ::__gekko_state._max_depth = (lvl <= 2) ? 0
                               : (lvl == 3) ? 1
                               : (lvl == 4) ? 2
                               : (lvl == 5) ? 4
                                            : 6;

    local out = {};
    if (!("battle" in this.getroottable())) return ::__gekko_state._ser_out(out);

    // Battle-level phase-machine + round state. `battleUpdate` is the
    // per-frame phase function pointer (Game_BeginUpdate → ReadyUpdate →
    // BattleUpdate → KO); `demoCount` is the per-phase transition
    // counter. Without these, a rollback restores `time`/`state` but
    // leaves the phase machine at the live (wrong) frame — the round
    // phases drift out of step with the timer.
    foreach (f in ::__gekko_state._battle_fields) {
        if (f in ::battle) out[f] <- ::battle[f];
    }
    // Park the live-value side table for this frame: battleUpdate (a
    // closure — named method OR an inline anonymous one), infoActor (array
    // of actor weakrefs) and ::battle.task (task instance table). These
    // are NOT in the text blob; load_battle restores them from _keep.
    // infoActor/task are shallow-cloned so a later in-place mutation of
    // the live container can't corrupt the parked snapshot.
    if (!checksum_only) {
        local slot = frame % ::__gekko_state._keep_ring;
        local ia = ("infoActor" in ::battle) ? ::battle.infoActor : null;
        local tk = ("task" in ::battle) ? ::battle.task : null;
        ::__gekko_state._keep[slot] = {
            battleUpdate = ("battleUpdate" in ::battle) ? ::battle.battleUpdate : null,
            infoActor    = (typeof ia == "array") ? (clone ia) : ia,
            task         = (typeof tk == "table") ? (clone tk) : null,
        };
    }
    // Periodic snapshot log so we can see the round phase machine
    // actually advancing (state 2->4->8->64) across the match.
    ::__gekko_state._save_tick = (::__gekko_state._save_tick + 1) % 1200;
    if (::__gekko_state._save_tick == 0) {
        local ewd = ("endWinDemo" in ::battle && ::battle.endWinDemo != null)
            ? (::battle.endWinDemo[0] + "/" + ::battle.endWinDemo[1]) : "?";
        local bu = "?";
        if ("battleUpdate" in ::battle && ::battle.battleUpdate != null) {
            bu = "anon";
            foreach (k, val in ::battle) {
                if (k != "battleUpdate" && typeof val == "function" &&
                    val == ::battle.battleUpdate) { bu = k; break; }
            }
        }
        ::print("[gekko_state] snapshot state=" + ("state" in out ? out.state : "?")
                + " demoCount=" + ("demoCount" in out ? out.demoCount : "?")
                + " time=" + ("time" in out ? out.time : "?")
                + " endWinDemo=" + ewd
                + " bu=" + bu + "\n");
    }
    if (::__gekko_state._bisect_level >= 2 && "team"  in ::battle) {
        // Put the team_data instances (PlayerTeamData — pure Squirrel)
        // straight into the output. ser() walks them, capturing combo
        // state, target ref, gauges, etc. The instance branch will
        // detect t.master / t.slave as Sqrat-bound and emit empty
        // bodies for them — C++ memcpy in save_state_to_buf handles
        // their core data.
        local teams = [];
        foreach (t in ::battle.team) {
            teams.append(t);
        }
        out.teams <- teams;
        if (!::__gekko_state._dumped_actor_type) {
            ::__gekko_state._dumped_actor_type = true;
            local t0 = ::battle.team[0];
            local lt = (t0 != null && "master" in t0 && t0.master != null)
                ? ::__gekko_state._pretty_type(typeof t0.master) : "absent";
            ::print("[gekko_state] team[0].master sqrat-type=" + lt + "\n");
        }
    }
    return ::__gekko_state._ser_out(out);
};

::__gekko_state._load_log_quota <- 4;
::__gekko_state.load_battle <- function (str, frame = 0) {
    if (::__gekko_state._load_log_quota > 0) {
        ::__gekko_state._load_log_quota--;
        ::print("[gekko_state] load_battle called len=" + str.len() + "\n");
    }
    local r = ::__gekko_state.deser(str, 0);
    local data = r[0];
    if (data == null) {
        ::print("[gekko_state] load_battle: deser returned null\n");
        return;
    }
    if (!("battle" in this.getroottable()) || ::battle == null) {
        ::print("[gekko_state] load_battle: ::battle missing or null\n");
        return;
    }

    ::__gekko_state._live_by_id = {};

    local pre_time  = ("time"  in ::battle) ? ::battle.time  : null;
    local pre_state = ("state" in ::battle) ? ::battle.state : null;
    // Restore the battle-level fields. Arrays (win/endWinDemo/...) are
    // copied element-wise so any cached reference to the live array
    // stays valid; scalars are assigned directly.
    foreach (f in ::__gekko_state._battle_fields) {
        if (!(f in data) || !(f in ::battle)) continue;
        local sv = data[f];
        if (sv == ::__gekko_state._skip_marker) continue;
        if (typeof sv == "array" && typeof ::battle[f] == "array") {
            local live = ::battle[f];
            local n = (sv.len() < live.len()) ? sv.len() : live.len();
            for (local i = 0; i < n; ++i) live[i] = sv[i];
        } else if (sv != null) {
            ::battle[f] = sv;
        }
    }
    // Restore the live-value side table parked by save_battle for this
    // frame: battleUpdate closure, infoActor weakref array, task table.
    local kept = ::__gekko_state._keep[frame % ::__gekko_state._keep_ring];
    if (kept != null) {
        if ("battleUpdate" in ::battle) {
            ::battle.battleUpdate = kept.battleUpdate;
        }
        if ("infoActor" in ::battle) {
            ::battle.infoActor = kept.infoActor;
        }
        // Rebuild ::battle.task IN PLACE — a caller may hold the table
        // reference, so we mutate the live table rather than replace it.
        if (kept.task != null && "task" in ::battle &&
            typeof ::battle.task == "table")
        {
            local live = ::battle.task;
            local old_keys = [];
            foreach (k, _ in live) old_keys.append(k);
            foreach (k in old_keys) delete live[k];
            foreach (k, val in kept.task) live[k] <- val;
        }
    }
    if (::__gekko_state._load_log_quota > 0) {
        ::print("[gekko_state] restore time " + pre_time + " -> " + ::battle.time
                + ", state " + pre_state + " -> " + ::battle.state
                + ", kept=" + (kept != null ? "y" : "n")
                + "\n");
    }
    if ("teams" in data && "team" in ::battle) {
        // New save layout (post-Sqrat-skip): teams[i] is the team_data
        // (PlayerTeamData) instance directly. Walk its slots back onto
        // the live team_data, and register .master/.slave references in
        // _live_by_id so any nested R<id> references resolve to live
        // Sqrat actors.
        for (local ti = 0; ti < data.teams.len() && ti < ::battle.team.len(); ++ti) {
            local sav = data.teams[ti];
            local team = ::battle.team[ti];
            if (team == null || sav == null) continue;
            if (typeof sav != "table" || !("__instance" in sav)) continue;
            // Register team_data id -> live team_data
            ::__gekko_state._live_by_id[sav.__id] <- team;
            // Inside sav, master/slave were emitted as I<id>:0:{} (Sqrat).
            // Map those ids to the live actor instances NOW so any R<id>
            // refs scattered elsewhere in the save (e.g. team_data.target
            // points to the opposing team's master) resolve correctly.
            if ("master" in sav && typeof sav.master == "table" &&
                "__instance" in sav.master && "master" in team && team.master)
            {
                ::__gekko_state._live_by_id[sav.master.__id] <- team.master;
            }
            if ("slave" in sav && typeof sav.slave == "table" &&
                "__instance" in sav.slave && "slave" in team && team.slave)
            {
                ::__gekko_state._live_by_id[sav.slave.__id] <- team.slave;
            }
            // Walk back into team_data — restores combo, gauges, target
            // ref, etc.
            ::__gekko_state.load_into(team, sav);
        }
    }
};

# Netcode Selector (Delay vs Rollback) + Real Lobby Flow — Plan

**Goal:** let players choose delay netcode (vanilla) or GekkoNet rollback, negotiated per match, over the real lobby→CSS→battle flow. Ship-ready online.

---

## 1. What already exists (don't rebuild)

Squiroll keeps th155's **native delay netcode + native lobby fully intact** for matchmaking / menus / CSS / round-intro, and **only hijacks the in-battle frame loop** with rollback (arm at `Round_Fight` / `battle.state==8`, disarm at round end, re-arm next round). So the two netcodes already **coexist per match**: delay drives the handshake/menus/intro/transitions, rollback drives the interactive fight.

The delay-vs-rollback switch **already exists** — one flag:
- `gekko_enabled` (`config.cpp:101`, env `SQUIROLL_GEKKO_ENABLED`) → `::setting.network.gekko_enabled` (`plugin.cpp:207`).
- **false → native delay netcode** (squiroll stays out of the fight loop). **true → GekkoNet rollback layered on.**
- Already forks correctly in both the auto_connect test path (`boot.nut:83`) and the real online flow (`network_component.nut` `AcceptMatch:550` / `BeginMatch:489`, each gated `if (::setting.network.gekko_enabled)`).

The match flow (host; client symmetric):
```
lobby match → AcceptMatch (rand_seed exchanged, DECISION POINT)
            → [gekko_enabled? arm gekko_watch_for_fight_dual]
            → CSS (delay netcode)
            → round intro (delay, deterministic from shared rand_seed)
            → Round_Fight (state==8) → pre_arm_poll fires gekko init() → ROLLBACK
            → round end → disarm → delay for transition → re-arm next round
```

Rollback session real entry: `gekko_bridge::init(local_port, remote_port, local_idx, remote_ip)` (`gekko_bridge.cpp:3150`), armed via `gekko_watch_for_fight_dual` (`network_component.nut:550/489`), fired by `pre_arm_poll` at fight-frame-0 (`gekko_bridge.cpp:3778`).

Also built: UI framework (`::UI.Menu`) + a config-page template with Enum toggles (`mod_config.nut`), and a stubbed-but-ready Network config page (`network_config.nut`, body commented out).

---

## 2. What's missing (the 3 things to build)

1. **Endpoint wiring (the #24 gap).** The arm call currently uses config `peer_ip`/`peer_port` + fixed `+10/+11` port offsets (`network_component.nut:490-493, 551-554`) — two-local-instance testing only. Real internet play needs the **NAT-punched endpoint** (`punch_ip_buffer`, `netcode.cpp:452`, exposed as `::punch.ip_available`) fed into the arm.
2. **Peer agreement (negotiation).** Today each client reads its OWN `gekko_enabled` independently — nothing guarantees both run the same netcode (mixed = instant desync). The choice must be exchanged in the match handshake and resolved to one value both sides compute identically.
3. **Selector UI.** `gekko_enabled` is only settable via config.ini / env — no in-game UI.

---

## 3. Design decisions (need your call)

### D1 — Where the choice lives
- **(A, recommended) Global preference + negotiation.** A persistent "Netcode: Delay / Rollback" toggle in the config page; the per-match resolution is handled by negotiation (§4). Simple, and it gives the full behavior: you set your preference, rollback happens iff both peers prefer+support it.
- (B) Per-match choice surfaced in the online menu at match time. Better UX, more work (new UI in `network.nut`). Can layer on later.

### D2 — Negotiation rule (who wins)
- **(recommended) Symmetric AND.** Each peer advertises `{rollback_capable, rollback_wanted}`. Both compute `use_rollback = my.capable && my.wanted && peer.capable && peer.wanted`. Identical on both sides → no disagreement. Any peer lacking capability (vanilla th155, older squiroll) or preferring delay → **delay**. This preserves cross-play with vanilla opponents (they never advertise the flag → delay).
- Alternative: host decides. Simpler but the client can't refuse rollback on a bad connection. AND-rule is safer + player-respecting.

### D3 — Rollout order
- **(recommended)** Endpoint wiring FIRST (unblocks real internet play at all), then negotiation, then UI. Each is independently testable.

---

## 4. Negotiation protocol (concrete)

The `AcceptMatch` "yes" handshake payload (`network_component.nut:531-540`) already carries `rand_seed`. Add one netcode byte next to it:
```
byte netcode_flags = (rollback_capable ? 0x1 : 0) | (rollback_wanted ? 0x2 : 0)
```
- `rollback_capable` = this build supports rollback (a compile constant / version gate).
- `rollback_wanted` = `::setting.network.gekko_enabled` (the player's preference).

Both peers exchange flags (host in AcceptMatch reply, client in BeginMatch request — the message goes both ways), then each computes:
```
use_rollback = local.capable && local.wanted && remote.capable && remote.wanted
```
Store `use_rollback` as the **negotiated per-match value**. Replace the two `if (::setting.network.gekko_enabled)` gates (`network_component.nut:489, 550`) with `if (use_rollback)`.

**Safety:** because both sides compute the same AND over the same exchanged bytes, they can't disagree. Add a one-line assert/log at arm time so a protocol bug surfaces loudly instead of desyncing.

**Version gate:** `rollback_capable` lets a future rollback protocol change be advertised, so mismatched squiroll versions fall back to delay instead of desyncing.

---

## 5. Endpoint wiring (#24)

Replace, in `network_component.nut` `AcceptMatch`/`BeginMatch`:
```
gekko_watch_for_fight_dual(base+10, base+11, 0, ::setting.network.peer_ip)   // testing
```
with the punched endpoint from the lobby punch handshake (`::punch.ip_available` / `punch_ip_buffer`), parsing `ip:port` and passing real values. Keep the config `peer_ip` path as a fallback for the two-local-instance test rig (gate on auto_connect).

Test matrix: (a) two LAN instances (current), (b) two real internet peers via the lobby + NAT punch. Confirm the arm gets the correct remote endpoint and the session connects.

---

## 6. Selector UI

Fastest home (matches existing template exactly): add an Enum to `mod_config.nut` (or re-enable `network_config.nut`), mirroring the hitbox/discord toggles:
```
::UI.Menu.Enum("Netcode", ["Delay", "Rollback"], current, cb)
// cb: ::setting.save("network", "gekko_enabled", (idx==1).tostring())
```
Persisted via the existing config plumbing; read as `::setting.network.gekko_enabled` = the `rollback_wanted` input to negotiation.

---

## 7. Full validation plan (#24 + the sibling gaps)

Once the 3 pieces land, validate the REAL flow (not auto_connect) end to end:
- **#24 flow:** real lobby → match → CSS → battle, for each negotiated outcome:
  - both prefer rollback + both capable → **rollback** (verify 0 desync over a full match).
  - either prefers delay / one vanilla → **delay** (verify vanilla-compatible).
- **#22 multi-round:** CSS → round 1 → round 2 → match end → back to CSS/lobby, rollback arming/disarming each round cleanly.
- **#29 disconnect:** peer drop mid-match → unwind to menu (currently closes the game — needs a GekkoNet disconnect-event handler that tears down the session and returns to the online menu instead of exiting).
- **#23 plugin guards:** input_display / ping_display / GetDelay mod plugins behave across the delay↔rollback transition and round ends.

---

## 8. Files to touch

- `embed/network/network_component.nut` — negotiation compute + drive the arm gates + endpoint wiring (primary).
- `embed/network/network.nut` — handshake payload (add netcode byte); optional per-match UI (D1-B).
- `embed/config/mod_config.nut` (or re-enable `network_config.nut`) — the selector Enum.
- `plugin.cpp` / `netcode.cpp` — only if the punched endpoint needs new Squirrel bindings to reach the arm call.
- (`gekko_bridge.cpp` — a defensive log/assert that arm-time netcode matches the negotiated value.)

---

## 9. Risk notes

- **Determinism seed** already exchanged (`rand_seed`, srand at AcceptMatch) — rollback + delay both rely on it; unchanged.
- **Per-match coexistence** (delay for menus/intro/transitions, rollback for fight) is the existing, proven design — the selector doesn't change it, only which mode the *fight* uses.
- **Vanilla cross-play** preserved by the capability flag (no flag → delay).
- **The only truly new failure mode** is a negotiation bug making peers disagree — mitigated by symmetric AND + an arm-time assert.

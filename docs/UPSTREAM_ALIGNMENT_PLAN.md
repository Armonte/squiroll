# Upstream Alignment: merge Daze's menu rewrite ⟷ local-dev's GekkoNet rollback

**Goal:** bring local-dev (GekkoNet rollback engine) onto Daze's new menu/UI/netplay structure, so the netcode selector (#43/#44/#45) is built the way the menus are actually going.

---

## 1. The divergence (as of 2026-07-08)

Split point: **`b944def` (2026-05-21)**. Since then:
- **local-dev: +181 commits** — the entire GekkoNet rollback engine (`gekko_bridge`, `snapshot_ring`, dirty-page arenas, this session's M4 + 60fps + hardening). **Only place GekkoNet exists.**
- **Daze's `upstream/dev`: +14 commits** — a menu/UI rewrite (`aa61e0b` new UI framework → `2e0b321` "menus rewrite wip"), new plugins (oki_dummy, frame_bar, framerate_control), F# lobby-server rework. **Has NO GekkoNet** — only the old prototype `rollback.cpp`/`rollback.nut` (diff-based, commented out). His tip is **WIP with matchmaking broken**.

They are **complementary**, not competing: rollback engine (Armonte) + menu/lobby rewrite (Daze).

## 2. Merge scope — TRACTABLE

Only **13 files overlap**; trial merge = ~11 conflicts, most trivial:
- **Trivial:** `.gitignore`, `build.sh`, `embed_manifest.txt`, `config.h`.
- **Config/bindings:** `config.cpp`, `plugin.cpp` — both added network config keys / Squirrel bindings; merge = union.
- **`network_config.nut`:** Daze DELETED it (folded into his new UI); local modified it → take Daze's deletion, move any unique settings into his new network menu.
- **Meaningful (need care):**
  - `network_component.nut` — his handshake refactor vs your gekko-arm (#43/#44 site). The arm must move to his new netplay structure.
  - `loop.nut` — his menu-loop changes vs your `better_game_loop` / rollback `tick()` takeover.
  - `battle.nut` — your rollback arm/disarm hooks vs his edits.
  - plugins `input_display.nut` / `ping_display.nut` / `manager.nut` — his plugin-API refactor vs your rollback-aware tweaks.

Daze's ~40 other changed files are **new, non-conflicting** (UI framework, netplay.nut, config_base.nut, new plugins).

## 3. Daze's menu design (align to this)

New framework is **declarative + config-pointer-bound**:
- Menus: `::UI.Menu.Create.call(this, ::UI.Menu.Page(...), ...)` with items `::UI.Network.Buttons.*`, `::UI.Menu.Value.String`, `::UI.Menu.Enum.Boolean`.
- Config binding: `::UI.Config.SquirollPTR(::setting.network, item, "network", key)` / `VanillaPTR(::config.network, item)`.
- Text/localization: `item.csv` + an `add(key, jp[], en[])` helper.
- New netplay flow lives in `netplay.nut` (`WaitInLobby`/`Host`/`Connect`/`FoundMatch`/`IsConnecting`) + `netplay_update.nut` (match handshake) + `network_new.nut` (the menu).

**Netcode selector in this design** = a `::UI.Menu.Enum` (Delay/Rollback) on a network page, bound via `SquirollPTR(::setting.network, ..., "network", "gekko_enabled")`, text added through `add("netcode", ["Netcode(jp)","Delay","Rollback"], ["Netcode","Delay","Rollback"])`. Per-match value flows into the handshake (netplay_update.nut).

## 4. Alignment steps (proposed)

1. **Integration branch** off local-dev (keep local-dev stable): `merge-daze-menus`.
2. **Merge `upstream/dev` (2e0b321)**; resolve the ~11 conflicts (trivial ones first; then network_component.nut / loop.nut / battle.nut / config).
3. **Re-wire the gekko-arm** into Daze's new netplay structure — the `gekko_watch_for_fight_dual` call moves from the old `network_component.nut` AcceptMatch/BeginMatch to his match-accept path (netplay.nut / netplay_update.nut). The C++ engine is unchanged; only the Squirrel arm site moves.
4. **Get matchmaking working** — his tip is WIP-broken; fix + validate delay netcode first (coordinate with Daze), then rollback arm.
5. **Build #43/#44/#45 on the merged base** using his UI framework + netplay structure:
   - #43 endpoint wiring (punched socket → gekko adapter) — C++, unaffected by the menu merge.
   - #44 negotiation — in netplay_update.nut's handshake.
   - #45 selector UI — `::UI.Menu.Enum` per §3.
6. **Validate** real lobby→CSS→battle for delay + rollback (two clients).

## 4b. MERGE BASE — use `67c4433`, not the broken-WIP tip (2026-07-08)

Comparison of merge scope onto local-dev:
- **`67c4433` (Jun 21, "Merge PR #30 practice") — RECOMMENDED BASE.** Already contains Daze's **new UI framework (`embed/UI`), `netplay.nut`, `network_new.nut`** — i.e. the full menu/netplay design direction — with **working matchmaking**. **10 conflicts.**
- `2e0b321` ("menus rewrite wip") — his BROKEN tip (matchmaking broken). Adds 2 more conflicts (`loop.nut` ↔ your `better_game_loop` takeover, `embed_manifest.txt`) and imports the breakage. **12 conflicts.**

→ Merge `67c4433` (stable, has the architecture); merge his `2e0b321` menus-rewrite as a **separate later increment** once he lands it. Align the selector DESIGN to his rewrite direction regardless (studied from 2e0b321 — the `::UI.Menu.Enum` fit holds).

Conflict resolutions (both bases): `.gitignore` union · `build.sh` = **keep local-dev's** (incremental/cached, far ahead of his simple one) · `config.cpp`/`plugin.cpp` = union of additions · `network_config.nut` = take his deletion (folded into new UI) · **semantic (care):** `battle.nut`/`network_component.nut` (arm site) / `manager.nut`/`input_display.nut`/`ping_display.nut` (his plugin-API refactor ↔ your rollback tweaks).

## 5. Open coordination questions (with Daze)

- **Timing:** merge his WIP-broken tip now (on the integration branch, help fix matchmaking) vs wait for his rewrite to stabilize? His "moving lobby logic breaks matchmaking" is unresolved.
- **Lobby server:** he reworked `src/lobby/` (F#). Does the punch/endpoint flow (#43) change with his new server?
- **Direction of merge:** merge Daze→local (14 commits, integrate his menus) is far smaller than rebasing local's 181 onto his dev. Recommend Daze→local on an integration branch.

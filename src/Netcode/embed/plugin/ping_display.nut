config = {
    enabled = true
    simple = false
    great_threshold = 60
    good_threshold = 130
    bad_threshold = 200
};

// ============================================================================
// Netcode HUD — live proof the GekkoNet rollback is running. Each stat is its
// own coloured segment (one colour per stat, Touhou-ish palette, no ugly blue),
// laid out centred along the bottom.
//
// [#30] RENDER PATH: this now draws through the NATIVE, rollback-safe
// immediate-mode HUD (::hud.*, backed by squiroll's own D3D in overlay.cpp) —
// NOT th155's UI.Core.Text / DrawCommandSlot machinery. That machinery lives in
// the rolled-back arenas, so a HUD built on it HUNG deterministically right
// after the first rollback (verified rb 0->1). The native HUD keeps nothing in
// any snapshot, so it is rollback-safe by construction: every Update we
// ::hud.clear() then re-emit the whole HUD forward-only (immediate mode).
//
// Reads ::__gekko_netstats() (native, gekko_bridge): { active, ping, avg,
// jitter, sent, recv, ahead, rb, fwd, frame, desync, delay, runahead, simfps,
// rfps }. Everything is in one try/catch so a HUD read can never halt the sim
// at a round-end transition. Stays async=true; since it no longer creates any
// render objects, the M3 render_arena bracket in battle.nut still applies but is
// harmless. The queue is cleared on battle teardown by battle.Release().
// ============================================================================
class modifier extends modifier {
	async = true;
	diag_done = false;

	// Render scale for the native 8x8 font: SCALE px per font-pixel, so each
	// monospace cell is 8*SCALE px. Single source of truth — passed to
	// ::hud.text AND used for the layout math, so they can never desync.
	SCALE = 2.0;

	// Touhou palette (RGB 0..1). Deliberately NO flat ugly blue.
	COL = {
		label = [0.62, 0.66, 0.74]   // dim slate for the little labels
		delay = [0.55, 0.82, 0.98]   // Cirno ice — a GOOD blue
		ping_good = [0.42, 0.92, 0.55]  // green
		ping_ok   = [1.00, 0.82, 0.32]  // gold
		ping_bad  = [0.98, 0.36, 0.40]  // red
		jitter = [1.00, 0.84, 0.30]  // Marisa gold
		ahead  = [0.76, 0.58, 0.98]  // Patchouli lavender
		rb     = [1.00, 0.50, 0.66]  // Reimu pink — the star stat
		frame  = [0.90, 0.92, 0.97]  // soft white
		fps_good = [0.45, 0.94, 0.58]  // render fps at/near 60
		fps_bad  = [1.00, 0.60, 0.30]  // render fps dropping
		ok     = [0.45, 0.94, 0.58]  // sync green
		bad    = [0.98, 0.26, 0.32]  // desync red
	};

	// One-decimal float -> string, no ::format (which threw on some builds).
	function f1(v) {
		local neg = v < 0.0;
		if (neg) v = -v;
		local x = ((v * 10.0) + 0.5).tointeger();
		local s = (x / 10) + "." + (x % 10);
		return neg ? ("-" + s) : s;
	}

	function Update() {
		try {
			// Immediate mode: clear + re-emit the whole HUD every frame.
			::hud.clear();

			local s = ("__gekko_netstats" in ::getroottable())
				? ::__gekko_netstats() : null;

			// Build the segment list: [text, colour].
			local parts = [];
			if (s != null && s.active) {
				::rollback.update_delay(s.ping.tointeger());
				local cfg = ::plugin.cfg.ping_display.data;
				local pv = s.ping.tointeger();
				local pcol = (pv > cfg.bad_threshold) ? COL.ping_bad
					: (pv > cfg.good_threshold) ? COL.ping_ok : COL.ping_good;

				// sim/render fps: sim is the pinned deterministic 60, render
				// is the real presented rate (dips under load).
				local fpscol = (s.rfps >= 58) ? COL.fps_good : COL.fps_bad;
				parts.append([s.simfps + "/" + s.rfps + "fps", fpscol]);
				parts.append(["dly" + s.delay, COL.delay]);
				parts.append([pv + "ms", pcol]);
				parts.append(["j" + f1(s.jitter), COL.jitter]);
				parts.append(["ahd" + f1(s.ahead), COL.ahead]);
				parts.append(["RB" + s.rb, COL.rb]);          // the money stat
				parts.append(["f" + s.frame, COL.frame]);
				if (s.runahead > 0)
					parts.append(["ra" + s.runahead, COL.label]);
				parts.append([s.desync > 0 ? ("DESYNC" + s.desync) : "SYNC",
					s.desync > 0 ? COL.bad : COL.ok]);

				if (!diag_done) {
					diag_done = true;
					local dbg = "";
					foreach (p in parts) dbg += p[0] + " ";
					::print("[hud] " + dbg + "\n");
				}
			} else {
				// Pre-session / menu: just show the delay-based ping.
				local d = ::network.GetDelay();
				::rollback.update_delay(d);
				parts.append(["ping " + d, COL.frame]);
			}

			// Layout: monospace cell = 8*SCALE px. Measure, centre, place L->R.
			local cell = 8.0 * SCALE;   // char advance / cell height
			local gap = cell;           // one cell between segments
			local widths = [];
			local total = 0.0;
			for (local i = 0; i < parts.len(); ++i) {
				local w = parts[i][0].len() * cell;
				widths.append(w);
				total += w + (i > 0 ? gap : 0);
			}

			local x = 640.0 - (total / 2.0);
			local y = 720.0 - cell - 6.0;   // sit near the bottom edge
			for (local i = 0; i < parts.len(); ++i) {
				local col = parts[i][1];
				// a=1.0, scale=SCALE passed explicitly (no default-coupling).
				::hud.text(x, y, parts[i][0], col[0], col[1], col[2], 1.0, SCALE);
				x += widths[i] + gap;
			}
		} catch (_e) {}
	}

	function Enabled(param) {
		return (::network.IsPlaying() && ::plugin.cfg.ping_display.data.enabled);
	}
};

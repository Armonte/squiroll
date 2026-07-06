config = {
    enabled = true
    simple = false
    great_threshold = 60
    good_threshold = 130
    bad_threshold = 200
};

// ============================================================================
// Netcode HUD — live proof the GekkoNet rollback is running. Each stat is its
// own coloured Text segment (one colour per stat, Touhou-ish palette, no ugly
// blue), laid out centred along the bottom. Reads ::__gekko_netstats() (native,
// gekko_bridge): { active, ping, avg, jitter, sent, recv, ahead, rb, fwd,
// frame, desync, delay, runahead }. Everything is in one try/catch so a HUD
// read can never halt the sim at a round-end transition.
// ============================================================================
class modifier extends modifier {
	async = true;
	segs = null;          // array of Text objects, one per stat
	diag_done = false;

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
		ok     = [0.45, 0.94, 0.58]  // sync green
		bad    = [0.98, 0.26, 0.32]  // desync red
	};

	constructor() {
		segs = [];
		// Up to 8 segments: delay, ping, jitter, ahead, rb, frame, status, (ra)
		// Render on the UI slot at a high layer (60000, same as the network
		// name/text overlays) so we draw ON TOP of the game HUD — the old
		// status/layer-1 put us behind it (game HUD icons sit at status ~3000).
		for (local i = 0; i < 9; ++i) {
			local t = ::UI.Core.Text("");
			t.ConnectRenderSlot(::graphics.slot.ui, 60000);
			t.sx = 1.0;   // full size (the 0.62 shrink squeezed the text)
			t.sy = 1.0;
			segs.push(t);
		}
	}

	// One-decimal float -> string, no ::format (which threw on some builds).
	function f1(v) {
		local neg = v < 0.0;
		if (neg) v = -v;
		local x = ((v * 10.0) + 0.5).tointeger();
		local s = (x / 10) + "." + (x % 10);
		return neg ? ("-" + s) : s;
	}

	// Stamp segment i with text + colour; returns its scaled width.
	function put(i, str, col) {
		local t = segs[i];
		t.Set(str);
		t.red   = col[0];
		t.green = col[1];
		t.blue  = col[2];
		return t.width * t.sx;
	}

	function Update() {
		try {
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

			// Layout: measure, centre, place left-to-right with a gap.
			local gap = 14.0;
			local widths = [];
			local total = 0.0;
			for (local i = 0; i < parts.len(); ++i) {
				local w = put(i, parts[i][0], parts[i][1]);
				widths.append(w);
				total += w + (i > 0 ? gap : 0);
			}
			// Hide any leftover segments from a longer previous frame.
			for (local i = parts.len(); i < segs.len(); ++i) segs[i].Set("");

			local x = 640.0 - (total / 2.0);
			local y = 714.0;
			for (local i = 0; i < parts.len(); ++i) {
				local t = segs[i];
				t.x = x;
				t.y = y - (t.height * t.sy);
				x += widths[i] + gap;
			}
		} catch (_e) {}
	}

	function Enabled(param) {
		return (::network.IsPlaying() && ::plugin.cfg.ping_display.data.enabled);
	}
};

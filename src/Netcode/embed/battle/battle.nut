::manbow.CompileFile("data/script/battle/battle_team.nut", this);
::manbow.CompileFile("data/script/battle/battle_on_move.nut", this);
::manbow.CompileFile("data/script/battle/battle_on_hit.nut", this);
::manbow.CompileFile("data/script/battle/battle_param.nut", this);
::manbow.CompileFile("data/script/battle/battle_update.nut", this);

function InitializeUser(){}
function UpdateUser(){}
function TerminateUser(){}

::manbow.CompileFile("data/actor/script/battle.nut", this);

gauge <- {};
::manbow.CompileFile("data/actor/status/gauge_common.nut", gauge);
::manbow.CompileFile("data/actor/status/spellcard.nut", this);
::manbow.CompileFile("data/actor/status/combo.nut", this);
::manbow.CompileFile("data/actor/status/bgm_title.nut", this);
game_mode <- -1;
modifiers <- {};
class InitializeParam {
	game_mode = 1;
	seed = 0;
	team = [
		null,
		null
	];
	constructor() {
		team = [
			{},
			{}
		];

		foreach( i, v in team ) {
			v.device_id <- -2;
			v.master <- null;
			v.slave <- null;
			v.slave_sub <- null;
			v.master_spell <- 0;
			v.slave_spell <- 0;
			v.slave_sub_spell <- 0;
			v.start_x <- ::battle.start_x[i];
			v.start_y <- ::battle.start_y[i];
			v.start_direction <- ::battle.start_direction[i];
		}
	}
}

name_overrides <- {
    hijiri = "Byakuren",
    sinmyoumaru = "Shinmyoumaru",
    usami = "Sumireko",
    udonge = "Reisen",
    jyoon = "Joon"
};

function _LookupName(name) {
	return (name in name_overrides) ? name_overrides[name] : (name.slice(0, 1).toupper() + name.slice(1, name.len()));
}

function _HideNames() {
	// ::setting.network.update_consts();
	if (::network.IsActive() && ::setting.network.hide_opponent_name) {
		::network.player_name[0] = "P1";
		::network.player_name[1] = "P2";
	}
}

function _SetRPC(param) {
	if (::replay.GetState() == ::replay.PLAY && ::network.inst == null)::discord.rpc_set_details("Watching a replay");
	::discord.rpc_set_state(param.game_mode < 10 ? "Starting match" : "");
	::discord.rpc_set_large_img_key("stage" + ::stage.background.id);
	if (::replay.GetState() != ::replay.PLAY && (::network.IsPlaying() || (param.game_mode != 1 && param.game_mode != 10))) {
		local p1m = param.team[0].master.name;
		local p1s = param.team[0].slave.name;
		local p2m = param.team[1].master.name;
		local p2s = param.team[1].slave.name;

		::discord.rpc_set_small_img_key(::network.IsPlaying() ? (::network.is_parent_vs ? p2m : p1m) : p1m);
		::discord.rpc_set_small_img_text(format("%s/%s vs %s/%s", _LookupName(p1m), _LookupName(p1s), _LookupName(p2m), _LookupName(p2s)));
	}
	::discord.rpc_commit();
}

function _SetCoreFunctions() {
	::manbow.SetTerminateFunction(function () {
		::battle.Release();
	});
	Update = UpdateMain;
	::manbow.CompileFile("data/script/battle/battle_param.nut", this);
	::manbow.CompileFile("data/script/battle/battle_team.nut", this);
}

function _SetupBattleFuncs(param) {
	switch(param.game_mode) {
	case 10:
		::manbow.CompileFile("data/actor/status/gauge_story.nut", gauge);
		::manbow.CompileFile("data/script/battle/battle_story.nut", this);
		break;

	case 40:
		::manbow.CompileFile("data/actor/status/gauge_vs.nut", gauge);
		::manbow.CompileFile("data/script/battle/battle_practice.nut", this);
		break;

	case 30:
		::manbow.CompileFile("data/actor/status/gauge_vs.nut", gauge);
		::manbow.CompileFile("data/script/battle/battle_tutorial.nut", this);
		break;

	default:
		::manbow.CompileFile("data/actor/status/gauge_vs.nut", gauge);
		::manbow.CompileFile("data/script/battle/battle_vs_player.nut", this);
		break;
	}
	if (::replay.GetState() == ::replay.PLAY) {
		if (::network.IsActive())::manbow.CompileFile("data/script/battle/battle_watch.nut", this);
		else if (param.game_mode == 10)::manbow.CompileFile("data/script/battle/battle_replay_story.nut", this);
		else ::manbow.CompileFile("data/script/battle/battle_replay.nut", this);
	}

	if (::network.IsPlaying())::manbow.CompileFile("data/script/battle/battle_network.nut", this);
}

function _SetupWorld(param) {
	srand(param.seed);
	::graphics.ShowActor(true);
	::graphics.ShowBackground(true);
	::camera.Initialize();

	if ("SetCamera" in ::stage.background) {
		::stage.background.SetCamera();
	}

	world = ::manbow.World2D();
	world.Init(-1000, -1000, -100, 3560, 2440, 100);
}

function _SetupTeams(param) {
	group_player = ::manbow.Actor2DGroup();
	group_player.SetWorld(world);
	group_player.SetCamera(::camera.camera2d);
	group_player.SetOnMoveCallbackFunction(this, OnMove);
	group_effect = ::manbow.Actor2DGroup();
	::actor.SetGroup(group_player, group_effect);
	CreateTeam(0, param.team[0]);
	CreateTeam(1, param.team[1]);
	InitializeUser();
	::camera.Reset();
 	gauge.Initialize();
}

function _SetupProfilePictures() {
	if (::network.IsActive() && !::setting.network.hide_profile_pictures) {
		for( local i = 0; i < 2; i = ++i ) {
			local custom_icon = ::manbow.Texture();
			if (::network.icon[i] != null && custom_icon.CreateFromBase64(::network.icon[i], 32, 32)) {
				local icon = ::manbow.Sprite();
				icon.Initialize(custom_icon, 0, 0, 32, 32);
				icon.x = i == 0 ? 116 : 1280 - 116 - 32;
				icon.y = 4;
				icon.ConnectRenderSlot(::graphics.slot.status, 3000);
				gauge.AddParts(icon, i == 0 ? gauge.mat_left_top : gauge.mat_right_top);
			}
		}
	}
}

function _SetupInputs(param) {
	if (::network.IsActive())return;
	local devices = [team[0].input,team[1].input];
	local offset = 0;
	if (param.game_mode == 10) {
		devices.append(::input_talk);
		offset = ::story.stage * 3;
	}
	::replay.SetDevice(devices,offset);
}

// [M3] Wrap a plugin instance in a bracketing PROXY table. Squirrel class
// instances don't allow reassigning a method slot, so the engine's lifecycle
// calls go through this table: each runs inside the render_arena plugin scope
// (so the plugin's render objects -- glyph/VB -- AND its Squirrel churn land
// off-snapshot, never rewound by a rollback) then delegates to the real
// instance. `r` is a PER-CALL parameter (avoids the classic loop-capture trap),
// and `r.Method()` is a normal method call so `this == r` inside (a plain
// bindenv on the extracted method did NOT bind member access correctly).
// Squirrel has no `finally`, so leave() on BOTH the normal and throw paths keeps
// the nestable scope balanced even if a plugin throws.
function _BracketModifier(r) {
	local p = {};
	p.Update    <- function() { ::__render_unsnap_enter(); try { r.Update(); }    catch(e){ ::__render_unsnap_leave(); throw e; } ::__render_unsnap_leave(); };
	p.Begin     <- function() { ::__render_unsnap_enter(); try { r.Begin(); }     catch(e){ ::__render_unsnap_leave(); throw e; } ::__render_unsnap_leave(); };
	p.PreFrame  <- function() { ::__render_unsnap_enter(); local ok=true; try { ok = r.PreFrame(); } catch(e){ ::__render_unsnap_leave(); throw e; } ::__render_unsnap_leave(); return ok; };
	p.PostFrame <- function() { ::__render_unsnap_enter(); try { r.PostFrame(); } catch(e){ ::__render_unsnap_leave(); throw e; } ::__render_unsnap_leave(); };
	p.Release   <- function() { ::__render_unsnap_enter(); try { r.Release(); }   catch(e){ ::__render_unsnap_leave(); throw e; } ::__render_unsnap_leave(); };
	return p;
}

function _SetupModifiers(param) {
	// [plugin test rig] SQUIROLL_PLUGIN_FORCE="frame_bar,oki_dummy" forces the
	// listed plugins ON regardless of their Enabled() (e.g. a practice-only
	// plugin under a netplay rollback match). Per-instance via the launch env.
	local force = {};
	try {
		local fe = ::__squiroll_env("SQUIROLL_PLUGIN_FORCE");
		if (fe != null) foreach (n in ::split(fe, ",")) force[::strip(n)] <- true;
	} catch (e) {}
	foreach(name,modifier in modifiers) {
		::print(::format("[mod] trying %s\n",name));
		local on = (name in force) || modifier.enabled.call(this,param);
		if (name in force) ::print(::format("[mod] %s FORCED on (SQUIROLL_PLUGIN_FORCE)\n",name));
		if (!on) { ::print(::format("[mod] %s disabled\n",name)); continue; }
		::print(::format("[mod] %s constructing\n",name));
		if (modifier.async) {
			// [M3] ASYNC plugins (e.g. ping_display) dispatch from loop.task_async
			// -- forward-only, NOT re-run by a rollback re-sim -- so their entire
			// lifecycle can safely live off-snapshot: bracket the constructor AND
			// every engine call (via the proxy) in the render_arena plugin scope,
			// so their render objects (glyph/VB) + Squirrel churn are never rewound
			// (fixes the #28 dual-only crash without breaking determinism).
			::__render_unsnap_enter();
			local real = modifier.base_class();
			::__render_unsnap_leave();
			if (!real) { ::print(::format("[mod] %s null task\n",name)); continue; }
			modifier.task = _BracketModifier(real);
			::print(::format("Activating %s...\n",name));
			::loop.AddTask(modifier.task);
		} else {
			// SYNC plugins run INSIDE the re-simmed sim path (UpdateMain). Bracketing
			// them would route deterministic sim-path allocations off-snapshot and
			// break rollback (endless re-sim / stall). Dispatch them unchanged; a
			// finer sync-plugin API (bracket only their render-object creation) is
			// future work -- see docs/STRUCTURAL_ROLLBACK_PLAN.md §12.
			modifier.task = modifier.base_class();
			if (!modifier.task) { ::print(::format("[mod] %s null task\n",name)); continue; }
			::print(::format("Activating %s...\n",name));
			AddTask(modifier.task);
		}
		// (Daze 67c4433) new plugin system tracks the live modifier task by name.
		::plugin.active_modifiers[name] <- modifier.task;
	}
}

function Create( param ) {
	::print("[bC] _HideNames\n");          _HideNames();
	::print("[bC] _SetRPC\n");             _SetRPC(param);
	::print("[bC] _SetCoreFunctions\n");   _SetCoreFunctions();
	::print("[bC] _SetupBattleFuncs\n");   _SetupBattleFuncs(param);
	::print("[bC] _SetupWorld\n");         _SetupWorld(param);
	::print("[bC] _SetupTeams\n");         _SetupTeams(param);
	::print("[bC] _SetupProfilePictures\n"); _SetupProfilePictures();
	::print("[bC] _SetupInputs\n");        _SetupInputs(param);
	::print("[bC] _SetupModifiers\n");     _SetupModifiers(param);
	::print("[bC] done\n");
	//::rollback.start();
}

function _ClearRPC() {
	::discord.rpc_set_small_img_key("");
	::discord.rpc_set_small_img_text("");
	::discord.rpc_set_large_img_key("mainicon");
	if (::replay.GetState() == ::replay.PLAY && ::network.inst == null)
		::discord.rpc_commit_details_and_state("Idle", "");

}

function _ClearModifiers() {
    ::plugin.active_modifiers = {};
	foreach(plugin in modifiers) {
		if (!plugin.task)continue;
		::loop.DeleteTask(plugin.task);
		DeleteTask(plugin.task);
		plugin.task = null;
	}
}

function _ClearBattle() {
	task = {};
	gauge.Terminate();
	TerminateUser();
	::actor.SetGroup(null, null);
	::camera.Clear();
	battleUpdate = null;
	group_player = null;
	group_effect = null;

	if ("bgm" in this)bgm = null;
	foreach( v in team )v.Release();
	team = [null,null];
	::talk.Clear();
	::effect.Clear();
	::manbow.SetTerminateFunction(null);
}

function Release() {
	_ClearRPC();
	//::rollback.stop();
	_ClearModifiers();
	_ClearBattle();
	::overlay.clear();
	::hud.clear();   // [#30] drop the native HUD queue so it doesn't linger post-match
}

function Begin(){
	foreach(modifier in modifiers)if(modifier.task)modifier.task.Begin();
}

function End() {
	::sound.StopBGM(500);
	::loop.EndWithFade();
}

function AddTask( actor ) {
	task[actor.tostring()] <- actor;
}

function DeleteTask( actor ) {
	if (actor.tostring() in task)
	{
		delete task[actor.tostring()];
	}
}

function SetTimeStop( n ){time_stop_count = n;}
function SetSlow( n ){slow_count = n;}

local updatemain = UpdateMain;
function UpdateMain() {
	foreach(modifier in modifiers) {
		if(modifier.task){
			if(!modifier.task.PreFrame())return;
		}
	}
	updatemain();
	foreach(modifier in modifiers)if(modifier.task)modifier.task.PostFrame();
	if (::menu.pause_hack)
		::menu.pause_hack = false;
	else if (::loop.pause_count == 0 && !is_time_stop && !::network.IsPlaying())
		::overlay.set_hitboxes(group_player, team[0].current.hit_state, team[1].current.hit_state);

}

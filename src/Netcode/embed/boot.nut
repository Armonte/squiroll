// Defensive guard around ::loop.End: with skip_to_battle / auto_connect,
// the loop stack isn't the vanilla [boot, title, network, character_select,
// vs] chain — many pause-menu and post-match actions call End(somescene)
// expecting that scene to be in the stack. Make End a no-op when the
// scene isn't present or the stack is already empty so we don't blow up
// on "top() on an empty array".
function InstallLoopEndGuard() {
    if ("_loop_end_guard_installed" in ::loop) return;
    ::loop._loop_end_guard_installed <- true;
    local _orig_loop_End = ::loop.End;
    ::loop.End = function (scene = null) {
        if (this.env_stack.len() == 0) return;
        if (scene) {
            local found = false;
            foreach (s in this.env_stack) {
                if (s == scene) { found = true; break; }
            }
            if (!found) return;
        }
        _orig_loop_End.call(this, scene);
    };
}

// Auto-connect uses AcceptMatch / BeginMatch which call menu.network.Suspend,
// which in turn does ::menu.title.Hide / EndAnime / etc. Since we skip the
// menu chain, those locals aren't safe to touch. Replace Suspend with a
// minimal stub that just flags is_suspend.
function StubMenuNetworkSuspend() {
    if (!("network" in ::menu)) return;
    ::menu.network.Suspend = function () {
        is_suspend = true;
    };
}

// AcceptMatch (server) and BeginMatch (client) both end their fade callback
// with `::menu.character_select.Initialize(1)`. For the local two-client
// test we skip CSS entirely. If gekko is enabled we DEFER vs.Initialize
// until both peers have completed the GekkoSessionStarted handshake — the
// engine's first battle frame must run under gekko ownership on both
// peers simultaneously, otherwise their frame counters diverge and gekko's
// DESYNC detector fires forever. While waiting, the loop runs vanilla
// menu Update (boot scene's state machine).
function OverrideCSSForAutoConnect() {
    ::menu.character_select.Initialize = function (game_mode = 1, difficulty = 0) {
        local param = ::vs.InitializeParam();
        param.game_mode = 1;
        param.difficulty = 0;

        // Host is player slot 0 (master in vanilla parlance), client is
        // slot 1. is_client is set by BeginMatch on the client side; the
        // host never calls BeginMatch so its is_client stays false.
        local our_slot = ::network.is_client ? 1 : 0;
        param.device_id[our_slot]   = ::network.local_device_id;
        param.device_id[1 - our_slot] = -1;

        param.mode[0] = 0;
        param.mode[1] = 0;
        param.master_name[0]  = "reimu";
        param.slave_name[0]   = "marisa";
        param.master_name[1]  = "reimu";
        param.slave_name[1]   = "marisa";
        param.master_color[0] = 0;
        param.slave_color[0]  = 0;
        param.master_color[1] = 1;
        param.slave_color[1]  = 1;
        param.spell[0] = 0;
        param.spell[1] = 0;
        param.background_id = 26;
        param.bgm_id = 1;
        param.seed = ::network.rand_seed;

        ::print("[squiroll boot] CSS override our_slot=" + our_slot
                + " device=" + ::network.local_device_id
                + " seed=" + param.seed
                + " gekko_enabled=" + ::setting.network.gekko_enabled + "\n");

        if (!::setting.network.gekko_enabled) {
            // Delay-based fallback: original behavior — vs.Initialize
            // immediately.
            ::print("[squiroll boot] gekko_enabled=false, vs.Initialize now\n");
            ::vs.Initialize(param);
            return;
        }

        // Gekko path: run vs.Initialize NOW so the round-start intro
        // plays out under the vanilla loop. Both peers run the intro
        // locally — it is deterministic from the shared rand_seed, so
        // they reach Round_Fight with identical battle state. The
        // GekkoGameSession is created at Round_Fight by pre_arm_poll;
        // the game then holds at fight-frame-0 until the handshake
        // completes (SessionStarted). gekko never rolls back the intro.
        local gk_port_base = ::setting.network.peer_port;
        local local_port  = ::network.is_client ? gk_port_base + 11 : gk_port_base + 10;
        local remote_port = ::network.is_client ? gk_port_base + 10 : gk_port_base + 11;

        ::print("[squiroll boot] dual: vs.Initialize (intro runs vanilla); "
                + "watch_for_fight_dual local=" + local_port
                + " remote=" + remote_port + " idx=" + our_slot
                + " ip=" + ::setting.network.peer_ip + "\n");
        ::vs.Initialize(param);
        ::setting.network.gekko_watch_for_fight_dual(local_port, remote_port,
                                                     our_slot,
                                                     ::setting.network.peer_ip);
    };
}

function Initialize() {
    ::print("[squiroll boot] Initialize entered\n");
    ::plugin <- {};
    ::manbow.CompileFile("squiroll/plugin/core/manager.nut",::plugin);
    // Squirrel-side state serializer for GekkoNet save/load. Wires
    // ::__gekko_state.{save_battle,load_battle,ser,deser,load_into}.
    ::manbow.CompileFile("squiroll/gekko_state.nut", this.getroottable());

    local skip_intro = ::setting.misc.skip_intro;
    local skip_to_battle = false;
    if ("skip_to_battle" in ::setting.misc) {
        skip_to_battle = ::setting.misc.skip_to_battle;
    }
    local auto_connect = "";
    if ("network" in ::setting && "auto_connect" in ::setting.network) {
        auto_connect = ::setting.network.auto_connect;
    }
    ::print("[squiroll boot] skip_intro=" + skip_intro
            + " skip_to_battle=" + skip_to_battle
            + " auto_connect=" + auto_connect + "\n");

    if (skip_intro) {
        ::graphics.FadeIn(15);
        ::manbow.CompileFile("data/script/initialize.nut", this.getroottable());
        ::actor.Initialize();

        if (auto_connect == "host" || auto_connect == "client") {
            this.stage <- "init_net";
            this.tick_count <- 0;
            this.peer_ip <- ::setting.network.peer_ip;
            this.peer_port <- ::setting.network.peer_port;
            this.role <- auto_connect;
            ::print("[squiroll boot] auto_connect: role=" + this.role
                    + " peer=" + this.peer_ip + ":" + this.peer_port
                    + " device=" + ::setting.network.device_id + "\n");

            // Per-instance local device assignment. Two windows sharing a
            // single controller see it twice unless we point them at
            // different devices (0=first / 1=second / etc).
            ::network.local_device_id = ::setting.network.device_id;

            InstallLoopEndGuard();
            StubMenuNetworkSuspend();
            OverrideCSSForAutoConnect();
            // Title needs to exist for any stray .Hide() calls in
            // canonical network paths; initialize+suspend it so it's a
            // valid (but invisible) scene below us.
            ::menu.title.Initialize();
            ::menu.title.Suspend();

            ::loop.Begin(this);
            ::print("[squiroll boot] state machine running\n");
            return;
        }

        if (auto_connect == "solo") {
            // Single-process gekko stress session. Boots straight into a
            // VS match (same param as skip_to_battle) but hands the frame
            // loop to a GekkoStressSession — one instance, full rollback
            // save/load path exercised. No networking, no second client.
            ::print("[squiroll boot] entering SOLO gekko stress session\n");
            local param = ::vs.InitializeParam();
            param.game_mode      = 1;
            param.difficulty     = 0;
            param.device_id[0]   = -1;
            param.device_id[1]   = -1;
            param.mode[0]        = 0;
            param.mode[1]        = 0;
            param.master_name[0] = "reimu";
            param.slave_name[0]  = "marisa";
            param.master_name[1] = "reimu";
            param.slave_name[1]  = "marisa";
            param.master_color[0] = 0;
            param.slave_color[0]  = 0;
            param.master_color[1] = 1;
            param.slave_color[1]  = 1;
            param.spell[0]       = 0;
            param.spell[1]       = 0;
            param.background_id  = 26;
            param.bgm_id         = 1;
            param.seed           = ::manbow.timeGetTime();

            ::menu.title.Initialize();
            ::menu.title.Suspend();
            ::menu.character_select.Initialize(param.game_mode, param.difficulty);
            ::menu.character_select.Suspend();
            InstallLoopEndGuard();

            if (!::setting.network.gekko_enabled) {
                ::print("[squiroll boot] solo: gekko_enabled=false, plain vs.Initialize\n");
                ::vs.Initialize(param);
                return;
            }

            // Run vs.Initialize NOW so the round-start intro plays out
            // under the vanilla loop. gekko_watch_for_fight_solo() then
            // arms the stress session the instant the intro ends and
            // Round_Fight begins — gekko frame 0 = fight frame 0, and
            // the rollback never touches the intro's demoCount machine.
            ::print("[squiroll boot] solo: vs.Initialize (intro runs vanilla)\n");
            ::vs.Initialize(param);
            ::setting.network.gekko_watch_for_fight_solo();
            ::print("[squiroll boot] solo: watching for Round_Fight to arm gekko\n");
            return;
        }

        if (skip_to_battle) {
            ::print("[squiroll boot] entering SkipToBattle (solo)\n");
            local param = ::vs.InitializeParam();
            param.game_mode      = 1;
            param.difficulty     = 0;
            param.device_id[0]   = -1;
            param.device_id[1]   = -1;
            param.mode[0]        = 0;
            param.mode[1]        = 0;
            param.master_name[0] = "reimu";
            param.slave_name[0]  = "marisa";
            param.master_name[1] = "reimu";
            param.slave_name[1]  = "marisa";
            param.master_color[0] = 0;
            param.slave_color[0]  = 0;
            param.master_color[1] = 1;
            param.slave_color[1]  = 1;
            param.spell[0]       = 0;
            param.spell[1]       = 0;
            param.background_id  = 26;
            param.bgm_id         = 1;
            param.seed           = ::manbow.timeGetTime();

            ::print("[squiroll boot] priming title (suspended)\n");
            ::menu.title.Initialize();
            ::menu.title.Suspend();
            ::print("[squiroll boot] priming character_select (suspended)\n");
            ::menu.character_select.Initialize(param.game_mode, param.difficulty);
            ::menu.character_select.Suspend();
            InstallLoopEndGuard();

            ::print("[squiroll boot] calling vs.Initialize\n");
            ::vs.Initialize(param);
            ::print("[squiroll boot] vs.Initialize returned\n");
        } else {
            ::menu.title.Initialize();
        }
    } else {
        local texture = ::manbow.Texture();
        texture.Load("data/system/boot/boot.png");
        this.sprite <- ::manbow.Sprite();
        this.sprite.Initialize(texture, 0, 0, texture.width, texture.height);
        this.sprite.ConnectRenderSlot(::graphics.slot.front, 0);
        this.count <- 0;
        ::graphics.FadeIn(15);
        ::loop.Begin(this);
    }
}

function Terminate()
{
    this.sprite = null;
}

function Update()
{
    // Auto-connect state machine path.
    if ("stage" in this) {
        this.tick_count++;

        switch (this.stage) {
            case "init_net":
                if (this.role == "host") {
                    ::print("[squiroll boot] StartupServer port=" + this.peer_port + "\n");
                    local ok = ::network.StartupServer(this.peer_port, 0);
                    ::print("[squiroll boot] StartupServer returned " + ok + "\n");
                    this.stage = "wait_host";
                } else {
                    ::print("[squiroll boot] StartupClient "
                            + this.peer_ip + ":" + this.peer_port + "\n");
                    local ok = ::network.StartupClient(
                        this.peer_ip, this.peer_port, 0);
                    ::print("[squiroll boot] StartupClient returned " + ok + "\n");
                    this.stage = "wait_client";
                }
                break;

            case "wait_host":
                if (::network.received_request != null) {
                    ::print("[squiroll boot] received_request from peer, AcceptMatch\n");
                    this.stage = "in_css";
                    ::network.AcceptMatch();
                }
                if (this.tick_count % 60 == 0) {
                    ::print("[squiroll boot] wait_host tick=" + this.tick_count + "\n");
                }
                break;

            case "wait_client":
                if (::network.inst != null && ::network.ready) {
                    ::print("[squiroll boot] client connected + ready (BeginMatch fired)\n");
                    this.stage = "in_css";
                }
                if (this.tick_count % 60 == 0) {
                    ::print("[squiroll boot] wait_client tick=" + this.tick_count
                            + " inst=" + (::network.inst != null)
                            + " ready=" + ::network.ready + "\n");
                }
                break;

            case "in_css":
                // CSS scene is on top of the loop; our Update no longer fires.
                break;
        }
        return;
    }

    // Vanilla boot path (used when skip_intro is false).
    this.count++;

    if (this.count == 15)
    {
        local begin = ::manbow.timeGetTime();
        ::manbow.CompileFile("data/script/initialize.nut", this.getroottable());
        ::actor.Initialize();

        while (::manbow.timeGetTime() - begin < 2000)
        {
            this.Sleep(1);
        }

        ::loop.Fade(function ()
        {
            ::loop.End();
            ::menu.title.Initialize();
        });
    }
}

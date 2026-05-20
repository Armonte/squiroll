function Initialize() {
    ::print("[squiroll boot] Initialize entered\n");
    ::plugin <- {};
    ::manbow.CompileFile("squiroll/plugin/core/manager.nut",::plugin);

    local skip_intro = ::setting.misc.skip_intro;
    local skip_to_battle = false;
    if ("skip_to_battle" in ::setting.misc) {
        skip_to_battle = ::setting.misc.skip_to_battle;
    }
    ::print("[squiroll boot] skip_intro=" + skip_intro + " skip_to_battle=" + skip_to_battle + "\n");

    if (skip_intro) {
        ::graphics.FadeIn(15);
        ::manbow.CompileFile("data/script/initialize.nut", this.getroottable());
        ::actor.Initialize();
        if (skip_to_battle) {
            ::print("[squiroll boot] entering SkipToBattle\n");
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
            // Prime title and character_select on the loop stack so the
            // pause menu's "return to title" and "return to character
            // select" options find their targets. Each scene is
            // Initialize()d (pushed) then Suspend()d (hidden but still
            // on the stack), matching the order the normal menu flow
            // produces: [title, character_select, vs(, pause)].
            ::print("[squiroll boot] priming title (suspended)\n");
            ::menu.title.Initialize();
            ::menu.title.Suspend();
            ::print("[squiroll boot] priming character_select (suspended)\n");
            ::menu.character_select.Initialize(param.game_mode, param.difficulty);
            ::menu.character_select.Suspend();

            // Defensive guard around ::loop.End: in vanilla flow there's
            // always a boot scene underneath title, and CSS sits between
            // title and battle. With skip_to_battle the stack is just
            // [title, vs(, pause)], so pause-menu options like "return to
            // character select" (which calls End(menu.character_select))
            // pop everything looking for a scene that's not there and
            // crash with "top() on a empty array". Make End a no-op when
            // the requested scene isn't on the stack or the stack is
            // already empty.
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

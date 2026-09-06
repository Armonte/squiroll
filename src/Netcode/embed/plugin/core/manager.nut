::manbow.CompileFile("squiroll/plugin/core/cfg.nut",this);
Input <- {};
::manbow.CompileFile("squiroll/plugin/core/input.nut",Input);
cfg <- {};
list <- {};
patches <- {};
patches_csv <- {};
active_modifiers <- {};

class Modifier {
	// [rollback] marker: the native cross-peer checksum (gekko_bridge
	// raw_ser_instance) emits any instance whose class carries this member as
	// EMPTY, so plugin state -- which may legitimately differ per peer (a
	// cosmetic plugin enabled on one side only) -- never enters the checksum.
	__squiroll_plugin = true;
	task = null;
	enabled = null;
	async = null;
	base_class = null;
	constructor(_base) {
		base_class = _base;
		async = _base.async;
		enabled = _base.Enabled;
	}
}

// [rollback] Declare a per-object field a plugin INJECTS into sim objects
// (e.g. a member added to the player actor class) as checksum-exempt. Such a
// field is plugin-owned, cosmetic and may differ per peer (plugin on one side
// only) -- it must never enter the cross-peer checksum or it desyncs the match.
// Call once at load time, before any battle: ::plugin.ExemptKey("_my_field").
exempt_keys <- {};
function ExemptKey(name) {
	exempt_keys[name] <- true;
	::print("[plugin] exempt key queued: " + name + " (native=" + ("__gekko_skip_key" in ::getroottable()) + ")\n");
	// The native side (gekko_bridge) may register ::__gekko_skip_key AFTER the
	// plugins load; it drains ::plugin.exempt_keys at registration time, so a
	// missing native here is fine.
	if ("__gekko_skip_key" in ::getroottable()) ::__gekko_skip_key(name);
}

function LoadCFG(label,Default) {
	cfg[label] <- CFG(label+".ini",Default);
    return cfg[label];
}

function Patch(file,patch) {
	if (file in patches) {
		local prev = patches[file];
		local new = function() {
			prev();
			patch();
		}
		patches[file] = new;
	}else patches[file] <- patch;
}

function PatchCSV(csv,patch) {
	if (csv in patches_csv) {
		local prev = patches_csv[csv];
		local new = function(table) {
			prev(table);
			patch(table);
		};
		patches_csv[csv] = new;
	}else patches_csv[csv] <- patch;
}

function AddModifier(base_class,label) {
    Patch("data/script/battle/battle.nut",function() {
		modifiers[label] <- ::plugin.Modifier(base_class);
	});
}

function NewPlugin(label) {
    local plugin = list[label] <- {
        config = {}
        modifier = class {
            __squiroll_plugin = true;   // [rollback] checksum-exempt, see Modifier
            async = false;
            function Enabled(param){return false};
            function PreFrame(){return true};
            function Begin(){};
            function Update(){};
            function Release(){};
            function PostFrame(){};
        }
    };
    return plugin;
}

function LoadNativePlugin(path,label) {
    local table = NewPlugin(label);	
    ::manbow.CompileFile(path,table);
    LoadCFG(label,table.config);
    AddModifier(table.modifier,label);
}

function LoadPlugin(path,label) {
    local table = NewPlugin(label);	
    ::loadfile("plugin/"+path,true).call(table);
    LoadCFG(label,table.config);
    AddModifier(table.modifier,label);
}

function CreatePage(plugin) {
    Patch("squiroll/config/mod_config.nut",function() {
        local config = ::plugin.cfg[plugin].data;
        local elems = [::UI.Menu.Page(::UI.Menu.Title(plugin))];
        local _page = elems.top();
        local i = 0;
        local function addElem(elem) {
            _page.item.push(elem);
            i = (i + 1) % 12;
            if (!i) {
                elems.push(::UI.Menu.Page(::UI.Menu.Title(plugin)));
                _page = elems.top();
            }
        };
        foreach (k,v in config) {
            addElem(::UI.Menu.Header(i,k));
            if (k.find("bind_")) {
                local device = k.slice(4);
                foreach (ke,va in v) {
                    addElem(::UI.Menu.Config.Keybind(i,ke,plugin,device,ke,ke,this));
                }
            }else {
                foreach (ke,va in v) {
                    switch (typeof va) {
                        case "bool":
                            addElem(::UI.Menu.Config.Boolean(i,ke,plugin,k,ke,this));
                            break;
                        default:
                            addElem(::UI.Menu.Config.Value(i,ke,plugin,k,ke,this));
                            break;
                    }
                }
            }
        }
        page.extend(elems);
    });
}

Patch("data/system/component/menu_common.nut",function() {
	local prev = LoadItemTextArray;
	function LoadItemTextArray(filename) {
		local table = prev(filename);
		if (filename in ::plugin.patches_csv) {
			local patch = ::plugin.patches_csv[filename];
			patch(table);
		};
		return table;
	}

    function LoadItemTextArrayA(filename) {
        local item = [];
        try item.extend(::manbow.LoadCSV(filename))
        catch(e);
        local item_table = {
            lang0 = {}//jp
            lang1 = {}//en
        };
        foreach (v in item) {
            local label = v[0];
            item_table.lang0[label] <- [];
            item_table.lang1[label] <- [];
            for (local i = 1; i < v.len(); ++i) {
                if (v[i].len() == 0)break;
                local lang = (i + 1) % 2;
                item_table["lang"+lang][label].push(v[i]);
            }
        }
        if (filename in ::plugin.patches_csv) {
			local patch = ::plugin.patches_csv[filename];
			patch(item_table);
		};
        return item_table;
    }
});

// Built-in plugins, feel free to comment out if not wanted.
LoadNativePlugin("squiroll/plugin/frame_data.nut","frame_data");
LoadNativePlugin("squiroll/plugin/frame_bar.nut","frame_bar");
LoadNativePlugin("squiroll/plugin/input_display.nut","input_display");
LoadNativePlugin("squiroll/plugin/framerate_control.nut","framerate_control");
LoadNativePlugin("squiroll/plugin/oki_dummy.nut","oki_dummy");
LoadNativePlugin("squiroll/plugin/ping_display.nut","ping_display");

::mkdir("plugin");
::mkdir("plugin/config");
foreach(file in ::listfiles("plugin")) {
	if (!file.find(".nut"))continue;
	local label = ::strip(file.slice(0,file.len()-4));
	LoadPlugin(file,label);
}

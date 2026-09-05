class CFG {
	filepath = null;
	data = null;

	constructor (path,cfg) {
		filepath = "plugin/config/"+path;
		data = cfg;
		Read();
		Write();
		ApplyEnvOverrides();   // AFTER Write(): launch overrides never persist to the .ini
	}

	// [rollback test rig] Per-INSTANCE override: SQUIROLL_PLUGIN_CFG =
	// "label:section.key=value;label:key=value;..." applied on top of the shared
	// .ini, so two instances launched from the same folder can run different
	// plugin configs (e.g. a cosmetic plugin on one peer only). Read via the
	// native ::__squiroll_env (plugin.cpp). Silently a no-op when unset.
	function ApplyEnvOverrides() {
		local env = null;
		try { env = ::__squiroll_env("SQUIROLL_PLUGIN_CFG"); } catch (e) { return; }
		if (env == null || env.len() == 0) return;
		local label = filepath.slice("plugin/config/".len());
		if (label.len() > 4 && label.slice(label.len()-4) == ".ini") label = label.slice(0, label.len()-4);
		foreach (entry in ::split(env, ";")) {
			entry = ::strip(entry);
			local colon = entry.find(":");
			if (colon == null) continue;
			if (::strip(entry.slice(0, colon)) != label) continue;
			local rest = entry.slice(colon + 1);
			local eq = rest.find("=");
			if (eq == null) continue;
			local path = ::strip(rest.slice(0, eq));
			local str = ::strip(rest.slice(eq + 1));
			try {
				local dot = path.find(".");
				local table = data;
				local key = path;
				if (dot != null) {
					table = data[::strip(path.slice(0, dot))];
					key = ::strip(path.slice(dot + 1));
				}
				table[key] = tovalue(str, typeof table[key]);
				::print(::format("[plugin cfg] env override %s:%s=%s\n", label, path, str));
			} catch (e) {
				::print(::format("[plugin cfg] env override error %s:%s -> %s\n", label, path, e));
			}
		}
	}

    function tovalue(str,type) {
        switch(type) {
            case "float":
            case "integer":
            case "string":
                return str["to"+type]();
            case "bool":
                return str.tolower() == "true";
        }
    }

	function Read() {
		local content = "";
        try{content = ::readfile(filepath);}catch(e){return};
		local lines = ::split(content,"\n");
		local table = data;
		foreach (line in lines) {
			try {
              line = ::strip(line);
			  if (line.len() == 0 || line[0] == ';' || line[0] == "#") continue;

			  if (line[0] == '[' && line[line.len() - 1] == ']') {
			  	table = data[(::strip(line.slice(1,line.len()-1)))];
			  	continue;
			  }

			  local eq = line.find("=");
			  if (!eq)continue;
			  local key = ::strip(line.slice(0,eq));
			  local str = ::strip(line.slice(eq+1));
			  local type = typeof table[key];
              table[key] = tovalue(str,type);
            }catch(e) {
                ::print(::format("Config Read Error @%s:%s\n->%s\n",filepath,line,e));
            }
		}
	}

	function Write() {
		local buffer = "";
	
		foreach (k,v in data) {
			if (typeof v == "table") {
				buffer += ::format("[%s]\n",k);
				foreach (k,v in v) {
					switch (typeof v) {
						case "float":
						case "integer":
						case "bool":
						case "string":
							buffer += ::format("%s=%s\n",k,v.tostring());
							break;
					}
				}
				continue;
			}
			buffer += ::format("%s=%s\n",k,v.tostring());
		}

		::writefile(filepath,buffer);
	}

	function Set(val,key,sec) {
		try {
            local new = val;
            if (typeof new != typeof data[sec][key])
                new = val["to"+typeof data[sec][key]]();
            data[sec][key] = new;
		    Write();
        }catch(e) {
            ::print("error setting config:"+e+"\n");
        }
	}
};

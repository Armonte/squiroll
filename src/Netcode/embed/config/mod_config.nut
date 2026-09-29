local item_table = {
    lang0 = {
	hitbox = ["View hitboxes(jp)","Disabled(jp)","Enabled(jp)"]
	discord = ["Use Discord**(jp)","Disabled(jp)","Enabled(jp)"]
	rollback = ["Rollback netcode(jp)","Disabled(jp)","Enabled(jp)"]
    }
    lang1 = {
        hitbox = ["View hitboxes","Disabled","Enabled"]
        discord = ["Use Discord**","Disabled","Enabled"]
        rollback = ["Rollback netcode","Disabled","Enabled"]
    }
};

local ptr = {
	hitbox = ::UI.Config.SquirollPTR(::overlay,"enabled","hitbox_vis","enabled")
	discord = ::UI.Config.SquirollPTR(::discord,"enabled","misc","discord_integration")
	// [#45] Netcode preference: ON = GekkoNet rollback, OFF = vanilla delay.
	// Writes ::setting.network.gekko_enabled (read by the #44 handshake
	// negotiation) and persists to config [network] gekko_enabled. The actual
	// per-match netcode is the symmetric AND of both peers' preference (#44),
	// so a rollback-preferring player still gets delay vs a delay-only opponent.
	rollback = ::UI.Config.SquirollPTR(::setting.network,"gekko_enabled","network","gekko_enabled")
};

::UI.Menu.Create.call(this,
    ::UI.Menu.Page(
        ::UI.Menu.Struct.Title("Squiroll"),
        ::UI.Menu.Enum.Boolean(item_table,"hitbox",ptr.hitbox),
        ::UI.Menu.Enum.Boolean(item_table,"discord",ptr.discord),
        ::UI.Menu.Enum.Boolean(item_table,"rollback",ptr.rollback)
    )
);

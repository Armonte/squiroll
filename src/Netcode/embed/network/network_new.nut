local item_table = ::menu.common.LoadItemTexArrayA("data/system/network/item.csv");
// [merge 67c4433] network_new is a dormant WIP STUB at this base — the active
// online menu is still network.nut here; the full page list lands in Daze's
// 2e0b321 menus-rewrite. The original `Create.call(this,\n)` had a trailing-comma
// / empty-arg syntax error (sqcheck rejects it; Daze's build skips sqcheck).
// Neutralized until the 2e0b321 increment is merged, so the tree compiles.
// ::UI.Menu.Create.call(this);

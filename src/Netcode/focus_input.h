#pragma once

namespace focus_input {
    // Install XInputGetState IAT hook that gates controller reads by
    // foreground-window check. Used to share one controller between
    // two squiroll instances during local rollback testing.
    void install();
}

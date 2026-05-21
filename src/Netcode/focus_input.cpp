// Focus-gated controller input for dual-instance local testing.
//
// Two squiroll processes share one physical controller (XInput device).
// XInput has no cooperative-level / foreground concept, so both processes
// read it simultaneously and one button press drives BOTH players.
//
// Fix: hook the XInputGetState IAT entry. When the calling process is
// not the foreground window, zero out the returned XINPUT_STATE so the
// game sees no input. Whichever instance has focus is the one whose
// controller reads count. Switch focus to swap which player is driven.
//
// DInput (keyboard) is already set up with DISCL_FOREGROUND by the
// game's PlayerInput::Initialize (0x43B680), so keyboard input is
// automatically gated. Only XInput needs the hook.

#include <windows.h>
#include "patch_utils.h"
#include "log.h"
#include "focus_input.h"

// Inline-defined XINPUT_STATE so we don't drag in xinput.h / xinput.lib.
typedef struct _XINPUT_GAMEPAD_LITE {
    WORD  wButtons;
    BYTE  bLeftTrigger;
    BYTE  bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
} XINPUT_GAMEPAD_LITE;

typedef struct _XINPUT_STATE_LITE {
    DWORD               dwPacketNumber;
    XINPUT_GAMEPAD_LITE Gamepad;
} XINPUT_STATE_LITE;

typedef DWORD (WINAPI *XInputGetState_t)(DWORD dwUserIndex, XINPUT_STATE_LITE* pState);

// th155.exe stores the main HWND at IDA 0x8DAD0C (RVA 0x4DAD0C from
// ImageBase 0x400000), set in CreateGame after CreateWindowExA.
// The IAT entry for XInputGetState is at IDA 0x788520 → RVA 0x388520.
#define TH155_HWND_RVA      0x4DAD0C
#define XINPUT_GETSTATE_IAT 0x388520

static XInputGetState_t orig_XInputGetState = nullptr;

static HWND get_game_hwnd() {
    return *(HWND*)((uintptr_t)GetModuleHandleA(NULL) + TH155_HWND_RVA);
}

static DWORD WINAPI hooked_XInputGetState(DWORD dwUserIndex, XINPUT_STATE_LITE* pState) {
    HWND game_hwnd = get_game_hwnd();
    if (game_hwnd && GetForegroundWindow() != game_hwnd) {
        if (pState) {
            ZeroMemory(pState, sizeof(*pState));
        }
        return ERROR_SUCCESS;
    }
    if (!orig_XInputGetState) {
        if (pState) ZeroMemory(pState, sizeof(*pState));
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    return orig_XInputGetState(dwUserIndex, pState);
}

namespace focus_input {
    void install() {
        void** iat_slot = (void**)((uintptr_t)GetModuleHandleA(NULL) + XINPUT_GETSTATE_IAT);
        orig_XInputGetState = (XInputGetState_t)*iat_slot;
        hotpatch_import((void*)iat_slot, (void*)&hooked_XInputGetState);
        log_printf("[focus_input] XInputGetState IAT hooked: orig=%p, hooked=%p, hwnd_global=%p\n",
                   orig_XInputGetState,
                   (void*)&hooked_XInputGetState,
                   (void*)((uintptr_t)GetModuleHandleA(NULL) + TH155_HWND_RVA));
    }
}

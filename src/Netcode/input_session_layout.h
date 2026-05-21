#pragma once

#ifndef INPUT_SESSION_LAYOUT_H
#define INPUT_SESSION_LAYOUT_H 1

// Binary layouts for AoCF's per-frame network input session, extracted
// from netcode.cpp so gekko_bridge.cpp can dereference into them
// without re-declaring everything. Field offsets are baked from the
// retail binary — keep in sync with netcode.cpp's copies if either
// moves.

#include <stdint.h>
#include <vector>
#include <memory>
#include <atomic>
#include <functional>

struct BoostMutex {
    std::atomic<uint32_t> active_count; // 0x0
    void* event_handle;                  // 0x4
    // 0x8
};

struct TF4InputDeviceState {
    int32_t  x;                          // 0x0
    int32_t  y;                          // 0x4
    uint32_t buttons[12];                // 0x8
    char     __unk38[0x90 - 0x38];       // 0x38
    // 0x90
};
static_assert(sizeof(TF4InputDeviceState) == 0x90);

struct TF4InputDevice {
    void*               vtbl;            // 0x0
    TF4InputDeviceState state;           // 0x4
    char                __unk94[0x140 - 0x94]; // 0x94
    // 0x140
};
static_assert(sizeof(TF4InputDevice) == 0x140);

struct TF4InputRecorderDevice {
    size_t input_write_idx;              // 0x0
    size_t input_read_idx;               // 0x4
    bool   __bool_8;                     // 0x8
    uint8_t probably_padding_bytes9[0xC - 0x9]; // 0x9
    TF4InputDevice* tf4_device;          // 0xC
    std::vector<uint16_t> input_vec;     // 0x10
    BoostMutex* input_mutex;             // 0x1C
    // 0x20
};
static_assert(sizeof(TF4InputRecorderDevice) == 0x20);

struct ManbowInputRecorder {
    uintptr_t vtbl;                      // 0x0
    std::vector<uint8_t> compressed_header; // 0x4
    std::vector<std::shared_ptr<TF4InputRecorderDevice>> devices; // 0x10
    BoostMutex input_mutex;              // 0x1C
    uint8_t __unk24[0x4C - 0x24];        // 0x24
    // 0x4C
};
static_assert(sizeof(ManbowInputRecorder) == 0x4C);

struct ManbowNetworkInputSessionRemote {
    size_t session_num;                  // 0x0
    size_t remote_write_idx;             // 0x4
    size_t local_write_idx;              // 0x8
    uint8_t __unkC[4];                   // 0xC
    std::function<void(void*, uint32_t)> send_packet_func; // 0x10
    std::function<uint32_t()>            get_ping_func;    // 0x38
    std::function<void()>                disconnect_func;  // 0x60
    uint64_t last_input_packet_time;     // 0x88
    // 0x90
};
static_assert(sizeof(ManbowNetworkInputSessionRemote) == 0x90);

struct ManbowNetworkInputSession {
    int32_t session_num;                 // 0x0
    uint8_t local_player_idx;            // 0x4
    uint8_t player_count;                // 0x5
    uint8_t __unk6[0x10 - 6];            // 0x6
    TF4InputDevice* local_input;         // 0x10
    bool __bool_14;                      // 0x14
    uint8_t probably_padding_bytes15[0x18 - 0x15]; // 0x15
    std::vector<std::shared_ptr<void>> device_vec; // 0x18
    std::shared_ptr<ManbowInputRecorder> input_recorder; // 0x24
    std::vector<uint8_t> player_to_remote_idx; // 0x2C
    std::vector<ManbowNetworkInputSessionRemote> remote_vec; // 0x38
    std::vector<uint8_t> input_packet_buf;  // 0x44
    std::vector<uint8_t> input_packet_buf2; // 0x50
    uint8_t __unk5C[0x68 - 0x5C];        // 0x5C
    // 0x68
};
static_assert(sizeof(ManbowNetworkInputSession) == 0x68);

#endif // INPUT_SESSION_LAYOUT_H

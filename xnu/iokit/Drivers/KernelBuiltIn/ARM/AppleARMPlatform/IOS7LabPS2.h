/* Three-byte PS/2 packets from the unmodified RealView PL050 mouse. */
#ifndef IOS7LAB_PS2_H
#define IOS7LAB_PS2_H
#include <stdint.h>
struct IOS7LabPointerSample {
    uint32_t valid, sequence, x, y, buttons, packets, errors, reserved;
};
struct IOS7LabPS2State {
    uint8_t bytes[3];
    unsigned count;
    uint32_t sequence, x, y, buttons, packets, errors;
};
static bool ios7lab_ps2_byte(IOS7LabPS2State *state, uint8_t value,
                            uint32_t width, uint32_t height)
{
    if (!width || !height || width > 2048 || height > 2048) return false;
    if (!state->count && !(value & 8)) { ++state->errors; return false; }
    state->bytes[state->count++] = value;
    if (state->count < 3) return false;
    state->count = 0;
    unsigned flags = state->bytes[0];
    ++state->packets;
    if (flags & 0xc0) { ++state->errors; return false; }
    int dx = (int)state->bytes[1] - ((flags & 0x10) ? 256 : 0);
    int dy = (int)state->bytes[2] - ((flags & 0x20) ? 256 : 0);
    int x = (int)state->x + dx, y = (int)state->y - dy;
    state->x = x < 0 ? 0U : (x >= (int)width ? width - 1 : (uint32_t)x);
    state->y = y < 0 ? 0U : (y >= (int)height ? height - 1 : (uint32_t)y);
    state->buttons = flags & 7;
    if (!++state->sequence) ++state->sequence;
    return true;
}
#endif

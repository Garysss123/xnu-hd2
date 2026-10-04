/* Bounded ARM32 transport adapter for original iOS7 io_connect_method.
 * Inputs and outputs keep their native Mach port and scalar semantics.
 * Include after uint32_t, size_t and memcpy/memset declarations.
 */
#ifndef IOS7LAB_IOKIT_WIRE_H
#define IOS7LAB_IOKIT_WIRE_H
static uint32_t ios7lab_wire_get(const unsigned char *p)
{
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}
static void ios7lab_wire_put(unsigned char *p, uint32_t value)
{
    memcpy(p, &value, sizeof(value));
}

static int ios7lab_iokit_request32(unsigned char *message, size_t length)
{
    uint32_t scalars, bytes, tail[10], native_tail[6];
    size_t offset;
    if (length < 84 || length > 84 + 128 + 4096 ||
        ios7lab_wire_get(message + 4) != length ||
        ios7lab_wire_get(message + 20) != 2865 ||
        (ios7lab_wire_get(message) & 0x80000000U) || message[28] != 1)
        return 0;
    scalars = ios7lab_wire_get(message + 36);
    if (scalars > 16)
        return 0;
    offset = 40 + 8 * scalars;
    if (offset + 4 > length)
        return 0;
    bytes = ios7lab_wire_get(message + offset);
    if (bytes > 4096)
        return 0;
    offset += 4 + ((bytes + 3U) & ~3U);
    if (offset + 40 != length)
        return 0;
    memcpy(tail, message + offset, sizeof(tail));
    /* No silently truncated addresses or lengths. */
    if (tail[1] || tail[3] || tail[7] || tail[9])
        return 0;
    native_tail[0] = tail[0];
    native_tail[1] = tail[2];
    native_tail[2] = tail[4];
    native_tail[3] = tail[5];
    native_tail[4] = tail[6];
    native_tail[5] = tail[8];
    memcpy(message + offset, native_tail, sizeof(native_tail));
    memset(message + length - 16, 0, 16);
    ios7lab_wire_put(message + 4, (uint32_t)length - 16);
    return 1;
}

static int ios7lab_iokit_reply64(unsigned char *message, size_t capacity)
{
    uint32_t length, bytes, scalars;
    size_t offset;
    if (capacity < 48)
        return 0;
    length = ios7lab_wire_get(message + 4);
    if (length < 48 || length > 48 + 4096 + 128 || length + 4 > capacity ||
        ios7lab_wire_get(message + 20) != 2965 ||
        ios7lab_wire_get(message + 32) != 0 ||
        (ios7lab_wire_get(message) & 0x80000000U))
        return 0;
    bytes = ios7lab_wire_get(message + 36);
    if (bytes > 4096)
        return 0;
    offset = 40 + ((bytes + 3U) & ~3U);
    if (offset + 4 > length)
        return 0;
    scalars = ios7lab_wire_get(message + offset);
    if (scalars > 16)
        return 0;
    offset += 4 + 8 * scalars;
    if (offset + 4 != length)
        return 0;
    ios7lab_wire_put(message + length, 0);
    ios7lab_wire_put(message + 4, length + 4);
    return 1;
}

/* mach_port_get_context's request is unchanged; only successful reply widens. */
static int ios7lab_context_request(unsigned char *message, size_t length)
{
    uint32_t id;
    if (length < 36 || ios7lab_wire_get(message + 4) != length ||
        (ios7lab_wire_get(message) & 0x80000000U) || message[28] != 1)
        return 0;
    id = ios7lab_wire_get(message + 20);
    if (id == 3228 && length == 36) return 1;
    if (id == 3229 && length == 44 && !ios7lab_wire_get(message + 40)) {
        ios7lab_wire_put(message + 4, 40);
        return 2;
    }
    return 0;
}

static int ios7lab_context_reply64(unsigned char *message, size_t capacity)
{
    if (capacity < 44 || ios7lab_wire_get(message + 4) != 40 ||
        ios7lab_wire_get(message + 20) != 3328 ||
        ios7lab_wire_get(message + 32) != 0 ||
        (ios7lab_wire_get(message) & 0x80000000U))
        return 0;
    ios7lab_wire_put(message + 40, 0);
    ios7lab_wire_put(message + 4, 44);
    return 1;
}
#endif

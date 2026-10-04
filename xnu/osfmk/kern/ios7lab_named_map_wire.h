/* ARM32 native iOS7 mach_vm_map MIG4811 wire adapter.
 * Include after uint32_t, size_t and memcpy/memset declarations.
 * Kernel descriptors are already normalized; their rights remain untouched.
 */
#ifndef IOS7LAB_NAMED_MAP_WIRE_H
#define IOS7LAB_NAMED_MAP_WIRE_H
static uint32_t ios7lab_named_map_get(const unsigned char *p)
{
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}
static void ios7lab_named_map_put(unsigned char *p, uint32_t value)
{
    memcpy(p, &value, sizeof(value));
}
static int ios7lab_named_map_request32(unsigned char *message, size_t length)
{
    uint32_t tail[10], port;
    if (length != 104 || ios7lab_named_map_get(message + 4) != 104 ||
        ios7lab_named_map_get(message + 20) != 4811 ||
        !(ios7lab_named_map_get(message) & 0x80000000U) ||
        ios7lab_named_map_get(message + 24) != 1 ||
        message[39] != 0 || message[38] != 17 || message[48] != 1)
        return 0;
    port = ios7lab_named_map_get(message + 28);
    if (!port || port == 0xffffffffU) return 0;
    /* Real kernel maps remain ARM32. Never silently truncate native values. */
    if (ios7lab_named_map_get(message + 56) ||
        ios7lab_named_map_get(message + 64) ||
        ios7lab_named_map_get(message + 72)) return 0;
    tail[0] = ios7lab_named_map_get(message + 52); /* address */
    tail[1] = ios7lab_named_map_get(message + 60); /* size */
    tail[2] = ios7lab_named_map_get(message + 68); /* mask */
    tail[3] = ios7lab_named_map_get(message + 76); /* flags */
    tail[4] = ios7lab_named_map_get(message + 80); /* object offset low */
    tail[5] = ios7lab_named_map_get(message + 84); /* object offset high */
    tail[6] = ios7lab_named_map_get(message + 88); /* copy */
    tail[7] = ios7lab_named_map_get(message + 92); /* cur protection */
    tail[8] = ios7lab_named_map_get(message + 96); /* max protection */
    tail[9] = ios7lab_named_map_get(message + 100); /* inheritance */
    memcpy(message + 52, tail, sizeof(tail));
    memset(message + 92, 0, 12);
    ios7lab_named_map_put(message + 4, 92);
    return 1;
}
static int ios7lab_named_map_reply64(unsigned char *message, size_t capacity)
{
    if (capacity < 44 || ios7lab_named_map_get(message + 4) != 40 ||
        ios7lab_named_map_get(message + 20) != 4911 ||
        ios7lab_named_map_get(message + 32) != 0 ||
        (ios7lab_named_map_get(message) & 0x80000000U)) return 0;
    ios7lab_named_map_put(message + 40, 0);
    ios7lab_named_map_put(message + 4, 44);
    return 1;
}
#endif

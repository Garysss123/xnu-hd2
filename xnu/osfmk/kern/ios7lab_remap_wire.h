/* Native iOS7 MIG4813 transport over the real ARM32 remap backend. */
#ifndef IOS7LAB_REMAP_WIRE_H
#define IOS7LAB_REMAP_WIRE_H
static uint32_t ios7lab_remap_get(const unsigned char *p)
{
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}
static void ios7lab_remap_put(unsigned char *p, uint32_t value)
{
    memcpy(p, &value, sizeof(value));
}
static int ios7lab_remap_request32(unsigned char *message, size_t length)
{
    uint32_t tail[7], port;
    if (length != 96 || ios7lab_remap_get(message + 4) != 96 ||
        ios7lab_remap_get(message + 20) != 4813 ||
        !(ios7lab_remap_get(message) & 0x80000000U) ||
        ios7lab_remap_get(message + 24) != 1 ||
        message[39] != 0 || message[38] != 17 || message[48] != 1)
        return 0;
    port = ios7lab_remap_get(message + 28);
    if (!port || port == 0xffffffffU) return 0;
    if (ios7lab_remap_get(message + 56) ||
        ios7lab_remap_get(message + 64) ||
        ios7lab_remap_get(message + 72) ||
        ios7lab_remap_get(message + 84)) return 0;
    tail[0] = ios7lab_remap_get(message + 52); /* target address */
    tail[1] = ios7lab_remap_get(message + 60); /* size */
    tail[2] = ios7lab_remap_get(message + 68); /* mask */
    tail[3] = ios7lab_remap_get(message + 76); /* flags */
    tail[4] = ios7lab_remap_get(message + 80); /* source address */
    tail[5] = ios7lab_remap_get(message + 88); /* copy */
    tail[6] = ios7lab_remap_get(message + 92); /* inheritance */
    memcpy(message + 52, tail, sizeof(tail));
    memset(message + 80, 0, 16);
    ios7lab_remap_put(message + 4, 80);
    return 1;
}
static int ios7lab_remap_reply64(unsigned char *message, size_t capacity)
{
    if (capacity < 52 || ios7lab_remap_get(message + 4) != 48 ||
        ios7lab_remap_get(message + 20) != 4913 ||
        ios7lab_remap_get(message + 32) != 0 ||
        (ios7lab_remap_get(message) & 0x80000000U)) return 0;
    /* Retain real backend cur/max protections after widening the address. */
    memmove(message + 44, message + 40, 8);
    ios7lab_remap_put(message + 40, 0);
    ios7lab_remap_put(message + 4, 52);
    return 1;
}
#endif

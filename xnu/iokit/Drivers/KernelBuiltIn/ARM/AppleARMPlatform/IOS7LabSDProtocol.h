#ifndef IOS7LAB_SD_PROTOCOL_H
#define IOS7LAB_SD_PROTOCOL_H
#include <stdint.h>

/* Standard CSD response words in controller MSW-first order. No fixed capacity. */
static inline uint32_t ios7lab_sd_csd_bits(const uint32_t words[4], unsigned high, unsigned low)
{
    uint32_t value = 0;
    unsigned bit;
    if (high > 127 || high < low || high - low >= 32) return 0;
    for (bit = low; bit <= high; ++bit)
        value |= ((words[(127U - bit) / 32U] >> (bit % 32U)) & 1U) << (bit - low);
    return value;
}

static inline int ios7lab_sd_capacity(const uint32_t words[4], int highCapacity,
                                    uint64_t *sectors, int *csdWriteProtected)
{
    uint32_t version = ios7lab_sd_csd_bits(words, 127, 126);
    uint64_t count = 0;
    if (!sectors || !csdWriteProtected) return 0;
    if (version == 0 && !highCapacity) {
        uint32_t length = ios7lab_sd_csd_bits(words, 83, 80);
        uint32_t multiplier = ios7lab_sd_csd_bits(words, 49, 47);
        uint32_t size = ios7lab_sd_csd_bits(words, 73, 62);
        uint64_t bytes;
        if (length < 9 || length > 11) return 0;
        bytes = ((uint64_t)size + 1U) << (multiplier + 2U + length);
        count = bytes >> 9;
        if (!count || ((count - 1U) << 9) > UINT32_MAX) return 0;
    } else if (version == 1 && highCapacity) {
        uint32_t size = ios7lab_sd_csd_bits(words, 69, 48);
        count = ((uint64_t)size + 1U) << 10;
        if (!count || count - 1U > UINT32_MAX) return 0;
    } else return 0;
    *sectors = count;
    *csdWriteProtected = ios7lab_sd_csd_bits(words, 13, 12) != 0;
    return 1;
}

static inline int ios7lab_sd_request_range(uint64_t capacity, uint64_t start,
                                         uint64_t blocks, uint64_t length,
                                         uint64_t *bytes)
{
    uint64_t wanted;
    if (!bytes || !blocks || blocks > UINT64_MAX / 512U) return 0;
    wanted = blocks * 512U;
    if (start >= capacity || blocks > capacity - start || wanted != length) return 0;
    *bytes = wanted;
    return 1;
}

static inline int ios7lab_sd_command_address(uint64_t sector, int highCapacity,
                                           uint32_t *argument)
{
    if (!argument) return 0;
    if (highCapacity) {
        if (sector > UINT32_MAX) return 0;
        *argument = (uint32_t)sector;
    } else {
        if (sector > UINT32_MAX / 512U) return 0;
        *argument = (uint32_t)(sector * 512U);
    }
    return 1;
}
#endif

#ifndef IOS7LAB_SURFACE_GEOMETRY_H
#define IOS7LAB_SURFACE_GEOMETRY_H
#include <stdint.h>

enum { IOS7LAB_SURFACE_MAX_WIDTH = 2048, IOS7LAB_SURFACE_MAX_HEIGHT = 2048,
       IOS7LAB_SURFACE_MAX_ROW_BYTES = 16384 };
/* Native root initializer writes these five words directly to its limits
 * cache. Alignment fields are masks; native getters add one to each mask. */
struct IOS7LabSurfaceCapabilities {
    uint32_t offsetAlignmentMask, rowAlignmentMask, maximumRowBytes;
    uint32_t maximumWidth, maximumHeight;
};
typedef char IOS7LabSurfaceCapabilitiesMustBe20[sizeof(IOS7LabSurfaceCapabilities)==20?1:-1];
static IOS7LabSurfaceCapabilities ios7lab_surface_capabilities(void)
{
    IOS7LabSurfaceCapabilities result = {4095, 63, IOS7LAB_SURFACE_MAX_ROW_BYTES,
        IOS7LAB_SURFACE_MAX_WIDTH, IOS7LAB_SURFACE_MAX_HEIGHT};
    return result;
}

/* Original IOSurface simple-create selector8, from 326cab88..326cacf2. */
struct IOS7LabSimpleSurfaceRequest {
    uint32_t memoryAddressLow, memoryAddressHigh, width, height, pixelFormat, bytesPerElement;
    uint32_t bytesPerRow, allocationSize;
};
typedef char IOS7LabSimpleSurfaceRequestMustBe32[sizeof(IOS7LabSimpleSurfaceRequest)==32?1:-1];

static bool ios7lab_surface_geometry(const IOS7LabSimpleSurfaceRequest *input,
                                    IOS7LabSimpleSurfaceRequest *output)
{
    if (!input || !output || input->memoryAddressHigh ||
        !input->width || !input->height || input->width > IOS7LAB_SURFACE_MAX_WIDTH ||
        input->height > IOS7LAB_SURFACE_MAX_HEIGHT ||
        input->bytesPerElement != 4 ||
        (input->pixelFormat != 0x42475241U && input->pixelFormat != 0x52474241U)) return false;
    *output = *input;
    uint64_t minimumRow = (uint64_t)input->width * 4;
    if (!output->bytesPerRow) output->bytesPerRow = (uint32_t)((minimumRow + 63) & ~63ULL);
    if (output->bytesPerRow < minimumRow || output->bytesPerRow > IOS7LAB_SURFACE_MAX_ROW_BYTES ||
        (output->bytesPerRow & 3)) return false;
    uint64_t fullRowBytes = (uint64_t)output->bytesPerRow * output->height;
    uint64_t minimumBytes = fullRowBytes;
    /* Only an explicit borrowed buffer may omit padding after its last
     * active pixel. Nonzero dimensions and bounded row/width/height above
     * make the subtraction and these uint64 products/addition safe.
     * Retain the original full-row geometry cap and actual caller length. */
    if (input->memoryAddressLow && input->allocationSize)
        minimumBytes = (uint64_t)output->bytesPerRow * (output->height - 1U) + minimumRow;
    if (!output->allocationSize) output->allocationSize = (uint32_t)((fullRowBytes + 4095) & ~4095ULL);
    if (fullRowBytes > 16U * 1024 * 1024 || output->allocationSize < minimumBytes ||
        output->allocationSize > 16U * 1024 * 1024) return false;
    if (output->memoryAddressLow &&
        (output->memoryAddressLow < 4096 ||
         (uint64_t)output->memoryAddressLow + output->allocationSize > 0x40000000ULL)) return false;
    return true;
}
#endif

#ifndef IOS7_LEO_FAT_LOCATOR_H
#define IOS7_LEO_FAT_LOCATOR_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FL32_SECTOR_BYTES 512U
#define FL32_DIRECTORY_CAP 128U
#define FL32_IMAGE_MAGIC 0x464c4831U
enum fl32_error { FL32_OK=0, FL32_ARGUMENT=-1, FL32_IO=-2,
 FL32_FORMAT=-3, FL32_RANGE=-4, FL32_CHAIN=-5, FL32_MIRROR=-6,
 FL32_NOT_FOUND=-7, FL32_DUPLICATE=-8, FL32_LIMIT=-9, FL32_HFS=-10 };
typedef int (*fl32_read512)(void *cookie,uint64_t physical_lba,uint8_t out[512]);
struct fl32_io { void *cookie; fl32_read512 read512; uint64_t sectors; };
struct fl32_limits { uint32_t read_budget,directory_clusters; uint64_t expected_file_bytes; };
/* Caller heap-owned, never a large kernel stack allocation. No malloc in parser. */
struct fl32_work {
 uint8_t fat[2][512],sector[512],primary[512],alternate[512];
 uint64_t fat_lba[2];
 uint32_t seen_directory[FL32_DIRECTORY_CAP*2U];
 uint32_t reads; int backend_error;
};
/* Logical image LBA is bounded by the FAT32 <4GiB file; physical LBA is UInt64. */
struct fl32_extent { uint64_t physical_lba; uint32_t logical_lba,sectors; };
struct fl32_image {
 uint32_t magic,extent_count,image_sectors,hfs_block_bytes,hfs_blocks;
 uint64_t file_bytes,card_sectors,partition_lba,partition_sectors,data_lba;
 uint32_t sectors_per_cluster,first_cluster;
 uint16_t hfs_signature,hfs_version;
};
/* Success means only a validated readonly map/header, never mounted HFS.
 * Selects one unambiguous primary MBR FAT32 partition; no GPT/extended/FAT12/16.
 * IO callback returns0 only for a complete512B read; nonzero is retained in work.
 * On failure image magic/count are zero; partially written extent storage invalid.
 * Exact owned SFN path IOS7/ROOTFS.HFS, fragmented chain, mirrored/active FAT,
 * clean nonjournaled bare HFS+/HFSX primary+alternate geometry validated.
 */
int fl32_locate(const struct fl32_io *,const struct fl32_limits *,struct fl32_work *,
               struct fl32_extent *,uint32_t extent_capacity,struct fl32_image *);
/* No IO; checked single-LBA translation plus remaining contiguous sector count.
 * Caller keeps successful map immutable and delegates returned LBA to realSDCC2.
 */
int fl32_translate(const struct fl32_image *,const struct fl32_extent *,uint32_t extent_capacity,
                   uint64_t logical_lba,uint64_t *physical_lba,uint32_t *contiguous_sectors);
#ifdef __cplusplus
}
#endif
#endif

/*
 * ============================================================================
 * 【中文导读】lfs_filebd —— POSIX 文件模拟块设备
 * ----------------------------------------------------------------------------
 * 职责：把一个普通文件当作 flash（lseek+read/write），适合在 PC 上
 *   生成/检查 littlefs 磁盘镜像（littlefs-fuse 等工具链的基础）。
 * 对外 API：lfs_filebd_create(cfg, path, bdcfg) 打开镜像文件，
 *   read/prog 走 lseek+read/write，erase 为 no-op，
 *   sync 调 fsync（Windows 用 FlushFileBuffers）——这是它比 rambd
 *   真实的地方：数据真的要过 OS 页缓存。
 * 数据流位置：lfs 缓存层 -> cfg 回调 -> 本文件 -> 镜像文件。
 * ============================================================================
 */
/*
 * Block device emulated in a file
 *
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef LFS_FILEBD_H
#define LFS_FILEBD_H

#include "lfs.h"
#include "lfs_util.h"

#ifdef __cplusplus
extern "C"
{
#endif


// Block device specific tracing
#ifndef LFS_FILEBD_TRACE
#ifdef LFS_FILEBD_YES_TRACE
#define LFS_FILEBD_TRACE(...) LFS_TRACE(__VA_ARGS__)
#else
#define LFS_FILEBD_TRACE(...)
#endif
#endif

// filebd config
struct lfs_filebd_config {
    // Minimum size of a read operation in bytes.
    lfs_size_t read_size;

    // Minimum size of a program operation in bytes.
    lfs_size_t prog_size;

    // Size of an erase operation in bytes.
    lfs_size_t erase_size;

    // Number of erase blocks on the device.
    lfs_size_t erase_count;
};

// filebd state
typedef struct lfs_filebd {
    int fd;
    const struct lfs_filebd_config *cfg;
} lfs_filebd_t;


// Create a file block device
int lfs_filebd_create(const struct lfs_config *cfg, const char *path,
        const struct lfs_filebd_config *bdcfg);

// Clean up memory associated with block device
int lfs_filebd_destroy(const struct lfs_config *cfg);

// Read a block
int lfs_filebd_read(const struct lfs_config *cfg, lfs_block_t block,
        lfs_off_t off, void *buffer, lfs_size_t size);

// Program a block
//
// The block must have previously been erased.
int lfs_filebd_prog(const struct lfs_config *cfg, lfs_block_t block,
        lfs_off_t off, const void *buffer, lfs_size_t size);

// Erase a block
//
// A block must be erased before being programmed. The
// state of an erased block is undefined.
int lfs_filebd_erase(const struct lfs_config *cfg, lfs_block_t block);

// Sync the block device
int lfs_filebd_sync(const struct lfs_config *cfg);


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif

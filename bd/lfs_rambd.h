/*
 * ============================================================================
 * 【中文导读】lfs_rambd —— RAM 模拟块设备（最小移植参考实现）
 * ----------------------------------------------------------------------------
 * 职责：用一段 RAM buffer 完全模拟 flash 的读写语义，是学习/移植
 *   littlefs 的"第一块垫脚石"：接口形态与真实 SPI Flash 驱动完全一致。
 * 对外 API（作为 lfs_config 的 read/prog/erase/sync 注册）：
 *   lfs_rambd_create/destroy   创建/销毁（分配 erase_size*erase_count 内存）
 *   lfs_rambd_read/prog/erase/sync  四回调实现（erase/sync 是 no-op）
 * 注意：prog 直接 memcpy，不校验"已擦除"；erase 为空操作——RAM 无需
 *   模拟 1->0 语义，因此掉电安全在这里测不出来（要用 emubd）。
 * 数据流位置：lfs_bd_read/prog(缓存层) -> cfg->read/prog -> 本文件 -> RAM。
 * ============================================================================
 */
/*
 * Block device emulated in RAM
 *
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef LFS_RAMBD_H
#define LFS_RAMBD_H

#include "lfs.h"
#include "lfs_util.h"

#ifdef __cplusplus
extern "C"
{
#endif


// Block device specific tracing
#ifndef LFS_RAMBD_TRACE
#ifdef LFS_RAMBD_YES_TRACE
#define LFS_RAMBD_TRACE(...) LFS_TRACE(__VA_ARGS__)
#else
#define LFS_RAMBD_TRACE(...)
#endif
#endif

// rambd config
struct lfs_rambd_config {
    // Minimum size of a read operation in bytes.
    lfs_size_t read_size;

    // Minimum size of a program operation in bytes.
    lfs_size_t prog_size;

    // Size of an erase operation in bytes.
    lfs_size_t erase_size;

    // Number of erase blocks on the device.
    lfs_size_t erase_count;

    // Optional statically allocated buffer for the block device.
    void *buffer;
};

// rambd state
typedef struct lfs_rambd {
    uint8_t *buffer;
    const struct lfs_rambd_config *cfg;
} lfs_rambd_t;


// Create a RAM block device
int lfs_rambd_create(const struct lfs_config *cfg,
        const struct lfs_rambd_config *bdcfg);

// Clean up memory associated with block device
int lfs_rambd_destroy(const struct lfs_config *cfg);

// Read a block
int lfs_rambd_read(const struct lfs_config *cfg, lfs_block_t block,
        lfs_off_t off, void *buffer, lfs_size_t size);

// Program a block
//
// The block must have previously been erased.
int lfs_rambd_prog(const struct lfs_config *cfg, lfs_block_t block,
        lfs_off_t off, const void *buffer, lfs_size_t size);

// Erase a block
//
// A block must be erased before being programmed. The
// state of an erased block is undefined.
int lfs_rambd_erase(const struct lfs_config *cfg, lfs_block_t block);

// Sync the block device
int lfs_rambd_sync(const struct lfs_config *cfg);


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif

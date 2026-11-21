/*
 * 【中文导读】lfs_util.c —— CRC32 与工具函数（B级导读）
 * 元数据提交的完整性全靠这里的 CRC32（每条记录带校验）；另有
 * lfs_aligndown/alignup、伪随机数、调试打印等小工具。
 */
/*
 * lfs util functions
 *
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "lfs_util.h"

// Only compile if user does not provide custom config
#ifndef LFS_CONFIG


// If user provides their own CRC impl we don't need this
#ifndef LFS_CRC
// Software CRC implementation with small lookup table
/**
 * @brief 软件CRC32：半字节（4bit）查表法，多项式 0x04c11db7
 * @param  crc    运行中的校验值（首次传 0xffffffff）
 * @param  buffer 数据
 * @param  size   长度
 * @return 更新后的 crc（收尾时调用方以 0xffffffff 收敛）
 * @note   rtable 是反射（reflected）多项式的 16 项表，仅 64 字节
 *         ROM、零 RAM 表——比 256 项字节表省 ROM，比逐位快一倍。
 *         循环里两步分别消费字节的低 4 位与高 4 位：
 *         crc = (crc>>4) ^ rtable[(crc ^ data) & 0xf]。
 *         元数据提交 CRC、坏块检测、挂载 seed 全部由它承担，
 *         是 littlefs 掉电安全的"校验地基"。
 */
uint32_t lfs_crc(uint32_t crc, const void *buffer, size_t size) {
    static const uint32_t rtable[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
        0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };

    const uint8_t *data = buffer;

    for (size_t i = 0; i < size; i++) {
        crc = (crc >> 4) ^ rtable[(crc ^ (data[i] >> 0)) & 0xf];
        crc = (crc >> 4) ^ rtable[(crc ^ (data[i] >> 4)) & 0xf];
    }

    return crc;
}
#endif


#endif

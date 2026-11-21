/*
 * lfs utility functions
 *
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef LFS_UTIL_H
#define LFS_UTIL_H

// 两级宏展开是 C 预处理器把宏参数变成字符串的标准技巧：
// LFS_STRINGIZE(LFS_CONFIG) 先展开 LFS_CONFIG 的值再 #串化，
// 一级写法只会得到字面量 "LFS_CONFIG"。
#define LFS_STRINGIZE(x) LFS_STRINGIZE2(x)
#define LFS_STRINGIZE2(x) #x

// Users can override lfs_util.h with their own configuration by defining
// LFS_CONFIG as a header file to include (-DLFS_CONFIG=lfs_config.h).
//
// If LFS_CONFIG is used, none of the default utils will be emitted and must be
// provided by the config file. To start, I would suggest copying lfs_util.h
// and modifying as needed.
#ifdef LFS_CONFIG
#include LFS_STRINGIZE(LFS_CONFIG)
#else

// Alternatively, users can provide a header file which defines
// macros and other things consumed by littlefs.
//
// For example, provide my_defines.h, which contains
// something like:
//
// #include <stddef.h>
// extern void *my_malloc(size_t sz);
// #define LFS_MALLOC(sz) my_malloc(sz)
//
// And build littlefs with the header by defining LFS_DEFINES.
// (-DLFS_DEFINES=my_defines.h)

#ifdef LFS_DEFINES
#include LFS_STRINGIZE(LFS_DEFINES)
#endif

// System includes
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>

#ifndef LFS_NO_MALLOC
#include <stdlib.h>
#endif
#ifndef LFS_NO_ASSERT
#include <assert.h>
#endif
#if !defined(LFS_NO_DEBUG) || \
        !defined(LFS_NO_WARN) || \
        !defined(LFS_NO_ERROR) || \
        defined(LFS_YES_TRACE)
#include <stdio.h>
#endif

#ifdef __cplusplus
extern "C"
{
#endif


// Macros, may be replaced by system specific wrappers. Arguments to these
// macros must not have side-effects as the macros can be removed for a smaller
// code footprint

// Logging functions
/**
 * 调试打印四件套（TRACE/DEBUG/WARN/ERROR）：默认编译期整体消失
 * （零代码体积）；定义 LFS_YES_TRACE 才打开 TRACE。三元结构
 * LFS_TRACE -> LFS_TRACE_ 是为了让 "..." 参数列表尾部能自动补 ""，
 * 兼容只支持 __VA_ARGS__ 严格计参的老编译器。
 * 移植者可 -DLFS_TRACE=my_log 重定向到自己的日志后端。
 */
#ifndef LFS_TRACE
#ifdef LFS_YES_TRACE
#define LFS_TRACE_(fmt, ...) \
    printf("%s:%d:trace: " fmt "%s\n", __FILE__, __LINE__, __VA_ARGS__)
#define LFS_TRACE(...) LFS_TRACE_(__VA_ARGS__, "")
#else
#define LFS_TRACE(...)
#endif
#endif

#ifndef LFS_DEBUG
#ifndef LFS_NO_DEBUG
#define LFS_DEBUG_(fmt, ...) \
    printf("%s:%d:debug: " fmt "%s\n", __FILE__, __LINE__, __VA_ARGS__)
#define LFS_DEBUG(...) LFS_DEBUG_(__VA_ARGS__, "")
#else
#define LFS_DEBUG(...)
#endif
#endif

#ifndef LFS_WARN
#ifndef LFS_NO_WARN
#define LFS_WARN_(fmt, ...) \
    printf("%s:%d:warn: " fmt "%s\n", __FILE__, __LINE__, __VA_ARGS__)
#define LFS_WARN(...) LFS_WARN_(__VA_ARGS__, "")
#else
#define LFS_WARN(...)
#endif
#endif

#ifndef LFS_ERROR
#ifndef LFS_NO_ERROR
#define LFS_ERROR_(fmt, ...) \
    printf("%s:%d:error: " fmt "%s\n", __FILE__, __LINE__, __VA_ARGS__)
#define LFS_ERROR(...) LFS_ERROR_(__VA_ARGS__, "")
#else
#define LFS_ERROR(...)
#endif
#endif

// Runtime assertions
/**
 * 运行时断言：默认映射到 assert()；-DLFS_NO_ASSERT 可整体关闭
 * （发布镜像常用）。littlefs 用它校验的是"调用方契约"
 * （配置对齐、文件已打开、写入已擦除），违反即未定义行为。
 */
#ifndef LFS_ASSERT
#ifndef LFS_NO_ASSERT
#define LFS_ASSERT(test) assert(test)
#else
#define LFS_ASSERT(test)
#endif
#endif


// Builtin functions, these may be replaced by more efficient
// toolchain-specific implementations. LFS_NO_INTRINSICS falls back to a more
// expensive basic C implementation for debugging purposes

// Min/max functions for unsigned 32-bit numbers
/** @brief 32 位无符号取大；参数不得有副作用（可能被宏化/删除省体积） */
static inline uint32_t lfs_max(uint32_t a, uint32_t b) {
    return (a > b) ? a : b;
}

/** @brief 32 位无符号取小 */
static inline uint32_t lfs_min(uint32_t a, uint32_t b) {
    return (a < b) ? a : b;
}

// Align to nearest multiple of a size
/**
 * @brief 向下对齐到 alignment 的倍数
 * @note  flash 读写的最小粒度是 read_size/prog_size，所有缓存窗口
 *        和提交落点都要先过这两个函数。
 */
static inline uint32_t lfs_aligndown(uint32_t a, uint32_t alignment) {
    return a - (a % alignment);
}

/** @brief 向上对齐：先加 alignment-1 再向下对齐（经典进位技巧） */
static inline uint32_t lfs_alignup(uint32_t a, uint32_t alignment) {
    return lfs_aligndown(a + alignment-1, alignment);
}

// Find the smallest power of 2 greater than or equal to a
/**
 * @brief 求不小于 a 的最小 2 的幂（a>=1）
 * @note  GCC/ARMCC 走 __builtin_clz 单指令；回退实现是二分
 *        集查（5 步定位最高位）。CTZ 跳表的跳距计算用它。
 */
static inline uint32_t lfs_npw2(uint32_t a) {
#if !defined(LFS_NO_INTRINSICS) && (defined(__GNUC__) || defined(__CC_ARM))
    return 32 - __builtin_clz(a-1);
#else
    uint32_t r = 0;
    uint32_t s;
    a -= 1;
    s = (a > 0xffff) << 4; a >>= s; r |= s;
    s = (a > 0xff  ) << 3; a >>= s; r |= s;
    s = (a > 0xf   ) << 2; a >>= s; r |= s;
    s = (a > 0x3   ) << 1; a >>= s; r |= s;
    return (r | (a >> 1)) + 1;
#endif
}

// Count the number of trailing binary zeros in a
// lfs_ctz(0) may be undefined
/**
 * @brief 末尾 0 计数（CTZ 跳表的名字来源！）
 * @note  第 n 块的指针数 = ctz(n)+1；ctz(0) 未定义（跳表索引
 *        恒 >=1，调用方已保证）。回退实现借 npw2 反推。
 */
static inline uint32_t lfs_ctz(uint32_t a) {
#if !defined(LFS_NO_INTRINSICS) && defined(__GNUC__)
    return __builtin_ctz(a);
#else
    return lfs_npw2((a & -a) + 1) - 1;
#endif
}

// Count the number of binary ones in a
/**
 * @brief 二进制 1 计数
 * @note  回退实现是经典的分治 SWAR：两两计数→四四→字节→乘法
 *        一次折算全字节。lfs_ctz_index 用它把 O(n) 的指针求和
 *        化为 O(1) 公式（2n-popcount(n) 恒等式）。
 */
static inline uint32_t lfs_popc(uint32_t a) {
#if !defined(LFS_NO_INTRINSICS) && (defined(__GNUC__) || defined(__CC_ARM))
    return __builtin_popcount(a);
#else
    a = a - ((a >> 1) & 0x55555555);
    a = (a & 0x33333333) + ((a >> 2) & 0x33333333);
    return (((a + (a >> 4)) & 0xf0f0f0f) * 0x1010101) >> 24;
#endif
}

// Find the sequence comparison of a and b, this is the distance
// between a and b ignoring overflow
/**
 * @brief 序列号比较：返回 a-b 的无符号距离（忽略溢出）
 * @note  元数据对的 rev 用它裁决新旧——回绕后 (a-b) 的无符号
 *        解释仍指向更新的块（RFC1982 序列算术）。
 */
static inline int lfs_scmp(uint32_t a, uint32_t b) {
    return (int)(unsigned)(a - b);
}

// Convert between 32-bit little-endian and native order
/**
 * @brief 32 位小端 -> 主机序（读盘必经）
 * @note  已知小端机直接返回；大端机 bswap；未知端序按字节拼装。
 *        littlefs 盘上除 tag（大端）外全部小端。
 */
static inline uint32_t lfs_fromle32(uint32_t a) {
#if (defined(  BYTE_ORDER  ) && defined(  ORDER_LITTLE_ENDIAN  ) &&   BYTE_ORDER   ==   ORDER_LITTLE_ENDIAN  ) || \
    (defined(__BYTE_ORDER  ) && defined(__ORDER_LITTLE_ENDIAN  ) && __BYTE_ORDER   == __ORDER_LITTLE_ENDIAN  ) || \
    (defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    return a;
#elif !defined(LFS_NO_INTRINSICS) && ( \
    (defined(  BYTE_ORDER  ) && defined(  ORDER_BIG_ENDIAN  ) &&   BYTE_ORDER   ==   ORDER_BIG_ENDIAN  ) || \
    (defined(__BYTE_ORDER  ) && defined(__ORDER_BIG_ENDIAN  ) && __BYTE_ORDER   == __ORDER_BIG_ENDIAN  ) || \
    (defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__))
    return __builtin_bswap32(a);
#else
    return ((uint32_t)((uint8_t*)&a)[0] <<  0) |
           ((uint32_t)((uint8_t*)&a)[1] <<  8) |
           ((uint32_t)((uint8_t*)&a)[2] << 16) |
           ((uint32_t)((uint8_t*)&a)[3] << 24);
#endif
}

/** @brief 主机序 -> 32 位小端（写盘必经）；小端/大端互换都是同一运算 */
static inline uint32_t lfs_tole32(uint32_t a) {
    return lfs_fromle32(a);
}

// Convert between 32-bit big-endian and native order
/**
 * @brief 32 位大端 -> 主机序
 * @note  只用于 tag：盘上 tag 是 littlefs 里唯一的大端数据
 *        （valid 位必须落在提交首字节，见 SPEC.md）。
 */
static inline uint32_t lfs_frombe32(uint32_t a) {
#if !defined(LFS_NO_INTRINSICS) && ( \
    (defined(  BYTE_ORDER  ) && defined(  ORDER_LITTLE_ENDIAN  ) &&   BYTE_ORDER   ==   ORDER_LITTLE_ENDIAN  ) || \
    (defined(__BYTE_ORDER  ) && defined(__ORDER_LITTLE_ENDIAN  ) && __BYTE_ORDER   == __ORDER_LITTLE_ENDIAN  ) || \
    (defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__))
    return __builtin_bswap32(a);
#elif (defined(  BYTE_ORDER  ) && defined(  ORDER_BIG_ENDIAN  ) &&   BYTE_ORDER   ==   ORDER_BIG_ENDIAN  ) || \
    (defined(__BYTE_ORDER  ) && defined(__ORDER_BIG_ENDIAN  ) && __BYTE_ORDER   == __ORDER_BIG_ENDIAN  ) || \
    (defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    return a;
#else
    return ((uint32_t)((uint8_t*)&a)[0] << 24) |
           ((uint32_t)((uint8_t*)&a)[1] << 16) |
           ((uint32_t)((uint8_t*)&a)[2] <<  8) |
           ((uint32_t)((uint8_t*)&a)[3] <<  0);
#endif
}

/** @brief 主机序 -> 32 位大端（写 tag 用） */
static inline uint32_t lfs_tobe32(uint32_t a) {
    return lfs_frombe32(a);
}

/**
 * @brief CRC32 计算入口（多项式 0x04c11db7，初值 0xffffffff）
 * @note  用户可 -DLFS_CRC=my_crc 换硬件 CRC 外设；默认实现见
 *        lfs_util.c（半字节查表，ROM 16 字、零 RAM 表）。
 *        元数据提交的完整性、坏块检测、伪随机 seed 全靠它。
 */
// Calculate CRC-32 with polynomial = 0x04c11db7
#ifdef LFS_CRC
static inline uint32_t lfs_crc(uint32_t crc, const void *buffer, size_t size) {
    return LFS_CRC(crc, buffer, size);
}
#else
uint32_t lfs_crc(uint32_t crc, const void *buffer, size_t size);
#endif

// Allocate memory, only used if buffers are not provided to littlefs
//
// littlefs current has no alignment requirements, as it only allocates
// byte-level buffers.
/**
 * @brief 内存分配出口：LFS_MALLOC 重定向 -> malloc -> 禁堆返回 NULL
 * @note  仅当用户未提供静态缓冲时才被调用（缓存/文件缓冲）；
 *        littlefs 只分配字节缓冲，无对齐要求。
 */
static inline void *lfs_malloc(size_t size) {
#if defined(LFS_MALLOC)
    return LFS_MALLOC(size);
#elif !defined(LFS_NO_MALLOC)
    return malloc(size);
#else
    (void)size;
    return NULL;
#endif
}

// Deallocate memory, only used if buffers are not provided to littlefs
/** @brief 内存释放出口（与 lfs_malloc 配套） */
static inline void lfs_free(void *p) {
#if defined(LFS_FREE)
    LFS_FREE(p);
#elif !defined(LFS_NO_MALLOC)
    free(p);
#else
    (void)p;
#endif
}


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
#endif

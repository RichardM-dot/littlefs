/*
 * ============================================================================
 * 【中文导读】lfs.c —— littlefs 文件系统核心实现（全库精华，一个文件即全部）
 * ----------------------------------------------------------------------------
 * littlefs 的两大设计目标（先读 DESIGN.md 再读本文件，事半功倍）：
 *   1. 掉电安全（power-loss resilience）: 任何时刻断电，文件系统都能恢复到
 *      上一次一致状态。核心机制 = 写时复制(copy-on-write) + 元数据双块轮换。
 *   2. 磨损均衡（wear leveling）: 没有主动搬块，靠"每次提交写到另一块"的
 *      双块轮转自然分散写入。
 *
 * 本文件结构（自上而下，建议按此顺序读）：
 *   - 块设备缓存层:       lfs_cache_*（RAM 缓存，减少小 IO）
 *   - CRC 校验:           lfs_crc / lfs_dir_crc
 *   - 路径操作:           lfs_path_*
 *   - 元数据块对操作:     lfs_dir_*（双块轮换的提交/分裂/合并全在这）
 *   - 32位tag系统:        lfs_tag_*（littlefs v2 用 tag 索引一切对象）
 *   - 文件操作:           lfs_file_*（打开/读写/seek/同步）
 *   - 目录操作:           lfs_dir_*（用户可见的目录 API）
 *   - 文件系统操作:       lfs_mount/format/remove/stat...（对外 API）
 *   - 后台整理(GC):       lfs_fs_pred/successor/relocate/forceconsistency
 *
 * 对外 API 一览（都带 lfs_ 前缀，参数中文说明见 lfs.h）：
 *   挂载/格式化: lfs_mount / lfs_format
 *   文件:        lfs_file_open/close/read/write/seek/sync/tell/size
 *   目录:        lfs_mkdir/dir_open/dir_read/dir_remove/dir_close
 *   其他:        lfs_rename/lfs_stat/lfs_remove/lfs_truncate
 * ============================================================================
 */
/*
 * The little filesystem
 *
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "lfs.h"
#include "lfs_util.h"


// some constants used throughout the code
#define LFS_BLOCK_NULL ((lfs_block_t)-1)
#define LFS_BLOCK_INLINE ((lfs_block_t)-2)

enum {
    LFS_OK_RELOCATED = 1,
    LFS_OK_DROPPED   = 2,
    LFS_OK_ORPHANED  = 3,
};

enum {
    LFS_CMP_EQ = 0,
    LFS_CMP_LT = 1,
    LFS_CMP_GT = 2,
};


/// Caching block device operations ///

/**
 * @brief 作废一块读缓存（只把 block 标记为无效，不清 buffer 内容）
 * @param  lfs    文件系统对象（未使用，保留接口一致性）
 * @param  rcache 待作废的读缓存
 * @note   不清零 buffer 是有意的：若缓存只读、或即将写入相同数据
 *         （relocate 会把旧块原样搬进新块），保留旧内容反而省一次
 *         预读与 memset，这是 littlefs 常见的"惰性失效"手法。
 */
static inline void lfs_cache_drop(lfs_t *lfs, lfs_cache_t *rcache) {
    // do not zero, cheaper if cache is readonly or only going to be
    // written with identical data (during relocates)
    (void)lfs;
    rcache->block = LFS_BLOCK_NULL;
}

/**
 * @brief 清空一块程序缓存并标记无效
 * @note  填 0xff 而不是 0：0xff 是 NOR flash 擦除后的默认值，未写满
 *        的缓存尾字节与真实介质状态一致，同时避免把上一次文件的
 *        残留数据泄漏进新文件（信息泄漏防护）。
 */
static inline void lfs_cache_zero(lfs_t *lfs, lfs_cache_t *pcache) {
    // zero to avoid information leak
    memset(pcache->buffer, 0xff, lfs->cfg->cache_size);
    pcache->block = LFS_BLOCK_NULL;
}

/**
 * @brief 带双缓存的小块设备读：优先命中 pcache/rcache，未命中才落驱动
 * @param  pcache 程序缓存（可为 NULL）；其中未冲刷的数据比盘上新，读时以它为准
 * @param  rcache 读缓存；未命中时按 read_size 对齐整段预读填充
 * @param  hint   提示本调用之后还要连续读多少字节，用于预读窗口 sizing
 * @param  block/off/buffer/size 逻辑块号/块内偏移/目的缓冲/长度
 * @return 0 成功；负错误码（驱动错误，或地址越界返回 LFS_ERR_CORRUPT）
 * @note   littlefs 全部读路径（元数据、CTZ 文件数据）都经过这里，
 *         是"用 RAM 换小 IO"的核心；单次调用 off+size 恒不跨块。
 */
static int lfs_bd_read(lfs_t *lfs,
        const lfs_cache_t *pcache, lfs_cache_t *rcache, lfs_size_t hint,
        lfs_block_t block, lfs_off_t off,
        void *buffer, lfs_size_t size) {
    uint8_t *data = buffer;
    if (off+size > lfs->cfg->block_size
            || (lfs->block_count && block >= lfs->block_count)) {
        return LFS_ERR_CORRUPT;
    }

    while (size > 0) {
        lfs_size_t diff = size;

        if (pcache && block == pcache->block &&
                off < pcache->off + pcache->size) {
            if (off >= pcache->off) {
                // is already in pcache?
                diff = lfs_min(diff, pcache->size - (off-pcache->off));
                memcpy(data, &pcache->buffer[off-pcache->off], diff);

                data += diff;
                off += diff;
                size -= diff;
                continue;
            }

            // pcache takes priority
            diff = lfs_min(diff, pcache->off-off);
        }

        if (block == rcache->block &&
                off < rcache->off + rcache->size) {
            if (off >= rcache->off) {
                // is already in rcache?
                diff = lfs_min(diff, rcache->size - (off-rcache->off));
                memcpy(data, &rcache->buffer[off-rcache->off], diff);

                data += diff;
                off += diff;
                size -= diff;
                continue;
            }

            // rcache takes priority
            diff = lfs_min(diff, rcache->off-off);
        }

        // 大块连续读（size>=hint 且对齐 read_size）直接绕过缓存落到驱动，
        // 省一次 memcpy；hint 让顺序读的请求随消费速度逐渐"长大"
        if (size >= hint && off % lfs->cfg->read_size == 0 &&
                size >= lfs->cfg->read_size) {
            // bypass cache?
            diff = lfs_aligndown(diff, lfs->cfg->read_size);
            int err = lfs->cfg->read(lfs->cfg, block, off, data, diff);
            LFS_ASSERT(err <= 0);
            if (err) {
                return err;
            }

            data += diff;
            off += diff;
            size -= diff;
            continue;
        }

        // 预读窗口：起点向下对齐 read_size，终点按 off+hint 对齐后夹在
        // block_size 与 cache_size 之间——一次 IO 尽量喂饱后续同块访问
        // load to cache, first condition can no longer fail
        LFS_ASSERT(!lfs->block_count || block < lfs->block_count);
        rcache->block = block;
        rcache->off = lfs_aligndown(off, lfs->cfg->read_size);
        rcache->size = lfs_min(
                lfs_min(
                    lfs_alignup(off+hint, lfs->cfg->read_size),
                    lfs->cfg->block_size)
                - rcache->off,
                lfs->cfg->cache_size);
        int err = lfs->cfg->read(lfs->cfg, rcache->block,
                rcache->off, rcache->buffer, rcache->size);
        LFS_ASSERT(err <= 0);
        if (err) {
            return err;
        }
    }

    return 0;
}

/**
 * @brief 逐字节比较"盘上内容"与"RAM 内容"（经缓存读出）
 * @return LFS_CMP_EQ/LFS_CMP_LT/LFS_CMP_GT，或负错误码
 * @note   两个用途：1) 写后回读校验（lfs_bd_flush 的 validate），发现
 *         写入不粘即判坏块；2) lfs_dir_find_match 按文件名逐字节比对。
 */
static int lfs_bd_cmp(lfs_t *lfs,
        const lfs_cache_t *pcache, lfs_cache_t *rcache, lfs_size_t hint,
        lfs_block_t block, lfs_off_t off,
        const void *buffer, lfs_size_t size) {
    const uint8_t *data = buffer;
    lfs_size_t diff = 0;

    for (lfs_off_t i = 0; i < size; i += diff) {
        uint8_t dat[8];

        diff = lfs_min(size-i, sizeof(dat));
        int err = lfs_bd_read(lfs,
                pcache, rcache, hint-i,
                block, off+i, &dat, diff);
        if (err) {
            return err;
        }

        int res = memcmp(dat, data + i, diff);
        if (res) {
            return res < 0 ? LFS_CMP_LT : LFS_CMP_GT;
        }
    }

    return LFS_CMP_EQ;
}

/**
 * @brief 对盘上一段数据流式计算 CRC32（8 字节一段经缓存读出累加）
 * @note  用于提交收尾回读校验与 fetchmatch 边扫边校验；配合 lfs_crc
 *        实现 O(1) RAM 的校验，不整块缓冲。
 */
static int lfs_bd_crc(lfs_t *lfs,
        const lfs_cache_t *pcache, lfs_cache_t *rcache, lfs_size_t hint,
        lfs_block_t block, lfs_off_t off, lfs_size_t size, uint32_t *crc) {
    lfs_size_t diff = 0;

    for (lfs_off_t i = 0; i < size; i += diff) {
        uint8_t dat[8];
        diff = lfs_min(size-i, sizeof(dat));
        int err = lfs_bd_read(lfs,
                pcache, rcache, hint-i,
                block, off+i, &dat, diff);
        if (err) {
            return err;
        }

        *crc = lfs_crc(*crc, &dat, diff);
    }

    return 0;
}

#ifndef LFS_READONLY
/**
 * @brief 把 pcache 脏数据按 prog_size 对齐写出到驱动，可选回读校验
 * @param  validate 为 true 时写完立即读回逐字节比对，不一致返回
 *         LFS_ERR_CORRUPT——这是 littlefs 写路径上坏块的"第一现场"
 * @return 0 成功；负错误码
 */
static int lfs_bd_flush(lfs_t *lfs,
        lfs_cache_t *pcache, lfs_cache_t *rcache, bool validate) {
    if (pcache->block != LFS_BLOCK_NULL && pcache->block != LFS_BLOCK_INLINE) {
        LFS_ASSERT(pcache->block < lfs->block_count);
        lfs_size_t diff = lfs_alignup(pcache->size, lfs->cfg->prog_size);
        int err = lfs->cfg->prog(lfs->cfg, pcache->block,
                pcache->off, pcache->buffer, diff);
        LFS_ASSERT(err <= 0);
        if (err) {
            return err;
        }

        // 回读校验：写方手里本来就有 RAM 副本，所以一旦比对失败即可
        // 断定块坏了，调用方 goto relocate 换一块重试，无需 ECC
        if (validate) {
            // check data on disk
            lfs_cache_drop(lfs, rcache);
            int res = lfs_bd_cmp(lfs,
                    NULL, rcache, diff,
                    pcache->block, pcache->off, pcache->buffer, diff);
            if (res < 0) {
                return res;
            }

            if (res != LFS_CMP_EQ) {
                return LFS_ERR_CORRUPT;
            }
        }

        lfs_cache_zero(lfs, pcache);
    }

    return 0;
}
#endif

#ifndef LFS_READONLY
/**
 * @brief 冲刷 pcache 并调用驱动 sync，把数据真正落到介质
 * @note  掉电一致性窗口的物理边界就是它：元数据提交收尾与文件
 *        sync 前都会走到这里，sync 返回后本次写才视为"已完成"。
 */
static int lfs_bd_sync(lfs_t *lfs,
        lfs_cache_t *pcache, lfs_cache_t *rcache, bool validate) {
    lfs_cache_drop(lfs, rcache);

    int err = lfs_bd_flush(lfs, pcache, rcache, validate);
    if (err) {
        return err;
    }

    err = lfs->cfg->sync(lfs->cfg);
    LFS_ASSERT(err <= 0);
    return err;
}
#endif

#ifndef LFS_READONLY
/**
 * @brief 带缓存的块设备编程：先落 pcache，写满或换块时冲刷
 * @note  flash 只能把 1 写成 0 且必须先擦除；本函数不校验目标区
 *        是否处于擦除态（由调用方保证），validate=true 时冲刷阶段
 *        会回读校验。
 */
static int lfs_bd_prog(lfs_t *lfs,
        lfs_cache_t *pcache, lfs_cache_t *rcache, bool validate,
        lfs_block_t block, lfs_off_t off,
        const void *buffer, lfs_size_t size) {
    const uint8_t *data = buffer;
    LFS_ASSERT(block == LFS_BLOCK_INLINE || block < lfs->block_count);
    LFS_ASSERT(off + size <= lfs->cfg->block_size);

    while (size > 0) {
        if (block == pcache->block &&
                off >= pcache->off &&
                off < pcache->off + lfs->cfg->cache_size) {
            // already fits in pcache?
            lfs_size_t diff = lfs_min(size,
                    lfs->cfg->cache_size - (off-pcache->off));
            memcpy(&pcache->buffer[off-pcache->off], data, diff);

            data += diff;
            off += diff;
            size -= diff;

            pcache->size = lfs_max(pcache->size, off - pcache->off);
            // pcache 写满立即冲刷：后续块可复用同一块缓存，同时把
            // 坏块暴露的时机提前（而不是拖到 sync 才失败）
            if (pcache->size == lfs->cfg->cache_size) {
                // eagerly flush out pcache if we fill up
                int err = lfs_bd_flush(lfs, pcache, rcache, validate);
                if (err) {
                    return err;
                }
            }

            continue;
        }

        // pcache must have been flushed, either by programming an
        // entire block or manually flushing the pcache
        LFS_ASSERT(pcache->block == LFS_BLOCK_NULL);

        // prepare pcache, first condition can no longer fail
        pcache->block = block;
        pcache->off = lfs_aligndown(off, lfs->cfg->prog_size);
        pcache->size = 0;
    }

    return 0;
}
#endif

#ifndef LFS_READONLY
/**
 * @brief 擦除一个块（调用驱动 erase 回调），介质恢复为全 1 态
 * @note  compact / ctz_extend / file_relocate 分得新块后第一步都是
 *        它；返回 LFS_ERR_CORRUPT 时调用方换块重试（坏块回收路径）。
 */
static int lfs_bd_erase(lfs_t *lfs, lfs_block_t block) {
    LFS_ASSERT(block < lfs->block_count);
    int err = lfs->cfg->erase(lfs->cfg, block);
    LFS_ASSERT(err <= 0);
    return err;
}
#endif


/// Small type-level utilities ///

// some operations on paths
/** @brief 当前路径段的长度（到下一个 '/' 为止） */
static inline lfs_size_t lfs_path_namelen(const char *path) {
    return (lfs_size_t)strcspn(path, "/");
}

/**
 * @brief 名字之后是否只剩斜杠（即已是路径最后一段）
 * @note  lfs_dir_find 未命中时靠它区分"父目录在、末段不存在"（可创建）
 *        与"父目录不存在"（直接 NOENT）两种情形。
 */
static inline bool lfs_path_islast(const char *path) {
    lfs_size_t namelen = lfs_path_namelen(path);
    return path[namelen + strspn(path + namelen, "/")] == '\0';
}

/** @brief 名字后是否还有下一段（路径以 '/' 结尾按目录语义处理） */
static inline bool lfs_path_isdir(const char *path) {
    return path[lfs_path_namelen(path)] != '\0';
}

// operations on block pairs
/**
 * @brief 交换元数据对两块号的顺序
 * @note  约定 pair[0] 恒为"最新提交所在块"：compact 把新块写到
 *        pair[1] 再 swap 上位；fetchmatch 也把新块排到前面。
 */
static inline void lfs_pair_swap(lfs_block_t pair[2]) {
    lfs_block_t t = pair[0];
    pair[0] = pair[1];
    pair[1] = t;
}

/** @brief 对/尾指针是否为空（LFS_BLOCK_NULL=(lfs_block_t)-1；目录链终止条件） */
static inline bool lfs_pair_isnull(const lfs_block_t pair[2]) {
    return pair[0] == LFS_BLOCK_NULL || pair[1] == LFS_BLOCK_NULL;
}

/**
 * @brief 弱比较：两对中任一同位块号相等即认为同一对（返回 0）
 * @note  元数据对两块在翻转/搬迁后顺序会变，比较必须无视顺序；
 *        依赖块号全局唯一这一前提。
 */
static inline int lfs_pair_cmp(
        const lfs_block_t paira[2],
        const lfs_block_t pairb[2]) {
    return !(paira[0] == pairb[0] || paira[1] == pairb[1] ||
             paira[0] == pairb[1] || paira[1] == pairb[0]);
}

/**
 * @brief 严格比较：块号集合与顺序都一致
 * @note  deorphan 修复 half-orphan 时判断父目录指针是否已与链同步，
 *        不同步才需要写修复提交。
 */
static inline bool lfs_pair_issync(
        const lfs_block_t paira[2],
        const lfs_block_t pairb[2]) {
    return (paira[0] == pairb[0] && paira[1] == pairb[1]) ||
           (paira[0] == pairb[1] && paira[1] == pairb[0]);
}

/** @brief 元数据对块号：盘上小端 -> 主机序（读盘后调用） */
static inline void lfs_pair_fromle32(lfs_block_t pair[2]) {
    pair[0] = lfs_fromle32(pair[0]);
    pair[1] = lfs_fromle32(pair[1]);
}

#ifndef LFS_READONLY
/** @brief 元数据对块号：主机序 -> 盘上小端（写盘前调用） */
static inline void lfs_pair_tole32(lfs_block_t pair[2]) {
    pair[0] = lfs_tole32(pair[0]);
    pair[1] = lfs_tole32(pair[1]);
}
#endif

// operations on 32-bit entry tags
// littlefs v2 用 32 位 tag 索引元数据块里的一切对象（文件名、结构、
// 全局状态、CRC……）。布局：[valid 1 | type3 11 | id 10 | size 10]，
// type3 再分 type1(3bit 抽象类) + chunk(8bit)。盘上 tag 以大端存储，
// 且相邻 tag 互相 XOR（见 SPEC.md）；RAM 中统一使用 LFS_MKTAG 组装的
// "明文 tag"，读写盘时才做 XOR 与端序转换。valid 位在 RAM 明文中
// 恒为 0，最高位 0x80000000 另作他用（标记"数据在盘上"）。
typedef uint32_t lfs_tag_t;
typedef int32_t lfs_stag_t;

#define LFS_MKTAG(type, id, size) \
    (((lfs_tag_t)(type) << 20) | ((lfs_tag_t)(id) << 10) | (lfs_tag_t)(size))

#define LFS_MKTAG_IF(cond, type, id, size) \
    ((cond) ? LFS_MKTAG(type, id, size) : LFS_MKTAG(LFS_FROM_NOOP, 0, 0))

#define LFS_MKTAG_IF_ELSE(cond, type1, id1, size1, type2, id2, size2) \
    ((cond) ? LFS_MKTAG(type1, id1, size1) : LFS_MKTAG(type2, id2, size2))

/** @brief valid 位为 0 才有效（盘上 tag 首位存的是反相 valid 位） */
static inline bool lfs_tag_isvalid(lfs_tag_t tag) {
    return !(tag & 0x80000000);
}

/**
 * @brief size 字段全 1（0x3ff）=> 删除标记
 * @note  删除 tag 不携带数据；lfs_tag_dsize 里 tag+isdelete 的进位
 *        技巧把 size 还原为 0，一行覆盖两种情形。
 */
static inline bool lfs_tag_isdelete(lfs_tag_t tag) {
    return ((int32_t)(tag << 22) >> 22) == -1;
}

// 3bit 抽象类型掩码：NAME/STRUCT/USERATTR/SPLICE/TAIL/GLOBALS/CRC 分类查找用
static inline uint16_t lfs_tag_type1(lfs_tag_t tag) {
    return (tag & 0x70000000) >> 20;
}

// 4bit（type1+chunk 最高位）：CCRC/FCRC 等 CRC 族判定
static inline uint16_t lfs_tag_type2(lfs_tag_t tag) {
    return (tag & 0x78000000) >> 20;
}

// 完整 11bit 类型：精确匹配某个具体 tag（如 0x202 CTZSTRUCT）
static inline uint16_t lfs_tag_type3(lfs_tag_t tag) {
    return (tag & 0x7ff00000) >> 20;
}

// 8bit 子类型：文件类型(REG/DIR)、tail 类型、attr 类型、CRC 标志位等
static inline uint8_t lfs_tag_chunk(lfs_tag_t tag) {
    return (tag & 0x0ff00000) >> 20;
}

// CREATE/DELETE(SPLICE 族) 携带的 id 平移量：chunk 直接当有符号数
static inline int8_t lfs_tag_splice(lfs_tag_t tag) {
    return (int8_t)lfs_tag_chunk(tag);
}

// 所属文件 id；0x3ff 表示无 id 的全局条目（tail/gstate/crc）
static inline uint16_t lfs_tag_id(lfs_tag_t tag) {
    return (tag & 0x000ffc00) >> 10;
}

// 数据长度；0x3ff 是删除标记（见 lfs_tag_isdelete）
static inline lfs_size_t lfs_tag_size(lfs_tag_t tag) {
    return tag & 0x000003ff;
}

/**
 * @brief tag 在盘上的总占位：4 字节头 + 数据（删除记 0）
 * @note  tag+isdelete 的回绕：isdelete 时 size+1=0x400 截断回 0，
 *        避免写分支，是典型的 branchless 技巧。
 */
static inline lfs_size_t lfs_tag_dsize(lfs_tag_t tag) {
    return sizeof(tag) + lfs_tag_size(tag + lfs_tag_isdelete(tag));
}

// operations on attributes in attribute lists
// 提交时的"待写属性"：tag+buffer 成对，一次 commit 可写多条；
// LFS_MKATTRS(...) 用复合字面量把 {...} 语法糖展开成数组+个数
struct lfs_mattr {
    lfs_tag_t tag;
    const void *buffer;
};

// tag 数据在盘上的位置：数据可以不搬进 RAM，原地引用（move 场景）
struct lfs_diskoff {
    lfs_block_t block;
    lfs_off_t off;
};

#define LFS_MKATTRS(...) \
    (struct lfs_mattr[]){__VA_ARGS__}, \
    sizeof((struct lfs_mattr[]){__VA_ARGS__}) / sizeof(struct lfs_mattr)

// operations on global state
/**
 * @brief 全局状态 = 各元数据对上 delta 的 XOR 和（DESIGN.md "global state"）
 * @note  任何对提交 MOVESTATE 都能"翻转"全局状态；挂载时把链上全部
 *        gstate xor 起来即还原真实状态。这是跨元数据对原子性的基石。
 */
static inline void lfs_gstate_xor(lfs_gstate_t *a, const lfs_gstate_t *b) {
    a->tag ^= b->tag;
    a->pair[0] ^= b->pair[0];
    a->pair[1] ^= b->pair[1];
}

/** @brief 全 0 = 无待处理状态（无孤儿、无 move、超块已最新） */
static inline bool lfs_gstate_iszero(const lfs_gstate_t *a) {
    return a->tag == 0
            && a->pair[0] == 0
            && a->pair[1] == 0;
}

#ifndef LFS_READONLY
/** @brief size 字段低 9 位作孤儿计数（mkdir/remove 时 +1/-1 流水账） */
static inline bool lfs_gstate_hasorphans(const lfs_gstate_t *a) {
    return lfs_tag_size(a->tag);
}

/** @brief 取孤儿计数值（deorphan 收尾时 -n 一次清零） */
static inline uint8_t lfs_gstate_getorphans(const lfs_gstate_t *a) {
    return lfs_tag_size(a->tag) & 0x1ff;
}

/** @brief type1 非 0 => 有未完成 move（跨目录原子搬移的中间态） */
static inline bool lfs_gstate_hasmove(const lfs_gstate_t *a) {
    return lfs_tag_type1(a->tag);
}
#endif

/** @brief size bit9：挂载到旧次版本盘，首次写前需重写超块 */
static inline bool lfs_gstate_needssuperblock(const lfs_gstate_t *a) {
    return lfs_tag_size(a->tag) >> 9;
}

/** @brief move 源是否就是该元数据对（读路径也要"看见"未完成 move） */
static inline bool lfs_gstate_hasmovehere(const lfs_gstate_t *a,
        const lfs_block_t *pair) {
    return lfs_tag_type1(a->tag) && lfs_pair_cmp(a->pair, pair) == 0;
}

/** @brief gstate 三字段端序转换（MOVESTATE 在盘上按小端存） */
static inline void lfs_gstate_fromle32(lfs_gstate_t *a) {
    a->tag     = lfs_fromle32(a->tag);
    a->pair[0] = lfs_fromle32(a->pair[0]);
    a->pair[1] = lfs_fromle32(a->pair[1]);
}

#ifndef LFS_READONLY
/** @brief gstate 三字段端序转换（写盘方向） */
static inline void lfs_gstate_tole32(lfs_gstate_t *a) {
    a->tag     = lfs_tole32(a->tag);
    a->pair[0] = lfs_tole32(a->pair[0]);
    a->pair[1] = lfs_tole32(a->pair[1]);
}
#endif

// operations on forward-CRCs used to track erased state
/**
 * @brief FCRC 载荷：下一 program 窗口"擦除态"的校验（lfs2.1 新增）
 * @note  掉电可能把下一次提交写了一半，仅凭 valid 位无法确认其后
 *        字节仍是擦除态；FCRC 记录擦除态的 CRC，fetch 时比对即可
 *        判定"下一提交区域是否干净"，避免不必要的整块重擦。
 */
struct lfs_fcrc {
    lfs_size_t size;
    uint32_t crc;
};

/** @brief FCRC 载荷端序转换（读盘） */
static void lfs_fcrc_fromle32(struct lfs_fcrc *fcrc) {
    fcrc->size = lfs_fromle32(fcrc->size);
    fcrc->crc = lfs_fromle32(fcrc->crc);
}

#ifndef LFS_READONLY
/** @brief FCRC 载荷端序转换（写盘） */
static void lfs_fcrc_tole32(struct lfs_fcrc *fcrc) {
    fcrc->size = lfs_tole32(fcrc->size);
    fcrc->crc = lfs_tole32(fcrc->crc);
}
#endif

// other endianness operations
/** @brief CTZ 跳表头 {head 块号, 文件大小} 端序转换（读盘） */
static void lfs_ctz_fromle32(struct lfs_ctz *ctz) {
    ctz->head = lfs_fromle32(ctz->head);
    ctz->size = lfs_fromle32(ctz->size);
}

#ifndef LFS_READONLY
/** @brief CTZ 跳表头端序转换（写盘） */
static void lfs_ctz_tole32(struct lfs_ctz *ctz) {
    ctz->head = lfs_tole32(ctz->head);
    ctz->size = lfs_tole32(ctz->size);
}
#endif

/** @brief 超块配置字段端序转换（version/block_size/block_count/...） */
static inline void lfs_superblock_fromle32(lfs_superblock_t *superblock) {
    superblock->version     = lfs_fromle32(superblock->version);
    superblock->block_size  = lfs_fromle32(superblock->block_size);
    superblock->block_count = lfs_fromle32(superblock->block_count);
    superblock->name_max    = lfs_fromle32(superblock->name_max);
    superblock->file_max    = lfs_fromle32(superblock->file_max);
    superblock->attr_max    = lfs_fromle32(superblock->attr_max);
}

#ifndef LFS_READONLY
/** @brief 超块配置字段端序转换（写盘方向） */
static inline void lfs_superblock_tole32(lfs_superblock_t *superblock) {
    superblock->version     = lfs_tole32(superblock->version);
    superblock->block_size  = lfs_tole32(superblock->block_size);
    superblock->block_count = lfs_tole32(superblock->block_count);
    superblock->name_max    = lfs_tole32(superblock->name_max);
    superblock->file_max    = lfs_tole32(superblock->file_max);
    superblock->attr_max    = lfs_tole32(superblock->attr_max);
}
#endif

#ifndef LFS_NO_ASSERT
/**
 * @brief 调试断言用：file/dir 是否已在打开链表上（防重复打开/未开先关）
 */
static bool lfs_mlist_isopen(struct lfs_mlist *head,
        struct lfs_mlist *node) {
    for (struct lfs_mlist **p = &head; *p; p = &(*p)->next) {
        if (*p == (struct lfs_mlist*)node) {
            return true;
        }
    }

    return false;
}
#endif

/**
 * @brief 从 lfs->mlist（当前打开的 file/dir 链）摘除一个节点
 * @note  mlist 的意义：句柄打开期间，其它提交可能移动/删除同一条目，
 *        relocatingcommit 的 fixmlist 借此把受影响句柄的 id/pair 同步修正。
 */
static void lfs_mlist_remove(lfs_t *lfs, struct lfs_mlist *mlist) {
    for (struct lfs_mlist **p = &lfs->mlist; *p; p = &(*p)->next) {
        if (*p == mlist) {
            *p = (*p)->next;
            break;
        }
    }
}

/** @brief 头插进 mlist（O(1)）；打开 file/dir 时必须登记 */
static void lfs_mlist_append(lfs_t *lfs, struct lfs_mlist *mlist) {
    mlist->next = lfs->mlist;
    lfs->mlist = mlist;
}

// some other filesystem operations
/**
 * @brief 本次使用的盘上格式版本（LFS_MULTIVERSION 时可指定写旧版）
 */
static uint32_t lfs_fs_disk_version(lfs_t *lfs) {
    (void)lfs;
#ifdef LFS_MULTIVERSION
    if (lfs->cfg->disk_version) {
        return lfs->cfg->disk_version;
    } else
#endif
    {
        return LFS_DISK_VERSION;
    }
}

/** @brief 盘上版本主版本号（不兼容变更才递增） */
static uint16_t lfs_fs_disk_version_major(lfs_t *lfs) {
    return 0xffff & (lfs_fs_disk_version(lfs) >> 16);

}

/** @brief 盘上版本次版本号（可向后兼容） */
static uint16_t lfs_fs_disk_version_minor(lfs_t *lfs) {
    return 0xffff & (lfs_fs_disk_version(lfs) >> 0);
}


/// Internal operations predeclared here ///
#ifndef LFS_READONLY
static int lfs_dir_commit(lfs_t *lfs, lfs_mdir_t *dir,
        const struct lfs_mattr *attrs, int attrcount);
static int lfs_dir_compact(lfs_t *lfs,
        lfs_mdir_t *dir, const struct lfs_mattr *attrs, int attrcount,
        lfs_mdir_t *source, uint16_t begin, uint16_t end);
static lfs_ssize_t lfs_file_flushedwrite(lfs_t *lfs, lfs_file_t *file,
        const void *buffer, lfs_size_t size);
static lfs_ssize_t lfs_file_write_(lfs_t *lfs, lfs_file_t *file,
        const void *buffer, lfs_size_t size);
static int lfs_file_sync_(lfs_t *lfs, lfs_file_t *file);
static int lfs_file_outline(lfs_t *lfs, lfs_file_t *file);
static int lfs_file_flush(lfs_t *lfs, lfs_file_t *file);

static int lfs_fs_deorphan(lfs_t *lfs, bool powerloss);
static int lfs_fs_preporphans(lfs_t *lfs, int8_t orphans);
static void lfs_fs_prepmove(lfs_t *lfs,
        uint16_t id, const lfs_block_t pair[2]);
static int lfs_fs_pred(lfs_t *lfs, const lfs_block_t dir[2],
        lfs_mdir_t *pdir);
static lfs_stag_t lfs_fs_parent(lfs_t *lfs, const lfs_block_t dir[2],
        lfs_mdir_t *parent);
static int lfs_fs_forceconsistency(lfs_t *lfs);
#endif

static void lfs_fs_prepsuperblock(lfs_t *lfs, bool needssuperblock);

#ifdef LFS_MIGRATE
static int lfs1_traverse(lfs_t *lfs,
        int (*cb)(void*, lfs_block_t), void *data);
#endif

static int lfs_dir_rewind_(lfs_t *lfs, lfs_dir_t *dir);

static lfs_ssize_t lfs_file_flushedread(lfs_t *lfs, lfs_file_t *file,
        void *buffer, lfs_size_t size);
static lfs_ssize_t lfs_file_read_(lfs_t *lfs, lfs_file_t *file,
        void *buffer, lfs_size_t size);
static int lfs_file_close_(lfs_t *lfs, lfs_file_t *file);
static lfs_soff_t lfs_file_size_(lfs_t *lfs, lfs_file_t *file);

static lfs_ssize_t lfs_fs_size_(lfs_t *lfs);
static int lfs_fs_traverse_(lfs_t *lfs,
        int (*cb)(void *data, lfs_block_t block), void *data,
        bool includeorphans);

static int lfs_deinit(lfs_t *lfs);
static int lfs_unmount_(lfs_t *lfs);


/// Block allocator ///

// allocations should call this when all allocated blocks are committed to
// the filesystem
//
// after a checkpoint, the block allocator may realloc any untracked blocks
/**
 * @brief 记"无在途分配"检查点：ckpoint = 剩余可扫描块数
 * @note  一批分配开始前调用；当扫描覆盖了自检查点以来的全部块仍
 *        找不到空闲块，才能断定盘真的满了（而不是分配器视角暂缺）。
 */
static void lfs_alloc_ckpoint(lfs_t *lfs) {
    lfs->lookahead.ckpoint = lfs->block_count;
}

// drop the lookahead buffer, this is done during mounting and failed
// traversals in order to avoid invalid lookahead state
/**
 * @brief 清空 lookahead 窗口
 * @note  挂载后（起始点要按 seed 随机化）与全盘遍历失败时调用，
 *        防止拿过期的空闲位图继续分配。
 */
static void lfs_alloc_drop(lfs_t *lfs) {
    lfs->lookahead.size = 0;
    lfs->lookahead.next = 0;
    lfs_alloc_ckpoint(lfs);
}

#ifndef LFS_READONLY
/**
 * @brief lfs_fs_traverse_ 回调：把在用块标记进 lookahead 位图
 * @note  bit=1 表示占用；窗口外的块不标记，等下次窗口移动再覆盖。
 */
static int lfs_alloc_lookahead(void *p, lfs_block_t block) {
    lfs_t *lfs = (lfs_t*)p;
    lfs_block_t off = ((block - lfs->lookahead.start)
            + lfs->block_count) % lfs->block_count;

    if (off < lfs->lookahead.size) {
        lfs->lookahead.buffer[off / 8] |= 1U << (off % 8);
    }

    return 0;
}
#endif

#ifndef LFS_READONLY
/**
 * @brief 全盘扫描在用块，重建 lookahead 空闲窗口
 * @note  每次扫描窗口起点沿上次分配位置绕圈推进，叠加挂载时的
 *        随机起点，使分配位置在盘上均匀分布——这就是 littlefs 的
 *        动态磨损均衡（无静态搬块，纯概率摊平）。
 */
static int lfs_alloc_scan(lfs_t *lfs) {
    // move lookahead buffer to the first unused block
    //
    // note we limit the lookahead buffer to at most the amount of blocks
    // checkpointed, this prevents the math in lfs_alloc from underflowing
    // 窗口起点沿上次分配位置继续绕圈，避免每次都从块 0 开始
    // 造成低块号先磨损
    lfs->lookahead.start = (lfs->lookahead.start + lfs->lookahead.next) 
            % lfs->block_count;
    lfs->lookahead.next = 0;
    // 窗口上限受 ckpoint 钳制：已计入"剩余余量"的块不能重复计入，
    // 否则 lfs_alloc 的满盘判定会下溢
    lfs->lookahead.size = lfs_min(
            8*lfs->cfg->lookahead_size,
            lfs->lookahead.ckpoint);

    // find mask of free blocks from tree
    memset(lfs->lookahead.buffer, 0, lfs->cfg->lookahead_size);
    int err = lfs_fs_traverse_(lfs, lfs_alloc_lookahead, lfs, true);
    if (err) {
        lfs_alloc_drop(lfs);
        return err;
    }

    return 0;
}
#endif

#ifndef LFS_READONLY
/**
 * @brief 分配一个空闲块：先查 lookahead 位图，空了就全盘再扫
 * @param  block 输出分配到的块号
 * @return 0 成功；LFS_ERR_NOSPC 所有块都在用（含坏块耗尽）
 * @note   littlefs 不维护盘上空闲链表——"盘上无引用的块即空闲"，
 *         分配器像脚本语言的 GC 一样按需扫描；命中后还会顺带把
 *         next 推进到下一个空闲位，摊薄后续分配的循环开销。
 */
static int lfs_alloc(lfs_t *lfs, lfs_block_t *block) {
    while (true) {
        // scan our lookahead buffer for free blocks
        while (lfs->lookahead.next < lfs->lookahead.size) {
            if (!(lfs->lookahead.buffer[lfs->lookahead.next / 8]
                    & (1U << (lfs->lookahead.next % 8)))) {
                // found a free block
                *block = (lfs->lookahead.start + lfs->lookahead.next)
                        % lfs->block_count;

                // 命中后继续快进到下一个空闲位：下次分配 O(1)，
                // 同时消耗 ckpoint 余量
                // eagerly find next free block to maximize how many blocks
                // lfs_alloc_ckpoint makes available for scanning
                while (true) {
                    lfs->lookahead.next += 1;
                    lfs->lookahead.ckpoint -= 1;

                    if (lfs->lookahead.next >= lfs->lookahead.size
                            || !(lfs->lookahead.buffer[lfs->lookahead.next / 8]
                                & (1U << (lfs->lookahead.next % 8)))) {
                        return 0;
                    }
                }
            }

            lfs->lookahead.next += 1;
            lfs->lookahead.ckpoint -= 1;
        }

        // In order to keep our block allocator from spinning forever when our
        // filesystem is full, we mark points where there are no in-flight
        // allocations with a checkpoint before starting a set of allocations.
        //
        // If we've looked at all blocks since the last checkpoint, we report
        // the filesystem as out of storage.
        //
        // 自上次 ckpoint 起已看遍所有块仍无空闲 => 真满盘（含坏块），
        // 返回 NOSPC 而不是死循环；这是满盘判定的唯一出口
        if (lfs->lookahead.ckpoint <= 0) {
            LFS_ERROR("No more free space 0x%"PRIx32,
                    (lfs->lookahead.start + lfs->lookahead.next)
                        % lfs->block_count);
            return LFS_ERR_NOSPC;
        }

        // No blocks in our lookahead buffer, we need to scan the filesystem for
        // unused blocks in the next lookahead window.
        int err = lfs_alloc_scan(lfs);
        if(err) {
            return err;
        }
    }
}
#endif

/// Metadata pair and directory operations ///
/**
 * @brief 在元数据块上反向扫描 tag 链，找到 gmask/gtag 匹配的最新条目
 * @param  gmask  匹配掩码（哪些位参与比较）
 * @param  gtag   期望 tag（id 字段为要查的文件 id）
 * @param  goff/gbuffer/gsize 数据切片偏移/缓冲/长度（支持分片读）
 * @return 命中的 tag（id 已按 splice 修正）；LFS_ERR_NOENT 未命中
 * @note   从 dir->off 往回走（反向迭代）：最新提交在日志尾部，命中
 *         第一个即最新版本；gdiff 累计 CREATE/DELETE 对 id 的搬移，
 *         把"查询时刻的 id"映射回"当前日志里的 id"。
 */
static lfs_stag_t lfs_dir_getslice(lfs_t *lfs, const lfs_mdir_t *dir,
        lfs_tag_t gmask, lfs_tag_t gtag,
        lfs_off_t goff, void *gbuffer, lfs_size_t gsize) {
    lfs_off_t off = dir->off;
    lfs_tag_t ntag = dir->etag;
    lfs_stag_t gdiff = 0;

    // 合成 move：gdisk 记录挂载时发现的未完成 move，读路径要让源
    // 条目"看起来已被移走"（id 相同=>NOENT，更大=>平移 -1）
    // synthetic moves
    if (lfs_gstate_hasmovehere(&lfs->gdisk, dir->pair) &&
            lfs_tag_id(gmask) != 0) {
        if (lfs_tag_id(lfs->gdisk.tag) == lfs_tag_id(gtag)) {
            return LFS_ERR_NOENT;
        } else if (lfs_tag_id(lfs->gdisk.tag) < lfs_tag_id(gtag)) {
            gdiff -= LFS_MKTAG(0, 1, 0);
        }
    }

    // 从日志尾部向前扫：第一个匹配即最新版本，平均省一半扫描
    // iterate over dir block backwards (for faster lookups)
    while (off >= sizeof(lfs_tag_t) + lfs_tag_dsize(ntag)) {
        off -= lfs_tag_dsize(ntag);
        lfs_tag_t tag = ntag;
        int err = lfs_bd_read(lfs,
                NULL, &lfs->rcache, sizeof(ntag),
                dir->pair[0], off, &ntag, sizeof(ntag));
        LFS_ASSERT(err <= 0);
        if (err) {
            return err;
        }

    // 盘上 tag 为大端且与前一 tag XOR 成链：这里还原明文；
    // &0x7fffffff 把 valid 位归一（valid 语义由调用方按约定处理）
        ntag = (lfs_frombe32(ntag) ^ tag) & 0x7fffffff;

        if (lfs_tag_id(gmask) != 0 &&
                lfs_tag_type1(tag) == LFS_TYPE_SPLICE &&
                lfs_tag_id(tag) <= lfs_tag_id(gtag - gdiff)) {
            if (tag == (LFS_MKTAG(LFS_TYPE_CREATE, 0, 0) |
                    (LFS_MKTAG(0, 0x3ff, 0) & (gtag - gdiff)))) {
                // found where we were created
                return LFS_ERR_NOENT;
            }

            // move around splices
            gdiff += LFS_MKTAG(0, lfs_tag_splice(tag), 0);
        }

        if ((gmask & tag) == (gmask & (gtag - gdiff))) {
            if (lfs_tag_isdelete(tag)) {
                return LFS_ERR_NOENT;
            }

            lfs_size_t diff = lfs_min(lfs_tag_size(tag), gsize);
            err = lfs_bd_read(lfs,
                    NULL, &lfs->rcache, diff,
                    dir->pair[0], off+sizeof(tag)+goff, gbuffer, diff);
            LFS_ASSERT(err <= 0);
            if (err) {
                return err;
            }

            memset((uint8_t*)gbuffer + diff, 0, gsize - diff);

            return tag + gdiff;
        }
    }

    return LFS_ERR_NOENT;
}

/** @brief lfs_dir_getslice 的整段读封装（goff=0，长度=tag.size） */
static lfs_stag_t lfs_dir_get(lfs_t *lfs, const lfs_mdir_t *dir,
        lfs_tag_t gmask, lfs_tag_t gtag, void *buffer) {
    return lfs_dir_getslice(lfs, dir,
            gmask, gtag,
            0, buffer, lfs_tag_size(gtag));
}

/**
 * @brief 把 inline 文件（数据嵌在元数据对里的文件）当块设备来读
 * @note  inline 文件没有独立块号，读它=按 INLINESTRUCT tag 从元数据
 *        对里切片；用 LFS_BLOCK_INLINE 哨兵复用普通缓存的命中逻辑。
 */
static int lfs_dir_getread(lfs_t *lfs, const lfs_mdir_t *dir,
        const lfs_cache_t *pcache, lfs_cache_t *rcache, lfs_size_t hint,
        lfs_tag_t gmask, lfs_tag_t gtag,
        lfs_off_t off, void *buffer, lfs_size_t size) {
    uint8_t *data = buffer;
    if (off+size > lfs->cfg->block_size) {
        return LFS_ERR_CORRUPT;
    }

    while (size > 0) {
        lfs_size_t diff = size;

        if (pcache && pcache->block == LFS_BLOCK_INLINE &&
                off < pcache->off + pcache->size) {
            if (off >= pcache->off) {
                // is already in pcache?
                diff = lfs_min(diff, pcache->size - (off-pcache->off));
                memcpy(data, &pcache->buffer[off-pcache->off], diff);

                data += diff;
                off += diff;
                size -= diff;
                continue;
            }

            // pcache takes priority
            diff = lfs_min(diff, pcache->off-off);
        }

        if (rcache->block == LFS_BLOCK_INLINE &&
                off < rcache->off + rcache->size) {
            if (off >= rcache->off) {
                // is already in rcache?
                diff = lfs_min(diff, rcache->size - (off-rcache->off));
                memcpy(data, &rcache->buffer[off-rcache->off], diff);

                data += diff;
                off += diff;
                size -= diff;
                continue;
            }
        }

        // load to cache, first condition can no longer fail
        rcache->block = LFS_BLOCK_INLINE;
        rcache->off = lfs_aligndown(off, lfs->cfg->read_size);
        rcache->size = lfs_min(lfs_alignup(off+hint, lfs->cfg->read_size),
                lfs->cfg->cache_size);
        int err = lfs_dir_getslice(lfs, dir, gmask, gtag,
                rcache->off, rcache->buffer, rcache->size);
        if (err < 0) {
            return err;
        }
    }

    return 0;
}

#ifndef LFS_READONLY
/**
 * @brief traverse 的过滤器回调：剔除将被覆盖的旧 tag
 * @note  compact 挑"最新版本"的核心：同 id 同类型的旧 tag 被丢弃；
 *        DELETE 之后没有更新版本的条目整条消失。命中即返回 true
 *        停止下钻（filtertag 置 NOOP 使后续全部通过）。
 */
static int lfs_dir_traverse_filter(void *p,
        lfs_tag_t tag, const void *buffer) {
    lfs_tag_t *filtertag = p;
    (void)buffer;

    // which mask depends on unique bit in tag structure
    uint32_t mask = (tag & LFS_MKTAG(0x100, 0, 0))
            ? LFS_MKTAG(0x7ff, 0x3ff, 0)
            : LFS_MKTAG(0x700, 0x3ff, 0);

    // check for redundancy
    if ((mask & tag) == (mask & *filtertag) ||
            lfs_tag_isdelete(*filtertag) ||
            (LFS_MKTAG(0x7ff, 0x3ff, 0) & tag) == (
                LFS_MKTAG(LFS_TYPE_DELETE, 0, 0) |
                    (LFS_MKTAG(0, 0x3ff, 0) & *filtertag))) {
        *filtertag = LFS_MKTAG(LFS_FROM_NOOP, 0, 0);
        return true;
    }

    // check if we need to adjust for created/deleted tags
    if (lfs_tag_type1(tag) == LFS_TYPE_SPLICE &&
            lfs_tag_id(tag) <= lfs_tag_id(*filtertag)) {
        *filtertag += LFS_MKTAG(0, lfs_tag_splice(tag), 0);
    }

    return false;
}
#endif

#ifndef LFS_READONLY
// maximum recursive depth of lfs_dir_traverse, the deepest call:
//
// traverse with commit
// '-> traverse with move
//     '-> traverse with filter
//
#define LFS_DIR_TRAVERSE_DEPTH 3

struct lfs_dir_traverse {
    const lfs_mdir_t *dir;
    lfs_off_t off;
    lfs_tag_t ptag;
    const struct lfs_mattr *attrs;
    int attrcount;

    lfs_tag_t tmask;
    lfs_tag_t ttag;
    uint16_t begin;
    uint16_t end;
    int16_t diff;

    int (*cb)(void *data, lfs_tag_t tag, const void *buffer);
    void *data;

    lfs_tag_t tag;
    const void *buffer;
    struct lfs_diskoff disk;
};

/**
 * @brief 元数据提交的统一迭代器：串起盘上旧提交、待写 attrs、move 源
 * @param  tmask/ttag 只处理 (tmask&tag)==(tmask&ttag) 的 tag（0 表示全收）
 * @param  begin/end  id 过滤窗口 [begin,end)；diff 为 id 的整体平移
 * @param  cb/data    每个命中 tag 的回调（写 commit / 统计大小）
 * @return 0 遍历完成；回调返回正数提前终止；负错误码
 * @note   逻辑上是三层递归（commit→filter→move），这里用显式栈手工
 *         展开成 O(1) 栈深，保证 RAM 有界——littlefs 禁止无界递归。
 *         FROM_MOVE 分支"跳"进源目录，把待搬 id 的 tag 一并收割，
 *         实现 rename/move 的原子提交。
 */
static int lfs_dir_traverse(lfs_t *lfs,
        const lfs_mdir_t *dir, lfs_off_t off, lfs_tag_t ptag,
        const struct lfs_mattr *attrs, int attrcount,
        lfs_tag_t tmask, lfs_tag_t ttag,
        uint16_t begin, uint16_t end, int16_t diff,
        int (*cb)(void *data, lfs_tag_t tag, const void *buffer), void *data) {
    // This function in inherently recursive, but bounded. To allow tool-based
    // analysis without unnecessary code-cost we use an explicit stack
    struct lfs_dir_traverse stack[LFS_DIR_TRAVERSE_DEPTH-1];
    unsigned sp = 0;
    int res;

    // iterate over directory and attrs
    lfs_tag_t tag;
    const void *buffer;
    struct lfs_diskoff disk = {0};
    while (true) {
        {
            if (off+lfs_tag_dsize(ptag) < dir->off) {
                off += lfs_tag_dsize(ptag);
                int err = lfs_bd_read(lfs,
                        NULL, &lfs->rcache, sizeof(tag),
                        dir->pair[0], off, &tag, sizeof(tag));
                if (err) {
                    return err;
                }

                tag = (lfs_frombe32(tag) ^ ptag) | 0x80000000;
                disk.block = dir->pair[0];
                disk.off = off+sizeof(lfs_tag_t);
                buffer = &disk;
                ptag = tag;
            } else if (attrcount > 0) {
                tag = attrs[0].tag;
                buffer = attrs[0].buffer;
                attrs += 1;
                attrcount -= 1;
            } else {
                // finished traversal, pop from stack?
                res = 0;
                break;
            }

            // do we need to filter?
            lfs_tag_t mask = LFS_MKTAG(0x7ff, 0, 0);
            if ((mask & tmask & tag) != (mask & tmask & ttag)) {
                continue;
            }

            if (lfs_tag_id(tmask) != 0) {
                LFS_ASSERT(sp < LFS_DIR_TRAVERSE_DEPTH);
                // recurse, scan for duplicates, and update tag based on
                // creates/deletes
                stack[sp] = (struct lfs_dir_traverse){
                    .dir        = dir,
                    .off        = off,
                    .ptag       = ptag,
                    .attrs      = attrs,
                    .attrcount  = attrcount,
                    .tmask      = tmask,
                    .ttag       = ttag,
                    .begin      = begin,
                    .end        = end,
                    .diff       = diff,
                    .cb         = cb,
                    .data       = data,
                    .tag        = tag,
                    .buffer     = buffer,
                    .disk       = disk,
                };
                sp += 1;

                tmask = 0;
                ttag = 0;
                begin = 0;
                end = 0;
                diff = 0;
                cb = lfs_dir_traverse_filter;
                data = &stack[sp-1].tag;
                continue;
            }
        }

popped:
        // in filter range?
        if (lfs_tag_id(tmask) != 0 &&
                !(lfs_tag_id(tag) >= begin && lfs_tag_id(tag) < end)) {
            continue;
        }

        // handle special cases for mcu-side operations
        if (lfs_tag_type3(tag) == LFS_FROM_NOOP) {
            // do nothing
        } else if (lfs_tag_type3(tag) == LFS_FROM_MOVE) {
            // Without this condition, lfs_dir_traverse can exhibit an
            // extremely expensive O(n^3) of nested loops when renaming.
            // This happens because lfs_dir_traverse tries to filter tags by
            // the tags in the source directory, triggering a second
            // lfs_dir_traverse with its own filter operation.
            //
            // traverse with commit
            // '-> traverse with filter
            //     '-> traverse with move
            //         '-> traverse with filter
            //
            // However we don't actually care about filtering the second set of
            // tags, since duplicate tags have no effect when filtering.
            //
            // This check skips this unnecessary recursive filtering explicitly,
            // reducing this runtime from O(n^3) to O(n^2).
            if (cb == lfs_dir_traverse_filter) {
                continue;
            }

            // recurse into move
            stack[sp] = (struct lfs_dir_traverse){
                .dir        = dir,
                .off        = off,
                .ptag       = ptag,
                .attrs      = attrs,
                .attrcount  = attrcount,
                .tmask      = tmask,
                .ttag       = ttag,
                .begin      = begin,
                .end        = end,
                .diff       = diff,
                .cb         = cb,
                .data       = data,
                .tag        = LFS_MKTAG(LFS_FROM_NOOP, 0, 0),
            };
            sp += 1;

            uint16_t fromid = lfs_tag_size(tag);
            uint16_t toid = lfs_tag_id(tag);
            dir = buffer;
            off = 0;
            ptag = 0xffffffff;
            attrs = NULL;
            attrcount = 0;
            tmask = LFS_MKTAG(0x600, 0x3ff, 0);
            ttag = LFS_MKTAG(LFS_TYPE_STRUCT, 0, 0);
            begin = fromid;
            end = fromid+1;
            diff = toid-fromid+diff;
        } else if (lfs_tag_type3(tag) == LFS_FROM_USERATTRS) {
            for (unsigned i = 0; i < lfs_tag_size(tag); i++) {
                const struct lfs_attr *a = buffer;
                res = cb(data, LFS_MKTAG(LFS_TYPE_USERATTR + a[i].type,
                        lfs_tag_id(tag) + diff, a[i].size), a[i].buffer);
                if (res < 0) {
                    return res;
                }

                if (res) {
                    break;
                }
            }
        } else {
            res = cb(data, tag + LFS_MKTAG(0, diff, 0), buffer);
            if (res < 0) {
                return res;
            }

            if (res) {
                break;
            }
        }
    }

    if (sp > 0) {
        // pop from the stack and return, fortunately all pops share
        // a destination
        dir         = stack[sp-1].dir;
        off         = stack[sp-1].off;
        ptag        = stack[sp-1].ptag;
        attrs       = stack[sp-1].attrs;
        attrcount   = stack[sp-1].attrcount;
        tmask       = stack[sp-1].tmask;
        ttag        = stack[sp-1].ttag;
        begin       = stack[sp-1].begin;
        end         = stack[sp-1].end;
        diff        = stack[sp-1].diff;
        cb          = stack[sp-1].cb;
        data        = stack[sp-1].data;
        tag         = stack[sp-1].tag;
        buffer      = stack[sp-1].buffer;
        disk        = stack[sp-1].disk;
        sp -= 1;
        goto popped;
    } else {
        return res;
    }
}
#endif

/**
 * @brief 元数据对读取核心：选最新块、扫全部提交、顺带匹配目标 tag
 * @param  fmask/ftag 匹配掩码与目标（(lfs_tag_t)-1 表示纯 fetch 不匹配）
 * @param  cb/data    匹配回调（如按名字逐字节比对），返回 LFS_CMP_*
 * @param  id         输出：目标所在的 id（min(bestid,count)，供插入用）
 * @return 命中 tag；0=条目不存在但 id 合法；LFS_ERR_NOENT id 越界；
 *         LFS_ERR_CORRUPT 两块都读不出有效提交
 * @note   双块裁决靠 revision count 的序列比较（lfs_scmp 处理回绕）；
 *         每条提交边扫边 CRC，遇 CRC tag 时校验，并把 chunk 最低位
 *         折进下一提交的 valid 位——"半写提交被识别"的全部机制在此。
 */
static lfs_stag_t lfs_dir_fetchmatch(lfs_t *lfs,
        lfs_mdir_t *dir, const lfs_block_t pair[2],
        lfs_tag_t fmask, lfs_tag_t ftag, uint16_t *id,
        int (*cb)(void *data, lfs_tag_t tag, const void *buffer), void *data) {
    // we can find tag very efficiently during a fetch, since we're already
    // scanning the entire directory
    lfs_stag_t besttag = -1;

    // if either block address is invalid we return LFS_ERR_CORRUPT here,
    // otherwise later writes to the pair could fail
    if (lfs->block_count 
            && (pair[0] >= lfs->block_count || pair[1] >= lfs->block_count)) {
        return LFS_ERR_CORRUPT;
    }

    // find the block with the most recent revision
    uint32_t revs[2] = {0, 0};
    int r = 0;
    for (int i = 0; i < 2; i++) {
        int err = lfs_bd_read(lfs,
                NULL, &lfs->rcache, sizeof(revs[i]),
                pair[i], 0, &revs[i], sizeof(revs[i]));
        revs[i] = lfs_fromle32(revs[i]);
        if (err && err != LFS_ERR_CORRUPT) {
            return err;
        }

        // 序列号比较：rev 回绕时仍能判出新旧（RFC1982 语义），
        // rev 大者排到 pair[0]
        if (err != LFS_ERR_CORRUPT &&
                lfs_scmp(revs[i], revs[(i+1)%2]) > 0) {
            r = i;
        }
    }

    dir->pair[0] = pair[(r+0)%2];
    dir->pair[1] = pair[(r+1)%2];
    dir->rev = revs[(r+0)%2];
    dir->off = 0; // nonzero = found some commits

    // now scan tags to fetch the actual dir and find possible match
    for (int i = 0; i < 2; i++) {
        lfs_off_t off = 0;
        lfs_tag_t ptag = 0xffffffff;

        uint16_t tempcount = 0;
        lfs_block_t temptail[2] = {LFS_BLOCK_NULL, LFS_BLOCK_NULL};
        bool tempsplit = false;
        lfs_stag_t tempbesttag = besttag;

        // assume not erased until proven otherwise
        bool maybeerased = false;
        bool hasfcrc = false;
        struct lfs_fcrc fcrc;

        dir->rev = lfs_tole32(dir->rev);
        uint32_t crc = lfs_crc(0xffffffff, &dir->rev, sizeof(dir->rev));
        dir->rev = lfs_fromle32(dir->rev);

        while (true) {
            // extract next tag
            lfs_tag_t tag;
            off += lfs_tag_dsize(ptag);
            int err = lfs_bd_read(lfs,
                    NULL, &lfs->rcache, lfs->cfg->block_size,
                    dir->pair[0], off, &tag, sizeof(tag));
            if (err) {
                if (err == LFS_ERR_CORRUPT) {
                    // can't continue?
                    break;
                }
                return err;
            }

            crc = lfs_crc(crc, &tag, sizeof(tag));
            tag = lfs_frombe32(tag) ^ ptag;

            // next commit not yet programmed?
            if (!lfs_tag_isvalid(tag)) {
                // we only might be erased if the last tag was a crc
                maybeerased = (lfs_tag_type2(ptag) == LFS_TYPE_CCRC);
                break;
            // out of range?
            } else if (off + lfs_tag_dsize(tag) > lfs->cfg->block_size) {
                break;
            }

            ptag = tag;

            if (lfs_tag_type2(tag) == LFS_TYPE_CCRC) {
                // check the crc attr
                uint32_t dcrc;
                err = lfs_bd_read(lfs,
                        NULL, &lfs->rcache, lfs->cfg->block_size,
                        dir->pair[0], off+sizeof(tag), &dcrc, sizeof(dcrc));
                if (err) {
                    if (err == LFS_ERR_CORRUPT) {
                        break;
                    }
                    return err;
                }
                dcrc = lfs_fromle32(dcrc);

                if (crc != dcrc) {
                    break;
                }

                // CRC tag 的 chunk bit0 约定下一提交 valid 位应取反：
                // 未写区域全 0xff 解出的 tag valid 位必然不符 => 判无效，
                // 半写提交到此被拦下（配合 FCRC 更精确，见下）
                // reset the next bit if we need to
                ptag ^= (lfs_tag_t)(lfs_tag_chunk(tag) & 1U) << 31;

                // 把每个提交的 CRC 摘进 seed：这是跨掉电稳定的"熵源"，
                // 挂载后据此随机化分配起点（动态磨损均衡的种子）
                // toss our crc into the filesystem seed for
                // pseudorandom numbers, note we use another crc here
                // as a collection function because it is sufficiently
                // random and convenient
                lfs->seed = lfs_crc(lfs->seed, &crc, sizeof(crc));

                // update with what's found so far
                besttag = tempbesttag;
                dir->off = off + lfs_tag_dsize(tag);
                dir->etag = ptag;
                dir->count = tempcount;
                dir->tail[0] = temptail[0];
                dir->tail[1] = temptail[1];
                dir->split = tempsplit;

                // reset crc, hasfcrc
                crc = 0xffffffff;
                continue;
            }

            // crc the entry first, hopefully leaving it in the cache
            err = lfs_bd_crc(lfs,
                    NULL, &lfs->rcache, lfs->cfg->block_size,
                    dir->pair[0], off+sizeof(tag),
                    lfs_tag_dsize(tag)-sizeof(tag), &crc);
            if (err) {
                if (err == LFS_ERR_CORRUPT) {
                    break;
                }
                return err;
            }

            // directory modification tags?
            if (lfs_tag_type1(tag) == LFS_TYPE_NAME) {
                // increase count of files if necessary
                if (lfs_tag_id(tag) >= tempcount) {
                    tempcount = lfs_tag_id(tag) + 1;
                }
            } else if (lfs_tag_type1(tag) == LFS_TYPE_SPLICE) {
                tempcount += lfs_tag_splice(tag);

                if (tag == (LFS_MKTAG(LFS_TYPE_DELETE, 0, 0) |
                        (LFS_MKTAG(0, 0x3ff, 0) & tempbesttag))) {
                    tempbesttag |= 0x80000000;
                } else if (tempbesttag != -1 &&
                        lfs_tag_id(tag) <= lfs_tag_id(tempbesttag)) {
                    tempbesttag += LFS_MKTAG(0, lfs_tag_splice(tag), 0);
                }
            } else if (lfs_tag_type1(tag) == LFS_TYPE_TAIL) {
                tempsplit = (lfs_tag_chunk(tag) & 1);

                err = lfs_bd_read(lfs,
                        NULL, &lfs->rcache, lfs->cfg->block_size,
                        dir->pair[0], off+sizeof(tag), &temptail, 8);
                if (err) {
                    if (err == LFS_ERR_CORRUPT) {
                        break;
                    }
                    return err;
                }
                lfs_pair_fromle32(temptail);
            } else if (lfs_tag_type3(tag) == LFS_TYPE_FCRC) {
                err = lfs_bd_read(lfs,
                        NULL, &lfs->rcache, lfs->cfg->block_size,
                        dir->pair[0], off+sizeof(tag),
                        &fcrc, sizeof(fcrc));
                if (err) {
                    if (err == LFS_ERR_CORRUPT) {
                        break;
                    }
                    return err;
                }

                lfs_fcrc_fromle32(&fcrc);
                hasfcrc = true;
            }

            // found a match for our fetcher?
            if ((fmask & tag) == (fmask & ftag)) {
                LFS_ASSERT(cb != NULL);
                int res = cb(data, tag, &(struct lfs_diskoff){
                        dir->pair[0], off+sizeof(tag)});
                if (res < 0) {
                    if (res == LFS_ERR_CORRUPT) {
                        break;
                    }
                    return res;
                }

                if (res == LFS_CMP_EQ) {
                    // found a match
                    tempbesttag = tag;
                } else if ((LFS_MKTAG(0x7ff, 0x3ff, 0) & tag) ==
                        (LFS_MKTAG(0x7ff, 0x3ff, 0) & tempbesttag)) {
                    // found an identical tag, but contents didn't match
                    // this must mean that our besttag has been overwritten
                    tempbesttag = -1;
                } else if (res == LFS_CMP_GT &&
                        lfs_tag_id(tag) <= lfs_tag_id(tempbesttag)) {
                    // found a greater match, keep track to keep things sorted
                    tempbesttag = tag | 0x80000000;
                }
            }
        }

        // found no valid commits?
        if (dir->off == 0) {
            // try the other block?
            lfs_pair_swap(dir->pair);
            dir->rev = revs[(r+1)%2];
            continue;
        }

        // 尾提交完整且 off 对齐 prog_size 才可能"仍处于擦除态"；
        // lfs2.1 用 FCRC 复核下一窗口，lfs2.0 盘无 FCRC 直接视为已擦除
        //（否则对 2.0 盘的追加写会退化成每次都重擦）
        // did we end on a valid commit? we may have an erased block
        dir->erased = false;
        if (maybeerased && dir->off % lfs->cfg->prog_size == 0) {
        #ifdef LFS_MULTIVERSION
            // note versions < lfs2.1 did not have fcrc tags, if
            // we're < lfs2.1 treat missing fcrc as erased data
            //
            // we don't strictly need to do this, but otherwise writing
            // to lfs2.0 disks becomes very inefficient
            if (lfs_fs_disk_version(lfs) < 0x00020001) {
                dir->erased = true;

            } else
        #endif
            if (hasfcrc) {
                // check for an fcrc matching the next prog's erased state, if
                // this failed most likely a previous prog was interrupted, we
                // need a new erase
                uint32_t fcrc_ = 0xffffffff;
                int err = lfs_bd_crc(lfs,
                        NULL, &lfs->rcache, lfs->cfg->block_size,
                        dir->pair[0], dir->off, fcrc.size, &fcrc_);
                if (err && err != LFS_ERR_CORRUPT) {
                    return err;
                }

                // found beginning of erased part?
                dir->erased = (fcrc_ == fcrc.crc);
            }
        }

        // 合成 move：gdisk 的 move 源在本对时按 move id 修正查询结果
        //（与 getslice 开头的处理对偶，覆盖"扫全块"这条路径）
        // synthetic move
        if (lfs_gstate_hasmovehere(&lfs->gdisk, dir->pair)) {
            if (lfs_tag_id(lfs->gdisk.tag) == lfs_tag_id(besttag)) {
                besttag |= 0x80000000;
            } else if (besttag != -1 &&
                    lfs_tag_id(lfs->gdisk.tag) < lfs_tag_id(besttag)) {
                besttag -= LFS_MKTAG(0, 1, 0);
            }
        }

        // found tag? or found best id?
        if (id) {
            *id = lfs_min(lfs_tag_id(besttag), dir->count);
        }

        if (lfs_tag_isvalid(besttag)) {
            return besttag;
        } else if (lfs_tag_id(besttag) < dir->count) {
            return LFS_ERR_NOENT;
        } else {
            return 0;
        }
    }

    LFS_ERROR("Corrupted dir pair at {0x%"PRIx32", 0x%"PRIx32"}",
            dir->pair[0], dir->pair[1]);
    return LFS_ERR_CORRUPT;
}

/**
 * @brief 把元数据对刷新到最新状态（不匹配任何特定 tag）
 * @note  mask/tag 全 1 因 valid 位恒为 1 而永不可能命中，
 *        巧妙复用 fetchmatch 做纯 fetch。
 */
static int lfs_dir_fetch(lfs_t *lfs,
        lfs_mdir_t *dir, const lfs_block_t pair[2]) {
    // note, mask=-1, tag=-1 can never match a tag since this
    // pattern has the invalid bit set
    return (int)lfs_dir_fetchmatch(lfs, dir, pair,
            (lfs_tag_t)-1, (lfs_tag_t)-1, NULL, NULL, NULL);
}

/**
 * @brief 读出该对携带的 MOVESTATE delta 并 xor 进 gstate
 * @note  挂载时对链上每对调用（聚合出全局状态）；compact 时也用它
 *        把 delta 搬到新块——delta 不能丢，否则 move/孤儿状态丢失。
 */
static int lfs_dir_getgstate(lfs_t *lfs, const lfs_mdir_t *dir,
        lfs_gstate_t *gstate) {
    lfs_gstate_t temp;
    lfs_stag_t res = lfs_dir_get(lfs, dir, LFS_MKTAG(0x7ff, 0, 0),
            LFS_MKTAG(LFS_TYPE_MOVESTATE, 0, sizeof(temp)), &temp);
    if (res < 0 && res != LFS_ERR_NOENT) {
        return res;
    }

    if (res != LFS_ERR_NOENT) {
        // xor together to find resulting gstate
        lfs_gstate_fromle32(&temp);
        lfs_gstate_xor(gstate, &temp);
    }

    return 0;
}

/**
 * @brief 填充 lfs_info：type/name/size（root 特判为 "/"）
 * @note  size 来源：CTZSTRUCT 读 ctz.size；INLINESTRUCT 即 tag.size
 *        （数据本身嵌在元数据对里，长度就是 tag 的 size 字段）。
 */
static int lfs_dir_getinfo(lfs_t *lfs, lfs_mdir_t *dir,
        uint16_t id, struct lfs_info *info) {
    if (id == 0x3ff) {
        // special case for root
        strcpy(info->name, "/");
        info->type = LFS_TYPE_DIR;
        return 0;
    }

    lfs_stag_t tag = lfs_dir_get(lfs, dir, LFS_MKTAG(0x780, 0x3ff, 0),
            LFS_MKTAG(LFS_TYPE_NAME, id, lfs->name_max+1), info->name);
    if (tag < 0) {
        return (int)tag;
    }

    info->type = lfs_tag_type3(tag);

    struct lfs_ctz ctz;
    tag = lfs_dir_get(lfs, dir, LFS_MKTAG(0x700, 0x3ff, 0),
            LFS_MKTAG(LFS_TYPE_STRUCT, id, sizeof(ctz)), &ctz);
    if (tag < 0) {
        return (int)tag;
    }
    lfs_ctz_fromle32(&ctz);

    if (lfs_tag_type3(tag) == LFS_TYPE_CTZSTRUCT) {
        info->size = ctz.size;
    } else if (lfs_tag_type3(tag) == LFS_TYPE_INLINESTRUCT) {
        info->size = lfs_tag_size(tag);
    }

    return 0;
}

struct lfs_dir_find_match {
    lfs_t *lfs;
    const void *name;
    lfs_size_t size;
};

/**
 * @brief 按名字逐字节比较盘上的 name tag（fetchmatch 的回调）
 * @return LFS_CMP_EQ 名字完全一致；LT/GT 供 fetchmatch 维护
 *         字典序插入位置（tempbesttag）
 */
static int lfs_dir_find_match(void *data,
        lfs_tag_t tag, const void *buffer) {
    struct lfs_dir_find_match *name = data;
    lfs_t *lfs = name->lfs;
    const struct lfs_diskoff *disk = buffer;

    // compare with disk
    lfs_size_t diff = lfs_min(name->size, lfs_tag_size(tag));
    int res = lfs_bd_cmp(lfs,
            NULL, &lfs->rcache, diff,
            disk->block, disk->off, name->name, diff);
    if (res != LFS_CMP_EQ) {
        return res;
    }

    // only equal if our size is still the same
    if (name->size != lfs_tag_size(tag)) {
        return (name->size < lfs_tag_size(tag)) ? LFS_CMP_LT : LFS_CMP_GT;
    }

    // found a match!
    return LFS_CMP_EQ;
}

// lfs_dir_find tries to set path and id even if file is not found
//
// returns:
// - 0                  if file is found
// - LFS_ERR_NOENT      if file or parent is not found
// - LFS_ERR_NOTDIR     if parent is not a dir
/**
 * @brief 路径解析：逐段下钻目录树，定位最终条目
 * @param  path 输入完整路径；返回时指向"最后一段"（便于创建新条目）
 * @param  id   输出最后一段的命中/插入 id
 * @return 命中 tag；LFS_ERR_NOENT 文件或父目录不存在；
 *         LFS_ERR_NOTDIR 中途遇到文件；'.' 与可消化的 '..' 已就地处理
 * @note   每段用 fetchmatch(name) 找条目；目录跨块时沿 hardtail 继续
 *         （字典序分家保证）；'..' 只允许成对消化、不允许越根。
 */
static lfs_stag_t lfs_dir_find(lfs_t *lfs, lfs_mdir_t *dir,
        const char **path, uint16_t *id) {
    // we reduce path to a single name if we can find it
    const char *name = *path;

    // default to root dir
    lfs_stag_t tag = LFS_MKTAG(LFS_TYPE_DIR, 0x3ff, 0);
    dir->tail[0] = lfs->root[0];
    dir->tail[1] = lfs->root[1];

    // empty paths are not allowed
    if (*name == '\0') {
        return LFS_ERR_INVAL;
    }

    while (true) {
nextname:
        // skip slashes if we're a directory
        if (lfs_tag_type3(tag) == LFS_TYPE_DIR) {
            name += strspn(name, "/");
        }
        lfs_size_t namelen = (lfs_size_t)strcspn(name, "/");

        // skip '.'
        if (namelen == 1 && memcmp(name, ".", 1) == 0) {
            name += namelen;
            goto nextname;
        }

        // error on unmatched '..', trying to go above root?
        if (namelen == 2 && memcmp(name, "..", 2) == 0) {
            return LFS_ERR_INVAL;
        }

        // skip if matched by '..' in name
        const char *suffix = name + namelen;
        lfs_size_t sufflen;
        int depth = 1;
        while (true) {
            suffix += strspn(suffix, "/");
            sufflen = (lfs_size_t)strcspn(suffix, "/");
            if (sufflen == 0) {
                break;
            }

            if (sufflen == 1 && memcmp(suffix, ".", 1) == 0) {
                // noop
            } else if (sufflen == 2 && memcmp(suffix, "..", 2) == 0) {
                depth -= 1;
                if (depth == 0) {
                    name = suffix + sufflen;
                    goto nextname;
                }
            } else {
                depth += 1;
            }

            suffix += sufflen;
        }

        // found path
        if (*name == '\0') {
            return tag;
        }

        // update what we've found so far
        *path = name;

        // only continue if we're a directory
        if (lfs_tag_type3(tag) != LFS_TYPE_DIR) {
            return LFS_ERR_NOTDIR;
        }

        // grab the entry data
        if (lfs_tag_id(tag) != 0x3ff) {
            lfs_stag_t res = lfs_dir_get(lfs, dir, LFS_MKTAG(0x700, 0x3ff, 0),
                    LFS_MKTAG(LFS_TYPE_STRUCT, lfs_tag_id(tag), 8), dir->tail);
            if (res < 0) {
                return res;
            }
            lfs_pair_fromle32(dir->tail);
        }

        // find entry matching name
        while (true) {
            tag = lfs_dir_fetchmatch(lfs, dir, dir->tail,
                    LFS_MKTAG(0x780, 0, 0),
                    LFS_MKTAG(LFS_TYPE_NAME, 0, namelen),
                    id,
                    lfs_dir_find_match, &(struct lfs_dir_find_match){
                        lfs, name, namelen});
            if (tag < 0) {
                return tag;
            }

            if (tag) {
                break;
            }

            if (!dir->split) {
                return LFS_ERR_NOENT;
            }
        }

        // to next name
        name += namelen;
    }
}

// commit logic
// 一次提交的游标状态：block/off 写入位置，ptag 上一条 tag（XOR 链），
// crc 运行校验，begin/end 日志可用区间（end 预留 8 字节给收尾 CRC）
struct lfs_commit {
    lfs_block_t block;
    lfs_off_t off;
    lfs_tag_t ptag;
    uint32_t crc;

    lfs_off_t begin;
    lfs_off_t end;
};

#ifndef LFS_READONLY
/**
 * @brief 提交的最小写单元：写出一段数据并同步累进 CRC 与偏移
 */
static int lfs_dir_commitprog(lfs_t *lfs, struct lfs_commit *commit,
        const void *buffer, lfs_size_t size) {
    int err = lfs_bd_prog(lfs,
            &lfs->pcache, &lfs->rcache, false,
            commit->block, commit->off ,
            (const uint8_t*)buffer, size);
    if (err) {
        return err;
    }

    commit->crc = lfs_crc(commit->crc, buffer, size);
    commit->off += size;
    return 0;
}
#endif

#ifndef LFS_READONLY
/**
 * @brief 写出一条 tag+数据（tag 大端、与前 tag XOR；数据可来自 RAM 或盘）
 * @note  tag 高位 0x80000000 表示 buffer 实为 struct lfs_diskoff——
 *        move 源数据直接盘到盘搬运不占 RAM（逐字节循环靠缓存提速）。
 *        空间不足返回 LFS_ERR_NOSPC，触发上层 compact。
 */
static int lfs_dir_commitattr(lfs_t *lfs, struct lfs_commit *commit,
        lfs_tag_t tag, const void *buffer) {
    // check if we fit
    lfs_size_t dsize = lfs_tag_dsize(tag);
    if (commit->off + dsize > commit->end) {
        return LFS_ERR_NOSPC;
    }

    // write out tag
    lfs_tag_t ntag = lfs_tobe32((tag & 0x7fffffff) ^ commit->ptag);
    int err = lfs_dir_commitprog(lfs, commit, &ntag, sizeof(ntag));
    if (err) {
        return err;
    }

    if (!(tag & 0x80000000)) {
        // from memory
        err = lfs_dir_commitprog(lfs, commit, buffer, dsize-sizeof(tag));
        if (err) {
            return err;
        }
    } else {
        // from disk
        const struct lfs_diskoff *disk = buffer;
        for (lfs_off_t i = 0; i < dsize-sizeof(tag); i++) {
            // rely on caching to make this efficient
            uint8_t dat;
            err = lfs_bd_read(lfs,
                    NULL, &lfs->rcache, dsize-sizeof(tag)-i,
                    disk->block, disk->off+i, &dat, 1);
            if (err) {
                return err;
            }

            err = lfs_dir_commitprog(lfs, commit, &dat, 1);
            if (err) {
                return err;
            }
        }
    }

    commit->ptag = tag & 0x7fffffff;
    return 0;
}
#endif

#ifndef LFS_READONLY

/**
 * @brief 收尾一次提交：对齐 prog_size、写 CRC tag、回读验证
 * @note  两种形态：块中部=5 词 CRC+FCRC（顺带校验下一窗口擦除态）；
 *        块尾=2 词 CRC（无后续写）。padding 不计入 CRC，所以收尾
 *        校验分两段：非 padding 区比对已知 CRC + 单独验证 CRC 字段
 *        自身为 0（CRC(crc)==0 自校验），防止拣到无关旧提交。
 *        eperturb 技巧：若下一窗口首字节与擦除值相同会"写不动"，
 *        翻转 valid 位强制下个提交判无效，保证状态机不卡死。
 */
static int lfs_dir_commitcrc(lfs_t *lfs, struct lfs_commit *commit) {
    // align to program units
    //
    // this gets a bit complex as we have two types of crcs:
    // - 5-word crc with fcrc to check following prog (middle of block)
    // - 2-word crc with no following prog (end of block)
    const lfs_off_t end = lfs_alignup(
            lfs_min(commit->off + 5*sizeof(uint32_t), lfs->cfg->block_size),
            lfs->cfg->prog_size);

    lfs_off_t off1 = 0;
    uint32_t crc1 = 0;

    // padding 不参与 CRC：fetch 扫描时可直接跳过它，但这使收尾
    // 校验必须分两段进行（见函数尾）
    // create crc tags to fill up remainder of commit, note that
    // padding is not crced, which lets fetches skip padding but
    // makes committing a bit more complicated
    while (commit->off < end) {
        lfs_off_t noff = (
                lfs_min(end - (commit->off+sizeof(lfs_tag_t)), 0x3fe)
                + (commit->off+sizeof(lfs_tag_t)));
        // too large for crc tag? need padding commits
        if (noff < end) {
            noff = lfs_min(noff, end - 5*sizeof(uint32_t));
        }

        // 需要 FCRC？先窥探下一 prog 窗口首字节：全 0xff 无需扰动；
        // 否则用 CRC tag 的 valid 位扰动，绕开"写不动"的死锁
        // space for fcrc?
        uint8_t eperturb = (uint8_t)-1;
        if (noff >= end && noff <= lfs->cfg->block_size - lfs->cfg->prog_size) {
            // first read the leading byte, this always contains a bit
            // we can perturb to avoid writes that don't change the fcrc
            int err = lfs_bd_read(lfs,
                    NULL, &lfs->rcache, lfs->cfg->prog_size,
                    commit->block, noff, &eperturb, 1);
            if (err && err != LFS_ERR_CORRUPT) {
                return err;
            }

        #ifdef LFS_MULTIVERSION
            // unfortunately fcrcs break mdir fetching < lfs2.1, so only write
            // these if we're a >= lfs2.1 filesystem
            if (lfs_fs_disk_version(lfs) <= 0x00020000) {
                // don't write fcrc
            } else
        #endif
            {
                // find the expected fcrc, don't bother avoiding a reread
                // of the eperturb, it should still be in our cache
                struct lfs_fcrc fcrc = {
                    .size = lfs->cfg->prog_size,
                    .crc = 0xffffffff
                };
                err = lfs_bd_crc(lfs,
                        NULL, &lfs->rcache, lfs->cfg->prog_size,
                        commit->block, noff, fcrc.size, &fcrc.crc);
                if (err && err != LFS_ERR_CORRUPT) {
                    return err;
                }

                lfs_fcrc_tole32(&fcrc);
                err = lfs_dir_commitattr(lfs, commit,
                        LFS_MKTAG(LFS_TYPE_FCRC, 0x3ff, sizeof(struct lfs_fcrc)),
                        &fcrc);
                if (err) {
                    return err;
                }
            }
        }

        // build commit crc
        struct {
            lfs_tag_t tag;
            uint32_t crc;
        } ccrc;
        lfs_tag_t ntag = LFS_MKTAG(
                LFS_TYPE_CCRC + (((uint8_t)~eperturb) >> 7), 0x3ff,
                noff - (commit->off+sizeof(lfs_tag_t)));
        ccrc.tag = lfs_tobe32(ntag ^ commit->ptag);
        commit->crc = lfs_crc(commit->crc, &ccrc.tag, sizeof(lfs_tag_t));
        ccrc.crc = lfs_tole32(commit->crc);

        int err = lfs_bd_prog(lfs,
                &lfs->pcache, &lfs->rcache, false,
                commit->block, commit->off, &ccrc, sizeof(ccrc));
        if (err) {
            return err;
        }

        // 记录第一条非 padding CRC 的位置与值，收尾回读比对用
        // keep track of non-padding checksum to verify
        if (off1 == 0) {
            off1 = commit->off + sizeof(lfs_tag_t);
            crc1 = commit->crc;
        }

        commit->off = noff;
        // perturb valid bit?
        commit->ptag = ntag ^ ((lfs_tag_t)(0x80 & ~eperturb) << 24);
        // reset crc for next commit
        commit->crc = 0xffffffff;

        // manually flush here since we don't prog the padding, this confuses
        // the caching layer
        if (noff >= end || noff >= lfs->pcache.off + lfs->cfg->cache_size) {
            // flush buffers
            int err = lfs_bd_sync(lfs, &lfs->pcache, &lfs->rcache, false);
            if (err) {
                return err;
            }
        }
    }

    // 提交完成前回读校验：对不上说明该块大概率要坏，上层走
    // relocate 换块重写（数据还在 RAM，零损失）
    // successful commit, check checksums to make sure
    //
    // note that we don't need to check padding commits, worst
    // case if they are corrupted we would have had to compact anyways
    lfs_off_t off = commit->begin;
    uint32_t crc = 0xffffffff;
    int err = lfs_bd_crc(lfs,
            NULL, &lfs->rcache, off1+sizeof(uint32_t),
            commit->block, off, off1-off, &crc);
    if (err) {
        return err;
    }

    // check non-padding commits against known crc
    if (crc != crc1) {
        return LFS_ERR_CORRUPT;
    }

    // make sure to check crc in case we happen to pick
    // up an unrelated crc (frozen block?)
    err = lfs_bd_crc(lfs,
            NULL, &lfs->rcache, sizeof(uint32_t),
            commit->block, off1, sizeof(uint32_t), &crc);
    if (err) {
        return err;
    }

    if (crc != 0) {
        return LFS_ERR_CORRUPT;
    }

    return 0;
}
#endif

#ifndef LFS_READONLY
/**
 * @brief 为新元数据对分配两个块并初始化 RAM 状态（不写盘）
 * @note  逆序分配（先 pair[1] 后 pair[0]）：万一第二个分配失败，
 *        已分配块仍是干净空闲块，不留半初始化状态；rev 对齐
 *        (block_cycles+1)|1 让新对不会立刻踩到磨损搬迁点。
 */
static int lfs_dir_alloc(lfs_t *lfs, lfs_mdir_t *dir) {
    // allocate pair of dir blocks (backwards, so we write block 1 first)
    for (int i = 0; i < 2; i++) {
        int err = lfs_alloc(lfs, &dir->pair[(i+1)%2]);
        if (err) {
            return err;
        }
    }

    // zero for reproducibility in case initial block is unreadable
    dir->rev = 0;

    // rather than clobbering one of the blocks we just pretend
    // the revision may be valid
    int err = lfs_bd_read(lfs,
            NULL, &lfs->rcache, sizeof(dir->rev),
            dir->pair[0], 0, &dir->rev, sizeof(dir->rev));
    dir->rev = lfs_fromle32(dir->rev);
    if (err && err != LFS_ERR_CORRUPT) {
        return err;
    }

    // rev 对齐到搬迁模数：防止"刚分配就到达搬迁点"造成
    // 分配→立即重写的空转
    // to make sure we don't immediately evict, align the new revision count
    // to our block_cycles modulus, see lfs_dir_compact for why our modulus
    // is tweaked this way
    if (lfs->cfg->block_cycles > 0) {
        dir->rev = lfs_alignup(dir->rev, ((lfs->cfg->block_cycles+1)|1));
    }

    // set defaults
    dir->off = sizeof(dir->rev);
    dir->etag = 0xffffffff;
    dir->count = 0;
    dir->tail[0] = LFS_BLOCK_NULL;
    dir->tail[1] = LFS_BLOCK_NULL;
    dir->erased = false;
    dir->split = false;

    // don't write out yet, let caller take care of that
    return 0;
}
#endif

#ifndef LFS_READONLY
/**
 * @brief 摘除空的尾元数据对：把它的 tail（和 gstate delta）并回前一对
 * @note  remove/rename 删空目录后目录链要收缩一格；先偷走 gdelta
 *        再改 tail，一次 commit 完成；中途掉电由 deorphan 兜底。
 */
static int lfs_dir_drop(lfs_t *lfs, lfs_mdir_t *dir, lfs_mdir_t *tail) {
    // steal state
    int err = lfs_dir_getgstate(lfs, tail, &lfs->gdelta);
    if (err) {
        return err;
    }

    // steal tail
    lfs_pair_tole32(tail->tail);
    err = lfs_dir_commit(lfs, dir, LFS_MKATTRS(
            {LFS_MKTAG(LFS_TYPE_TAIL + tail->split, 0x3ff, 8), tail->tail}));
    lfs_pair_fromle32(tail->tail);
    if (err) {
        return err;
    }

    return 0;
}
#endif

#ifndef LFS_READONLY
/**
 * @brief 把 [split,end) 的条目搬到新元数据对，本对保留 [begin,split)
 * @note  元数据对超过约 50% 就分家——日志式存储摊销 O(1) 的关键；
 *        root 分家时 root 指针跟到新尾部（超块链自然生长）。
 */
static int lfs_dir_split(lfs_t *lfs,
        lfs_mdir_t *dir, const struct lfs_mattr *attrs, int attrcount,
        lfs_mdir_t *source, uint16_t split, uint16_t end) {
    // create tail metadata pair
    lfs_mdir_t tail;
    int err = lfs_dir_alloc(lfs, &tail);
    if (err) {
        return err;
    }

    tail.split = dir->split;
    tail.tail[0] = dir->tail[0];
    tail.tail[1] = dir->tail[1];

    // note we don't care about LFS_OK_RELOCATED
    int res = lfs_dir_compact(lfs, &tail, attrs, attrcount, source, split, end);
    if (res < 0) {
        return res;
    }

    dir->tail[0] = tail.pair[0];
    dir->tail[1] = tail.pair[1];
    dir->split = true;

    // update root if needed
    if (lfs_pair_cmp(dir->pair, lfs->root) == 0 && split == 0) {
        lfs->root[0] = tail.pair[0];
        lfs->root[1] = tail.pair[1];
    }

    return 0;
}
#endif

#ifndef LFS_READONLY
/** @brief traverse 回调：累计 [begin,end) 条目的盘上字节数（分家决策用） */
static int lfs_dir_commit_size(void *p, lfs_tag_t tag, const void *buffer) {
    lfs_size_t *size = p;
    (void)buffer;

    *size += lfs_tag_dsize(tag);
    return 0;
}
#endif

#ifndef LFS_READONLY
struct lfs_dir_commit_commit {
    lfs_t *lfs;
    struct lfs_commit *commit;
};
#endif

#ifndef LFS_READONLY
/** @brief traverse 回调：把每个 tag 交给 lfs_dir_commitattr 写出 */
static int lfs_dir_commit_commit(void *p, lfs_tag_t tag, const void *buffer) {
    struct lfs_dir_commit_commit *commit = p;
    return lfs_dir_commitattr(commit->lfs, commit->commit, tag, buffer);
}
#endif

#ifndef LFS_READONLY
static bool lfs_dir_needsrelocation(lfs_t *lfs, lfs_mdir_t *dir) {
    // If our revision count == n * block_cycles, we should force a relocation,
    // this is how littlefs wear-levels at the metadata-pair level. Note that we
    // actually use (block_cycles+1)|1, this is to avoid two corner cases:
    // 1. block_cycles = 1, which would prevent relocations from terminating
    // 2. block_cycles = 2n, which, due to aliasing, would only ever relocate
    //    one metadata block in the pair, effectively making this useless
    return (lfs->cfg->block_cycles > 0
            && ((dir->rev + 1) % ((lfs->cfg->block_cycles+1)|1) == 0));
}
#endif

#ifndef LFS_READONLY
/**
 * @brief 压缩重写元数据对：擦 pair[1]→写 rev→全量条目→tail→gstate→CRC
 * @param  source 条目来源（通常为当前对；分家时为原对）
 * @param  begin/end 只搬 [begin,end) 的 id 区间
 * @return 0 或 LFS_OK_RELOCATED；坏块自动换块重试
 * @note   搬迁的两种触发：rev 达到 block_cycles 模数（磨损均衡，tired）
 *         或写坏块。superblock（块 0/1）不可搬迁——它就是根，
 *         磨损耗尽时报 LFS_ERR_NOSPC（文件系统"冻结"），这是
 *         littlefs 唯一无法自愈的点。
 */
static int lfs_dir_compact(lfs_t *lfs,
        lfs_mdir_t *dir, const struct lfs_mattr *attrs, int attrcount,
        lfs_mdir_t *source, uint16_t begin, uint16_t end) {
    // save some state in case block is bad
    bool relocated = false;
    bool tired = lfs_dir_needsrelocation(lfs, dir);

    // increment revision count
    dir->rev += 1;

    // do not proactively relocate blocks during migrations, this
    // can cause a number of failure states such: clobbering the
    // v1 superblock if we relocate root, and invalidating directory
    // pointers if we relocate the head of a directory. On top of
    // this, relocations increase the overall complexity of
    // lfs_migration, which is already a delicate operation.
#ifdef LFS_MIGRATE
    if (lfs->lfs1) {
        tired = false;
    }
#endif

    // 磨损均衡：这对块写到寿命就主动换块，把元数据磨损摊到全盘
    if (tired && lfs_pair_cmp(dir->pair, (const lfs_block_t[2]){0, 1}) != 0) {
        // we're writing too much, time to relocate
        goto relocate;
    }

    // begin loop to commit compaction to blocks until a compact sticks
    while (true) {
        {
            // setup commit state
            struct lfs_commit commit = {
                .block = dir->pair[1],
                .off = 0,
                .ptag = 0xffffffff,
                .crc = 0xffffffff,

                .begin = 0,
                .end = (lfs->cfg->metadata_max ?
                    lfs->cfg->metadata_max : lfs->cfg->block_size) - 8,
            };

            // erase block to write to
            int err = lfs_bd_erase(lfs, dir->pair[1]);
            if (err) {
                if (err == LFS_ERR_CORRUPT) {
                    goto relocate;
                }
                return err;
            }

            // rev 先行写盘：fetchmatch 靠它裁决新旧，所以它必须最先落笔
            // write out header
            dir->rev = lfs_tole32(dir->rev);
            err = lfs_dir_commitprog(lfs, &commit,
                    &dir->rev, sizeof(dir->rev));
            dir->rev = lfs_fromle32(dir->rev);
            if (err) {
                if (err == LFS_ERR_CORRUPT) {
                    goto relocate;
                }
                return err;
            }

            // traverse the directory, this time writing out all unique tags
            err = lfs_dir_traverse(lfs,
                    source, 0, 0xffffffff, attrs, attrcount,
                    LFS_MKTAG(0x400, 0x3ff, 0),
                    LFS_MKTAG(LFS_TYPE_NAME, 0, 0),
                    begin, end, -begin,
                    lfs_dir_commit_commit, &(struct lfs_dir_commit_commit){
                        lfs, &commit});
            if (err) {
                if (err == LFS_ERR_CORRUPT) {
                    goto relocate;
                }
                return err;
            }

            // commit tail, which may be new after last size check
            if (!lfs_pair_isnull(dir->tail)) {
                lfs_pair_tole32(dir->tail);
                err = lfs_dir_commitattr(lfs, &commit,
                        LFS_MKTAG(LFS_TYPE_TAIL + dir->split, 0x3ff, 8),
                        dir->tail);
                lfs_pair_fromle32(dir->tail);
                if (err) {
                    if (err == LFS_ERR_CORRUPT) {
                        goto relocate;
                    }
                    return err;
                }
            }

            // bring over gstate?
            lfs_gstate_t delta = {0};
            if (!relocated) {
                lfs_gstate_xor(&delta, &lfs->gdisk);
                lfs_gstate_xor(&delta, &lfs->gstate);
            }
            lfs_gstate_xor(&delta, &lfs->gdelta);
            delta.tag &= ~LFS_MKTAG(0, 0, 0x3ff);

            err = lfs_dir_getgstate(lfs, dir, &delta);
            if (err) {
                return err;
            }

            if (!lfs_gstate_iszero(&delta)) {
                lfs_gstate_tole32(&delta);
                err = lfs_dir_commitattr(lfs, &commit,
                        LFS_MKTAG(LFS_TYPE_MOVESTATE, 0x3ff,
                            sizeof(delta)), &delta);
                if (err) {
                    if (err == LFS_ERR_CORRUPT) {
                        goto relocate;
                    }
                    return err;
                }
            }

            // complete commit with crc
            err = lfs_dir_commitcrc(lfs, &commit);
            if (err) {
                if (err == LFS_ERR_CORRUPT) {
                    goto relocate;
                }
                return err;
            }

            // successful compaction, swap dir pair to indicate most recent
            LFS_ASSERT(commit.off % lfs->cfg->prog_size == 0);
            lfs_pair_swap(dir->pair);
            dir->count = end - begin;
            dir->off = commit.off;
            dir->etag = commit.ptag;
            // update gstate
            lfs->gdelta = (lfs_gstate_t){0};
            if (!relocated) {
                lfs->gdisk = lfs->gstate;
            }
        }
        break;

relocate:
        // commit was corrupted, drop caches and prepare to relocate block
        relocated = true;
        lfs_cache_drop(lfs, &lfs->pcache);
        if (!tired) {
            LFS_DEBUG("Bad block at 0x%"PRIx32, dir->pair[1]);
        }

        // can't relocate superblock, filesystem is now frozen
        if (lfs_pair_cmp(dir->pair, (const lfs_block_t[2]){0, 1}) == 0) {
            LFS_WARN("Superblock 0x%"PRIx32" has become unwritable",
                    dir->pair[1]);
            return LFS_ERR_NOSPC;
        }

        // relocate half of pair
        int err = lfs_alloc(lfs, &dir->pair[1]);
        if (err && (err != LFS_ERR_NOSPC || !tired)) {
            return err;
        }

        tired = false;
        continue;
    }

    return relocated ? LFS_OK_RELOCATED : 0;
}
#endif

#ifndef LFS_READONLY
/**
 * @brief compact 前的"能分则分"：按 40 字节保留量反复折半到放得下
 * @note  分不出去（无空闲块）就降级为纯 compact（性能换可用性）；
 *        superblock 还有扩张路径：盘使用率 <~88% 时把超块链再长一格
 *        延长根寿命（扩张不可回收，故保守）。
 */
static int lfs_dir_splittingcompact(lfs_t *lfs, lfs_mdir_t *dir,
        const struct lfs_mattr *attrs, int attrcount,
        lfs_mdir_t *source, uint16_t begin, uint16_t end) {
    while (true) {
        // find size of first split, we do this by halving the split until
        // the metadata is guaranteed to fit
        //
        // Note that this isn't a true binary search, we never increase the
        // split size. This may result in poorly distributed metadata but isn't
        // worth the extra code size or performance hit to fix.
        lfs_size_t split = begin;
        while (end - split > 1) {
            lfs_size_t size = 0;
            int err = lfs_dir_traverse(lfs,
                    source, 0, 0xffffffff, attrs, attrcount,
                    LFS_MKTAG(0x400, 0x3ff, 0),
                    LFS_MKTAG(LFS_TYPE_NAME, 0, 0),
                    split, end, -split,
                    lfs_dir_commit_size, &size);
            if (err) {
                return err;
            }

            // space is complicated, we need room for:
            //
            // - tail:         4+2*4 = 12 bytes
            // - gstate:       4+3*4 = 16 bytes
            // - move delete:  4     = 4 bytes
            // - crc:          4+4   = 8 bytes
            //                 total = 40 bytes
            //
            // And we cap at half a block to avoid degenerate cases with
            // nearly-full metadata blocks.
            //
            lfs_size_t metadata_max = (lfs->cfg->metadata_max)
                    ? lfs->cfg->metadata_max
                    : lfs->cfg->block_size;
            if (end - split < 0xff
                    && size <= lfs_min(
                        metadata_max - 40,
                        lfs_alignup(
                            metadata_max/2,
                            lfs->cfg->prog_size))) {
                break;
            }

            split = split + ((end - split) / 2);
        }

        if (split == begin) {
            // no split needed
            break;
        }

        // split into two metadata pairs and continue
        int err = lfs_dir_split(lfs, dir, attrs, attrcount,
                source, split, end);
        if (err && err != LFS_ERR_NOSPC) {
            return err;
        }

        if (err) {
            // we can't allocate a new block, try to compact with degraded
            // performance
            LFS_WARN("Unable to split {0x%"PRIx32", 0x%"PRIx32"}",
                    dir->pair[0], dir->pair[1]);
            break;
        } else {
            end = split;
        }
    }

    if (lfs_dir_needsrelocation(lfs, dir)
            && lfs_pair_cmp(dir->pair, (const lfs_block_t[2]){0, 1}) == 0) {
        // oh no! we're writing too much to the superblock,
        // should we expand?
        lfs_ssize_t size = lfs_fs_size_(lfs);
        if (size < 0) {
            return size;
        }

        // littlefs cannot reclaim expanded superblocks, so expand cautiously
        //
        // if our filesystem is more than ~88% full, don't expand, this is
        // somewhat arbitrary
        if (lfs->block_count - size > lfs->block_count/8) {
            LFS_DEBUG("Expanding superblock at rev %"PRIu32, dir->rev);
            int err = lfs_dir_split(lfs, dir, attrs, attrcount,
                    source, begin, end);
            if (err && err != LFS_ERR_NOSPC) {
                return err;
            }

            if (err) {
                // welp, we tried, if we ran out of space there's not much
                // we can do, we'll error later if we've become frozen
                LFS_WARN("Unable to expand superblock");
            } else {
                // duplicate the superblock entry into the new superblock
                end = 1;
            }
        }
    }

    return lfs_dir_compact(lfs, dir, attrs, attrcount, source, begin, end);
}
#endif

#ifndef LFS_READONLY
/**
 * @brief 元数据提交的执行体：先试日志追加，失败/到期则 compact
 * @param  pair 提交前该对的原始块号（fixmlist 用它同步打开句柄）
 * @return 0 / LFS_OK_DROPPED（该对已空应从链上摘除）/ LFS_OK_RELOCATED
 * @note   三段式：1) 预演 attrs 对 count/tail 的影响，空了对先摘；
 *         2) erased 且未满则直接追加（快路径，掉电窗口=一条 commit+crc）；
 *         3) 否则 splittingcompact 重写。
 *         fixmlist 两遍扫描修正所有打开句柄的 id/pair，保证之后的
 *         读写仍落在正确的条目上。
 */
static int lfs_dir_relocatingcommit(lfs_t *lfs, lfs_mdir_t *dir,
        const lfs_block_t pair[2],
        const struct lfs_mattr *attrs, int attrcount,
        lfs_mdir_t *pdir) {
    int state = 0;

    // calculate changes to the directory
    bool hasdelete = false;
    for (int i = 0; i < attrcount; i++) {
        if (lfs_tag_type3(attrs[i].tag) == LFS_TYPE_CREATE) {
            dir->count += 1;
        } else if (lfs_tag_type3(attrs[i].tag) == LFS_TYPE_DELETE) {
            LFS_ASSERT(dir->count > 0);
            dir->count -= 1;
            hasdelete = true;
        } else if (lfs_tag_type1(attrs[i].tag) == LFS_TYPE_TAIL) {
            dir->tail[0] = ((lfs_block_t*)attrs[i].buffer)[0];
            dir->tail[1] = ((lfs_block_t*)attrs[i].buffer)[1];
            dir->split = (lfs_tag_chunk(attrs[i].tag) & 1);
            lfs_pair_fromle32(dir->tail);
        }
    }

    // should we actually drop the directory block?
    if (hasdelete && dir->count == 0) {
        LFS_ASSERT(pdir);
        int err = lfs_fs_pred(lfs, dir->pair, pdir);
        if (err && err != LFS_ERR_NOENT) {
            return err;
        }

        if (err != LFS_ERR_NOENT && pdir->split) {
            state = LFS_OK_DROPPED;
            goto fixmlist;
        }
    }

    if (dir->erased && dir->count < 0xff) {
        // try to commit
        struct lfs_commit commit = {
            .block = dir->pair[0],
            .off = dir->off,
            .ptag = dir->etag,
            .crc = 0xffffffff,

            .begin = dir->off,
            .end = (lfs->cfg->metadata_max ?
                lfs->cfg->metadata_max : lfs->cfg->block_size) - 8,
        };

        // traverse attrs that need to be written out
        lfs_pair_tole32(dir->tail);
        int err = lfs_dir_traverse(lfs,
                dir, dir->off, dir->etag, attrs, attrcount,
                0, 0, 0, 0, 0,
                lfs_dir_commit_commit, &(struct lfs_dir_commit_commit){
                    lfs, &commit});
        lfs_pair_fromle32(dir->tail);
        if (err) {
            if (err == LFS_ERR_NOSPC || err == LFS_ERR_CORRUPT) {
                goto compact;
            }
            return err;
        }

        // commit any global diffs if we have any
        lfs_gstate_t delta = {0};
        lfs_gstate_xor(&delta, &lfs->gstate);
        lfs_gstate_xor(&delta, &lfs->gdisk);
        lfs_gstate_xor(&delta, &lfs->gdelta);
        delta.tag &= ~LFS_MKTAG(0, 0, 0x3ff);
        if (!lfs_gstate_iszero(&delta)) {
            err = lfs_dir_getgstate(lfs, dir, &delta);
            if (err) {
                return err;
            }

            lfs_gstate_tole32(&delta);
            err = lfs_dir_commitattr(lfs, &commit,
                    LFS_MKTAG(LFS_TYPE_MOVESTATE, 0x3ff,
                        sizeof(delta)), &delta);
            if (err) {
                if (err == LFS_ERR_NOSPC || err == LFS_ERR_CORRUPT) {
                    goto compact;
                }
                return err;
            }
        }

        // finalize commit with the crc
        err = lfs_dir_commitcrc(lfs, &commit);
        if (err) {
            if (err == LFS_ERR_NOSPC || err == LFS_ERR_CORRUPT) {
                goto compact;
            }
            return err;
        }

        // successful commit, update dir
        LFS_ASSERT(commit.off % lfs->cfg->prog_size == 0);
        dir->off = commit.off;
        dir->etag = commit.ptag;
        // and update gstate
        lfs->gdisk = lfs->gstate;
        lfs->gdelta = (lfs_gstate_t){0};

        goto fixmlist;
    }

compact:
    // fall back to compaction
    lfs_cache_drop(lfs, &lfs->pcache);

    state = lfs_dir_splittingcompact(lfs, dir, attrs, attrcount,
            dir, 0, dir->count);
    if (state < 0) {
        return state;
    }

    goto fixmlist;

fixmlist:;
    // this complicated bit of logic is for fixing up any active
    // metadata-pairs that we may have affected
    //
    // note we have to make two passes since the mdir passed to
    // lfs_dir_commit could also be in this list, and even then
    // we need to copy the pair so they don't get clobbered if we refetch
    // our mdir.
    lfs_block_t oldpair[2] = {pair[0], pair[1]};
    for (struct lfs_mlist *d = lfs->mlist; d; d = d->next) {
        if (lfs_pair_cmp(d->m.pair, oldpair) == 0) {
            d->m = *dir;
            if (d->m.pair != pair) {
                for (int i = 0; i < attrcount; i++) {
                    if (lfs_tag_type3(attrs[i].tag) == LFS_TYPE_DELETE &&
                            d->id == lfs_tag_id(attrs[i].tag) &&
                            d->type != LFS_TYPE_DIR) {
                        d->m.pair[0] = LFS_BLOCK_NULL;
                        d->m.pair[1] = LFS_BLOCK_NULL;
                    } else if (lfs_tag_type3(attrs[i].tag) == LFS_TYPE_DELETE &&
                            d->id > lfs_tag_id(attrs[i].tag)) {
                        d->id -= 1;
                        if (d->type == LFS_TYPE_DIR) {
                            ((lfs_dir_t*)d)->pos -= 1;
                        }
                    } else if (lfs_tag_type3(attrs[i].tag) == LFS_TYPE_CREATE &&
                            d->id >= lfs_tag_id(attrs[i].tag)) {
                        d->id += 1;
                        if (d->type == LFS_TYPE_DIR) {
                            ((lfs_dir_t*)d)->pos += 1;
                        }
                    }
                }
            }

            while (d->id >= d->m.count && d->m.split) {
                // we split and id is on tail now
                if (lfs_pair_cmp(d->m.tail, lfs->root) != 0) {
                    d->id -= d->m.count;
                }
                int err = lfs_dir_fetch(lfs, &d->m, d->m.tail);
                if (err) {
                    return err;
                }
            }
        }
    }

    return state;
}
#endif

#ifndef LFS_READONLY
/**
 * @brief commit 的完整版：处理搬迁连锁（父指针/前驱 tail）与孤儿
 * @note  搬迁会让"父目录里的 DIRSTRUCT 指针"与"目录链前驱的 tail"
 *        同时失效——两者无法在一个 commit 里原子完成，于是 littlefs
 *        把中间态合法化（孤儿/half-orphan 计入 gstate），掉电后由
 *        deorphan 二次修复。用 while 展开尾递归，避免无界栈。
 */
static int lfs_dir_orphaningcommit(lfs_t *lfs, lfs_mdir_t *dir,
        const struct lfs_mattr *attrs, int attrcount) {
    // 内联文件若超过 cache_size 且非 RAM 背书，提交前强制 outline：
    // 元数据 compact 会重写数据所在的元数据块，必须先把数据挪走
    // check for any inline files that aren't RAM backed and
    // forcefully evict them, needed for filesystem consistency
    for (lfs_file_t *f = (lfs_file_t*)lfs->mlist; f; f = f->next) {
        if (dir != &f->m && lfs_pair_cmp(f->m.pair, dir->pair) == 0 &&
                f->type == LFS_TYPE_REG && (f->flags & LFS_F_INLINE) &&
                f->ctz.size > lfs->cfg->cache_size) {
            int err = lfs_file_outline(lfs, f);
            if (err) {
                return err;
            }

            err = lfs_file_flush(lfs, f);
            if (err) {
                return err;
            }
        }
    }

    lfs_block_t lpair[2] = {dir->pair[0], dir->pair[1]};
    lfs_mdir_t ldir = *dir;
    lfs_mdir_t pdir;
    int state = lfs_dir_relocatingcommit(lfs, &ldir, dir->pair,
            attrs, attrcount, &pdir);
    if (state < 0) {
        return state;
    }

    // update if we're not in mlist, note we may have already been
    // updated if we are in mlist
    if (lfs_pair_cmp(dir->pair, lpair) == 0) {
        *dir = ldir;
    }

    // commit was successful, but may require other changes in the
    // filesystem, these would normally be tail recursive, but we have
    // flattened them here avoid unbounded stack usage

    // need to drop?
    if (state == LFS_OK_DROPPED) {
        // steal state
        int err = lfs_dir_getgstate(lfs, dir, &lfs->gdelta);
        if (err) {
            return err;
        }

        // steal tail, note that this can't create a recursive drop
        lpair[0] = pdir.pair[0];
        lpair[1] = pdir.pair[1];
        lfs_pair_tole32(dir->tail);
        state = lfs_dir_relocatingcommit(lfs, &pdir, lpair, LFS_MKATTRS(
                    {LFS_MKTAG(LFS_TYPE_TAIL + dir->split, 0x3ff, 8),
                        dir->tail}),
                NULL);
        lfs_pair_fromle32(dir->tail);
        if (state < 0) {
            return state;
        }

        ldir = pdir;
    }

    // need to relocate?
    bool orphans = false;
    while (state == LFS_OK_RELOCATED) {
        LFS_DEBUG("Relocating {0x%"PRIx32", 0x%"PRIx32"} "
                    "-> {0x%"PRIx32", 0x%"PRIx32"}",
                lpair[0], lpair[1], ldir.pair[0], ldir.pair[1]);
        state = 0;

        // update internal root
        if (lfs_pair_cmp(lpair, lfs->root) == 0) {
            lfs->root[0] = ldir.pair[0];
            lfs->root[1] = ldir.pair[1];
        }

        // update internally tracked dirs
        for (struct lfs_mlist *d = lfs->mlist; d; d = d->next) {
            if (lfs_pair_cmp(lpair, d->m.pair) == 0) {
                d->m.pair[0] = ldir.pair[0];
                d->m.pair[1] = ldir.pair[1];
            }

            if (d->type == LFS_TYPE_DIR &&
                    lfs_pair_cmp(lpair, ((lfs_dir_t*)d)->head) == 0) {
                ((lfs_dir_t*)d)->head[0] = ldir.pair[0];
                ((lfs_dir_t*)d)->head[1] = ldir.pair[1];
            }
        }

        // find parent
        lfs_stag_t tag = lfs_fs_parent(lfs, lpair, &pdir);
        if (tag < 0 && tag != LFS_ERR_NOENT) {
            return tag;
        }

        bool hasparent = (tag != LFS_ERR_NOENT);
        if (tag != LFS_ERR_NOENT) {
            // note that if we have a parent, we must have a pred, so this will
            // always create an orphan
            int err = lfs_fs_preporphans(lfs, +1);
            if (err) {
                return err;
            }

            // fix pending move in this pair? this looks like an optimization but
            // is in fact _required_ since relocating may outdate the move.
            uint16_t moveid = 0x3ff;
            if (lfs_gstate_hasmovehere(&lfs->gstate, pdir.pair)) {
                moveid = lfs_tag_id(lfs->gstate.tag);
                LFS_DEBUG("Fixing move while relocating "
                        "{0x%"PRIx32", 0x%"PRIx32"} 0x%"PRIx16"\n",
                        pdir.pair[0], pdir.pair[1], moveid);
                lfs_fs_prepmove(lfs, 0x3ff, NULL);
                if (moveid < lfs_tag_id(tag)) {
                    tag -= LFS_MKTAG(0, 1, 0);
                }
            }

            lfs_block_t ppair[2] = {pdir.pair[0], pdir.pair[1]};
            lfs_pair_tole32(ldir.pair);
            state = lfs_dir_relocatingcommit(lfs, &pdir, ppair, LFS_MKATTRS(
                        {LFS_MKTAG_IF(moveid != 0x3ff,
                            LFS_TYPE_DELETE, moveid, 0), NULL},
                        {tag, ldir.pair}),
                    NULL);
            lfs_pair_fromle32(ldir.pair);
            if (state < 0) {
                return state;
            }

            if (state == LFS_OK_RELOCATED) {
                lpair[0] = ppair[0];
                lpair[1] = ppair[1];
                ldir = pdir;
                orphans = true;
                continue;
            }
        }

        // find pred
        int err = lfs_fs_pred(lfs, lpair, &pdir);
        if (err && err != LFS_ERR_NOENT) {
            return err;
        }
        LFS_ASSERT(!(hasparent && err == LFS_ERR_NOENT));

        // if we can't find dir, it must be new
        if (err != LFS_ERR_NOENT) {
            if (lfs_gstate_hasorphans(&lfs->gstate)) {
                // next step, clean up orphans
                err = lfs_fs_preporphans(lfs, -(int8_t)hasparent);
                if (err) {
                    return err;
                }
            }

            // fix pending move in this pair? this looks like an optimization
            // but is in fact _required_ since relocating may outdate the move.
            uint16_t moveid = 0x3ff;
            if (lfs_gstate_hasmovehere(&lfs->gstate, pdir.pair)) {
                moveid = lfs_tag_id(lfs->gstate.tag);
                LFS_DEBUG("Fixing move while relocating "
                        "{0x%"PRIx32", 0x%"PRIx32"} 0x%"PRIx16"\n",
                        pdir.pair[0], pdir.pair[1], moveid);
                lfs_fs_prepmove(lfs, 0x3ff, NULL);
            }

            // replace bad pair, either we clean up desync, or no desync occured
            lpair[0] = pdir.pair[0];
            lpair[1] = pdir.pair[1];
            lfs_pair_tole32(ldir.pair);
            state = lfs_dir_relocatingcommit(lfs, &pdir, lpair, LFS_MKATTRS(
                        {LFS_MKTAG_IF(moveid != 0x3ff,
                            LFS_TYPE_DELETE, moveid, 0), NULL},
                        {LFS_MKTAG(LFS_TYPE_TAIL + pdir.split, 0x3ff, 8),
                            ldir.pair}),
                    NULL);
            lfs_pair_fromle32(ldir.pair);
            if (state < 0) {
                return state;
            }

            ldir = pdir;
        }
    }

    return orphans ? LFS_OK_ORPHANED : 0;
}
#endif

#ifndef LFS_READONLY
/**
 * @brief 元数据提交总入口：orphaningcommit + 收尾去孤儿
 * @note  返回 LFS_OK_ORPHANED 时立刻同步 deorphan(false)，把连锁
 *        孤儿清干净再返回用户——对外永远呈现一致状态。
 */
static int lfs_dir_commit(lfs_t *lfs, lfs_mdir_t *dir,
        const struct lfs_mattr *attrs, int attrcount) {
    int orphans = lfs_dir_orphaningcommit(lfs, dir, attrs, attrcount);
    if (orphans < 0) {
        return orphans;
    }

    if (orphans) {
        // make sure we've removed all orphans, this is a noop if there
        // are none, but if we had nested blocks failures we may have
        // created some
        int err = lfs_fs_deorphan(lfs, false);
        if (err) {
            return err;
        }
    }

    return 0;
}
#endif


/// Top level directory operations ///
#ifndef LFS_READONLY
/**
 * @brief 创建目录：分配新元数据对 → 挂到目录链尾 → 在父目录登记条目
 * @note  全程跨多个元数据对，无法一次原子；靠 gstate 孤儿计数 +
 *        下次写前 forceconsistency 补偿。链中段插入还要回改前驱的
 *        softtail（目录链 desync 的两个来源之一，另一个是搬迁）。
 */
static int lfs_mkdir_(lfs_t *lfs, const char *path) {
    // deorphan if we haven't yet, needed at most once after poweron
    int err = lfs_fs_forceconsistency(lfs);
    if (err) {
        return err;
    }

    struct lfs_mlist cwd;
    cwd.next = lfs->mlist;
    uint16_t id;
    err = lfs_dir_find(lfs, &cwd.m, &path, &id);
    if (!(err == LFS_ERR_NOENT && lfs_path_islast(path))) {
        return (err < 0) ? err : LFS_ERR_EXIST;
    }

    // check that name fits
    lfs_size_t nlen = lfs_path_namelen(path);
    if (nlen > lfs->name_max) {
        return LFS_ERR_NAMETOOLONG;
    }

    // build up new directory
    lfs_alloc_ckpoint(lfs);
    lfs_mdir_t dir;
    err = lfs_dir_alloc(lfs, &dir);
    if (err) {
        return err;
    }

    // find end of list
    lfs_mdir_t pred = cwd.m;
    while (pred.split) {
        err = lfs_dir_fetch(lfs, &pred, pred.tail);
        if (err) {
            return err;
        }
    }

    // setup dir
    lfs_pair_tole32(pred.tail);
    err = lfs_dir_commit(lfs, &dir, LFS_MKATTRS(
            {LFS_MKTAG(LFS_TYPE_SOFTTAIL, 0x3ff, 8), pred.tail}));
    lfs_pair_fromle32(pred.tail);
    if (err) {
        return err;
    }

    // current block not end of list?
    if (cwd.m.split) {
        // update tails, this creates a desync
        err = lfs_fs_preporphans(lfs, +1);
        if (err) {
            return err;
        }

        // it's possible our predecessor has to be relocated, and if
        // our parent is our predecessor's predecessor, this could have
        // caused our parent to go out of date, fortunately we can hook
        // ourselves into littlefs to catch this
        cwd.type = 0;
        cwd.id = 0;
        lfs->mlist = &cwd;

        lfs_pair_tole32(dir.pair);
        err = lfs_dir_commit(lfs, &pred, LFS_MKATTRS(
                {LFS_MKTAG(LFS_TYPE_SOFTTAIL, 0x3ff, 8), dir.pair}));
        lfs_pair_fromle32(dir.pair);
        if (err) {
            lfs->mlist = cwd.next;
            return err;
        }

        lfs->mlist = cwd.next;
        err = lfs_fs_preporphans(lfs, -1);
        if (err) {
            return err;
        }
    }

    // now insert into our parent block
    lfs_pair_tole32(dir.pair);
    err = lfs_dir_commit(lfs, &cwd.m, LFS_MKATTRS(
            {LFS_MKTAG(LFS_TYPE_CREATE, id, 0), NULL},
            {LFS_MKTAG(LFS_TYPE_DIR, id, nlen), path},
            {LFS_MKTAG(LFS_TYPE_DIRSTRUCT, id, 8), dir.pair},
            {LFS_MKTAG_IF(!cwd.m.split,
                LFS_TYPE_SOFTTAIL, 0x3ff, 8), dir.pair}));
    lfs_pair_fromle32(dir.pair);
    if (err) {
        return err;
    }

    return 0;
}
#endif

/**
 * @brief 打开目录：定位路径 → fetch 头块 → 登记 mlist（pos 从 0 起）
 * @note  root 目录直接用 lfs->root；普通目录从父条目的 DIRSTRUCT 取对。
 */
static int lfs_dir_open_(lfs_t *lfs, lfs_dir_t *dir, const char *path) {
    lfs_stag_t tag = lfs_dir_find(lfs, &dir->m, &path, NULL);
    if (tag < 0) {
        return tag;
    }

    if (lfs_tag_type3(tag) != LFS_TYPE_DIR) {
        return LFS_ERR_NOTDIR;
    }

    lfs_block_t pair[2];
    if (lfs_tag_id(tag) == 0x3ff) {
        // handle root dir separately
        pair[0] = lfs->root[0];
        pair[1] = lfs->root[1];
    } else {
        // get dir pair from parent
        lfs_stag_t res = lfs_dir_get(lfs, &dir->m, LFS_MKTAG(0x700, 0x3ff, 0),
                LFS_MKTAG(LFS_TYPE_STRUCT, lfs_tag_id(tag), 8), pair);
        if (res < 0) {
            return res;
        }
        lfs_pair_fromle32(pair);
    }

    // fetch first pair
    int err = lfs_dir_fetch(lfs, &dir->m, pair);
    if (err) {
        return err;
    }

    // setup entry
    dir->head[0] = dir->m.pair[0];
    dir->head[1] = dir->m.pair[1];
    dir->id = 0;
    dir->pos = 0;

    // add to list of mdirs
    dir->type = LFS_TYPE_DIR;
    lfs_mlist_append(lfs, (struct lfs_mlist *)dir);

    return 0;
}

/** @brief 关闭目录：从 mlist 摘除（无盘上副作用） */
static int lfs_dir_close_(lfs_t *lfs, lfs_dir_t *dir) {
    // remove from list of mdirs
    lfs_mlist_remove(lfs, (struct lfs_mlist *)dir);

    return 0;
}

/**
 * @brief 迭代目录项：pos 0/1 合成 "."/".."，之后按 id 顺序产出
 * @return true 有条目（info 已填）；false 到尾；负错误码
 * @note  本对读尽沿 tail 跨块；被删除的空洞（NOENT）静默跳过，
 *        调用方看到的永远是连续条目。
 */
static int lfs_dir_read_(lfs_t *lfs, lfs_dir_t *dir, struct lfs_info *info) {
    memset(info, 0, sizeof(*info));

    // special offset for '.' and '..'
    if (dir->pos == 0) {
        info->type = LFS_TYPE_DIR;
        strcpy(info->name, ".");
        dir->pos += 1;
        return true;
    } else if (dir->pos == 1) {
        info->type = LFS_TYPE_DIR;
        strcpy(info->name, "..");
        dir->pos += 1;
        return true;
    }

    while (true) {
        if (dir->id == dir->m.count) {
            if (!dir->m.split) {
                return false;
            }

            int err = lfs_dir_fetch(lfs, &dir->m, dir->m.tail);
            if (err) {
                return err;
            }

            dir->id = 0;
        }

        int err = lfs_dir_getinfo(lfs, &dir->m, dir->id, info);
        if (err && err != LFS_ERR_NOENT) {
            return err;
        }

        dir->id += 1;
        if (err != LFS_ERR_NOENT) {
            break;
        }
    }

    dir->pos += 1;
    return true;
}

/**
 * @brief 目录定位：rewind 后按条目序号快进（跨块推进）
 * @note  tell/seek 的坐标是"条目序号"而非字节偏移；root 对的
 *        superblock 条目（id 0）对用户不可见，定位时跳过。
 */
static int lfs_dir_seek_(lfs_t *lfs, lfs_dir_t *dir, lfs_off_t off) {
    // simply walk from head dir
    int err = lfs_dir_rewind_(lfs, dir);
    if (err) {
        return err;
    }

    // first two for ./..
    dir->pos = lfs_min(2, off);
    off -= dir->pos;

    // skip superblock entry
    dir->id = (off > 0 && lfs_pair_cmp(dir->head, lfs->root) == 0);

    while (off > 0) {
        if (dir->id == dir->m.count) {
            if (!dir->m.split) {
                return LFS_ERR_INVAL;
            }

            err = lfs_dir_fetch(lfs, &dir->m, dir->m.tail);
            if (err) {
                return err;
            }

            dir->id = 0;
        }

        int diff = lfs_min(dir->m.count - dir->id, off);
        dir->id += diff;
        dir->pos += diff;
        off -= diff;
    }

    return 0;
}

/** @brief 返回目录迭代位置（条目序号，含 ./.. 两个合成项） */
static lfs_soff_t lfs_dir_tell_(lfs_t *lfs, lfs_dir_t *dir) {
    (void)lfs;
    return dir->pos;
}

/** @brief 回到目录开头：重新 fetch 头块并清零 id/pos */
static int lfs_dir_rewind_(lfs_t *lfs, lfs_dir_t *dir) {
    // reload the head dir
    int err = lfs_dir_fetch(lfs, &dir->m, dir->head);
    if (err) {
        return err;
    }

    dir->id = 0;
    dir->pos = 0;
    return 0;
}


/// File index list operations ///
/**
 * @brief CTZ 跳表核心数学：由文件内偏移求 (块序号, 块内偏移)
 * @note  第 n 块含 ctz(n)+1 个指针，平均每块 2 指针（几何级数）。
 *        利用恒等式 sum_{i<=n}(ctz(i)+1) = 2n - popcount(n)（OEIS
 *        A001511/A005187），把 O(n) 求和折成 O(1)：先以 (B-8) 估
 *        n'，再用 popcount 修正一次即精确（DESIGN.md 公式 6/7）。
 *        这是"文件元数据只需 head+size 两个 32 位数"的全部代价。
 */
static int lfs_ctz_index(lfs_t *lfs, lfs_off_t *off) {
    lfs_off_t size = *off;
    lfs_off_t b = lfs->cfg->block_size - 2*4;
    lfs_off_t i = size / b;
    if (i == 0) {
        return 0;
    }

    i = (size - 4*(lfs_popc(i-1)+2)) / b;
    *off = size - b*i - 4*lfs_popc(i);
    return i;
}

/**
 * @brief CTZ 跳表查询：定位 pos 所在数据块（O(log n) 次读）
 * @note  贪心跳跃：每次选"不超过目标的最大跳距"，与二分等价；
 *        从尾部 head 往文件开头跳——backwards 链表 + 多层指针
 *        把顺读从 O(n^2) 压到 O(n log n) 的地方就在这里。
 */
static int lfs_ctz_find(lfs_t *lfs,
        const lfs_cache_t *pcache, lfs_cache_t *rcache,
        lfs_block_t head, lfs_size_t size,
        lfs_size_t pos, lfs_block_t *block, lfs_off_t *off) {
    if (size == 0) {
        *block = LFS_BLOCK_NULL;
        *off = 0;
        return 0;
    }

    lfs_off_t current = lfs_ctz_index(lfs, &(lfs_off_t){size-1});
    lfs_off_t target = lfs_ctz_index(lfs, &pos);

    while (current > target) {
        // 跳距 = min(能覆盖目标的 2^k, 当前块实际可用的指针数)
        lfs_size_t skip = lfs_min(
                lfs_npw2(current-target+1) - 1,
                lfs_ctz(current));

        int err = lfs_bd_read(lfs,
                pcache, rcache, sizeof(head),
                head, 4*skip, &head, sizeof(head));
        head = lfs_fromle32(head);
        if (err) {
            return err;
        }

        current -= 1 << skip;
    }

    *block = head;
    *off = pos;
    return 0;
}

#ifndef LFS_READONLY
/**
 * @brief CTZ 跳表追加一块：分配 → 擦除 → 写 skips 个指针 → 返回数据偏移
 * @param  block/off 输出：新数据块的写入位置
 * @note  尾块不满时先把旧尾块数据原样搬进新块（COW），再补指针：
 *        第 index 块写 ctz(index)+1 个 4 字节指针，指向 index-1、
 *        index-1-2^0、index-1-2^1……追加写永不触碰旧链——
 *        littlefs 文件追加无写放大、掉电只丢新块的根源。
 *        坏块直接换块重试（RAM 里什么都没丢）。
 */
static int lfs_ctz_extend(lfs_t *lfs,
        lfs_cache_t *pcache, lfs_cache_t *rcache,
        lfs_block_t head, lfs_size_t size,
        lfs_block_t *block, lfs_off_t *off) {
    while (true) {
        // go ahead and grab a block
        lfs_block_t nblock;
        int err = lfs_alloc(lfs, &nblock);
        if (err) {
            return err;
        }

        {
            err = lfs_bd_erase(lfs, nblock);
            if (err) {
                if (err == LFS_ERR_CORRUPT) {
                    goto relocate;
                }
                return err;
            }

            if (size == 0) {
                *block = nblock;
                *off = 0;
                return 0;
            }

            lfs_size_t noff = size - 1;
            lfs_off_t index = lfs_ctz_index(lfs, &noff);
            noff = noff + 1;

            // just copy out the last block if it is incomplete
            if (noff != lfs->cfg->block_size) {
                for (lfs_off_t i = 0; i < noff; i++) {
                    uint8_t data;
                    err = lfs_bd_read(lfs,
                            NULL, rcache, noff-i,
                            head, i, &data, 1);
                    if (err) {
                        return err;
                    }

                    err = lfs_bd_prog(lfs,
                            pcache, rcache, true,
                            nblock, i, &data, 1);
                    if (err) {
                        if (err == LFS_ERR_CORRUPT) {
                            goto relocate;
                        }
                        return err;
                    }
                }

                *block = nblock;
                *off = noff;
                return 0;
            }

            // 指针区：第 index 块共 ctz(index)+1 个指针，从低层到高层
            // 依次写 nhead 链；写一个读一个（沿旧跳表向下走）
            // append block
            index += 1;
            lfs_size_t skips = lfs_ctz(index) + 1;
            lfs_block_t nhead = head;
            for (lfs_off_t i = 0; i < skips; i++) {
                nhead = lfs_tole32(nhead);
                err = lfs_bd_prog(lfs, pcache, rcache, true,
                        nblock, 4*i, &nhead, 4);
                nhead = lfs_fromle32(nhead);
                if (err) {
                    if (err == LFS_ERR_CORRUPT) {
                        goto relocate;
                    }
                    return err;
                }

                if (i != skips-1) {
                    err = lfs_bd_read(lfs,
                            NULL, rcache, sizeof(nhead),
                            nhead, 4*i, &nhead, sizeof(nhead));
                    nhead = lfs_fromle32(nhead);
                    if (err) {
                        return err;
                    }
                }
            }

            *block = nblock;
            *off = 4*skips;
            return 0;
        }

relocate:
        LFS_DEBUG("Bad block at 0x%"PRIx32, nblock);

        // just clear cache and try a new block
        lfs_cache_drop(lfs, pcache);
    }
}
#endif

/**
 * @brief 从尾到头遍历 CTZ 跳表全部数据块（fs_traverse / 统计用）
 * @note  利用跳表结构分治下降：count = 2-(index&1)，每步沿最大指针
 *        下潜并累加沿途块，O(n) 访问全部块且 RAM O(1)。
 */
static int lfs_ctz_traverse(lfs_t *lfs,
        const lfs_cache_t *pcache, lfs_cache_t *rcache,
        lfs_block_t head, lfs_size_t size,
        int (*cb)(void*, lfs_block_t), void *data) {
    if (size == 0) {
        return 0;
    }

    lfs_off_t index = lfs_ctz_index(lfs, &(lfs_off_t){size-1});

    while (true) {
        int err = cb(data, head);
        if (err) {
            return err;
        }

        if (index == 0) {
            return 0;
        }

        lfs_block_t heads[2];
        int count = 2 - (index & 1);
        err = lfs_bd_read(lfs,
                pcache, rcache, count*sizeof(head),
                head, 0, &heads, count*sizeof(head));
        heads[0] = lfs_fromle32(heads[0]);
        heads[1] = lfs_fromle32(heads[1]);
        if (err) {
            return err;
        }

        for (int i = 0; i < count-1; i++) {
            err = cb(data, heads[i]);
            if (err) {
                return err;
            }
        }

        head = heads[count-1];
        index -= count;
    }
}


/// Top level file operations ///
/**
 * @brief 打开文件的实现：路径解析 → 建条目/读元数据 → 载属性 → 备缓存
 * @param  flags LFS_O_* 组合；LFS_O_CREAT 才允许新建
 * @return 0 成功；失败清资源并置 LFS_F_ERRED
 * @note  写打开前先 forceconsistency（首次写前清孤儿/完成 move）；
 *        新文件以 INLINESTRUCT 落地（先内联，超过 inline_max 才 outline）；
 *        LFS_O_TRUNC 只在 RAM 里改 tag，等 sync 时一并原子提交；
 *        LFS_O_EXCL 与已有条目冲突时报 LFS_ERR_EXIST。
 */
static int lfs_file_opencfg_(lfs_t *lfs, lfs_file_t *file,
        const char *path, int flags,
        const struct lfs_file_config *cfg) {
#ifndef LFS_READONLY
    // deorphan if we haven't yet, needed at most once after poweron
    if ((flags & LFS_O_WRONLY) == LFS_O_WRONLY) {
        int err = lfs_fs_forceconsistency(lfs);
        if (err) {
            return err;
        }
    }
#else
    LFS_ASSERT((flags & LFS_O_RDONLY) == LFS_O_RDONLY);
#endif

    // setup simple file details
    int err;
    file->cfg = cfg;
    file->flags = flags;
    file->pos = 0;
    file->off = 0;
    file->cache.buffer = NULL;

    // allocate entry for file if it doesn't exist
    lfs_stag_t tag = lfs_dir_find(lfs, &file->m, &path, &file->id);
    if (tag < 0 && !(tag == LFS_ERR_NOENT && lfs_path_islast(path))) {
        err = tag;
        goto cleanup;
    }

    // get id, add to list of mdirs to catch update changes
    file->type = LFS_TYPE_REG;
    lfs_mlist_append(lfs, (struct lfs_mlist *)file);

#ifdef LFS_READONLY
    if (tag == LFS_ERR_NOENT) {
        err = LFS_ERR_NOENT;
        goto cleanup;
#else
    if (tag == LFS_ERR_NOENT) {
        if (!(flags & LFS_O_CREAT)) {
            err = LFS_ERR_NOENT;
            goto cleanup;
        }

        // don't allow trailing slashes
        if (lfs_path_isdir(path)) {
            err = LFS_ERR_NOTDIR;
            goto cleanup;
        }

        // check that name fits
        lfs_size_t nlen = lfs_path_namelen(path);
        if (nlen > lfs->name_max) {
            err = LFS_ERR_NAMETOOLONG;
            goto cleanup;
        }

        // get next slot and create entry to remember name
        err = lfs_dir_commit(lfs, &file->m, LFS_MKATTRS(
                {LFS_MKTAG(LFS_TYPE_CREATE, file->id, 0), NULL},
                {LFS_MKTAG(LFS_TYPE_REG, file->id, nlen), path},
                {LFS_MKTAG(LFS_TYPE_INLINESTRUCT, file->id, 0), NULL}));

        // it may happen that the file name doesn't fit in the metadata blocks, e.g., a 256 byte file name will
        // not fit in a 128 byte block.
        err = (err == LFS_ERR_NOSPC) ? LFS_ERR_NAMETOOLONG : err;
        if (err) {
            goto cleanup;
        }

        tag = LFS_MKTAG(LFS_TYPE_INLINESTRUCT, 0, 0);
    } else if (flags & LFS_O_EXCL) {
        err = LFS_ERR_EXIST;
        goto cleanup;
#endif
    } else if (lfs_tag_type3(tag) != LFS_TYPE_REG) {
        err = LFS_ERR_ISDIR;
        goto cleanup;
#ifndef LFS_READONLY
    } else if (flags & LFS_O_TRUNC) {
        // truncate if requested
        tag = LFS_MKTAG(LFS_TYPE_INLINESTRUCT, file->id, 0);
        file->flags |= LFS_F_DIRTY;
#endif
    } else {
        // try to load what's on disk, if it's inlined we'll fix it later
        tag = lfs_dir_get(lfs, &file->m, LFS_MKTAG(0x700, 0x3ff, 0),
                LFS_MKTAG(LFS_TYPE_STRUCT, file->id, 8), &file->ctz);
        if (tag < 0) {
            err = tag;
            goto cleanup;
        }
        lfs_ctz_fromle32(&file->ctz);
    }

    // fetch attrs
    for (unsigned i = 0; i < file->cfg->attr_count; i++) {
        // if opened for read / read-write operations
        if ((file->flags & LFS_O_RDONLY) == LFS_O_RDONLY) {
            lfs_stag_t res = lfs_dir_get(lfs, &file->m,
                    LFS_MKTAG(0x7ff, 0x3ff, 0),
                    LFS_MKTAG(LFS_TYPE_USERATTR + file->cfg->attrs[i].type,
                        file->id, file->cfg->attrs[i].size),
                        file->cfg->attrs[i].buffer);
            if (res < 0 && res != LFS_ERR_NOENT) {
                err = res;
                goto cleanup;
            }
        }

#ifndef LFS_READONLY
        // if opened for write / read-write operations
        if ((file->flags & LFS_O_WRONLY) == LFS_O_WRONLY) {
            if (file->cfg->attrs[i].size > lfs->attr_max) {
                err = LFS_ERR_NOSPC;
                goto cleanup;
            }

            file->flags |= LFS_F_DIRTY;
        }
#endif
    }

    // allocate buffer if needed
    if (file->cfg->buffer) {
        file->cache.buffer = file->cfg->buffer;
    } else {
        file->cache.buffer = lfs_malloc(lfs->cfg->cache_size);
        if (!file->cache.buffer) {
            err = LFS_ERR_NOMEM;
            goto cleanup;
        }
    }

    // zero to avoid information leak
    lfs_cache_zero(lfs, &file->cache);

    if (lfs_tag_type3(tag) == LFS_TYPE_INLINESTRUCT) {
        // load inline files
        file->ctz.head = LFS_BLOCK_INLINE;
        file->ctz.size = lfs_tag_size(tag);
        file->flags |= LFS_F_INLINE;
        file->cache.block = file->ctz.head;
        file->cache.off = 0;
        file->cache.size = lfs->cfg->cache_size;

        // don't always read (may be new/trunc file)
        if (file->ctz.size > 0) {
            lfs_stag_t res = lfs_dir_get(lfs, &file->m,
                    LFS_MKTAG(0x700, 0x3ff, 0),
                    LFS_MKTAG(LFS_TYPE_STRUCT, file->id,
                        lfs_min(file->cache.size, 0x3fe)),
                    file->cache.buffer);
            if (res < 0) {
                err = res;
                goto cleanup;
            }
        }
    }

    return 0;

cleanup:
    // clean up lingering resources
#ifndef LFS_READONLY
    file->flags |= LFS_F_ERRED;
#endif
    lfs_file_close_(lfs, file);
    return err;
}

#ifndef LFS_NO_MALLOC
/** @brief 无额外配置的打开：默认 lfs_file_config（缓存走 malloc） */
static int lfs_file_open_(lfs_t *lfs, lfs_file_t *file,
        const char *path, int flags) {
    static const struct lfs_file_config defaults = {0};
    int err = lfs_file_opencfg_(lfs, file, path, flags, &defaults);
    return err;
}
#endif

/**
 * @brief 关闭文件：sync 提交元数据 → 摘 mlist → 释放缓存
 * @note  LFS_F_ERRED（写过程中出错）时跳过 sync——半截数据绝不提交，
 *        掉电安全在单次调用的错误路径里同样成立。
 */
static int lfs_file_close_(lfs_t *lfs, lfs_file_t *file) {
    int err = 0;
#ifndef LFS_READONLY
    // it's not safe to do anything if our file errored
    if (!(file->flags & LFS_F_ERRED)) {
        err = lfs_file_sync_(lfs, file);
    }
#endif

    // remove from list of mdirs
    lfs_mlist_remove(lfs, (struct lfs_mlist*)file);

    // clean up memory
    if (!file->cfg->buffer) {
        lfs_free(file->cache.buffer);
    }

    return err;
}


#ifndef LFS_READONLY
/**
 * @brief 把"正在写"的文件数据搬到新块（坏块恢复 / inline 转 CTZ）
 * @note  逐字节经缓存搬运，源可以是脏缓存（未冲刷的新数据）或盘；
 *        搬完把 pcache 内容直接接管为 file->cache，省一次重读。
 */
static int lfs_file_relocate(lfs_t *lfs, lfs_file_t *file) {
    while (true) {
        // just relocate what exists into new block
        lfs_block_t nblock;
        int err = lfs_alloc(lfs, &nblock);
        if (err) {
            return err;
        }

        err = lfs_bd_erase(lfs, nblock);
        if (err) {
            if (err == LFS_ERR_CORRUPT) {
                goto relocate;
            }
            return err;
        }

        // either read from dirty cache or disk
        for (lfs_off_t i = 0; i < file->off; i++) {
            uint8_t data;
            if (file->flags & LFS_F_INLINE) {
                err = lfs_dir_getread(lfs, &file->m,
                        // note we evict inline files before they can be dirty
                        NULL, &file->cache, file->off-i,
                        LFS_MKTAG(0xfff, 0x1ff, 0),
                        LFS_MKTAG(LFS_TYPE_INLINESTRUCT, file->id, 0),
                        i, &data, 1);
                if (err) {
                    return err;
                }
            } else {
                err = lfs_bd_read(lfs,
                        &file->cache, &lfs->rcache, file->off-i,
                        file->block, i, &data, 1);
                if (err) {
                    return err;
                }
            }

            err = lfs_bd_prog(lfs,
                    &lfs->pcache, &lfs->rcache, true,
                    nblock, i, &data, 1);
            if (err) {
                if (err == LFS_ERR_CORRUPT) {
                    goto relocate;
                }
                return err;
            }
        }

        // copy over new state of file
        memcpy(file->cache.buffer, lfs->pcache.buffer, lfs->cfg->cache_size);
        file->cache.block = lfs->pcache.block;
        file->cache.off = lfs->pcache.off;
        file->cache.size = lfs->pcache.size;
        lfs_cache_zero(lfs, &lfs->pcache);

        file->block = nblock;
        file->flags |= LFS_F_WRITING;
        return 0;

relocate:
        LFS_DEBUG("Bad block at 0x%"PRIx32, nblock);

        // just clear cache and try a new block
        lfs_cache_drop(lfs, &lfs->pcache);
    }
}
#endif

#ifndef LFS_READONLY
/** @brief inline 文件转 CTZ 文件：数据搬出元数据对，独立成块链 */
static int lfs_file_outline(lfs_t *lfs, lfs_file_t *file) {
    file->off = file->pos;
    lfs_alloc_ckpoint(lfs);
    int err = lfs_file_relocate(lfs, file);
    if (err) {
        return err;
    }

    file->flags &= ~LFS_F_INLINE;
    return 0;
}
#endif

/**
 * @brief 把文件的 RAM 状态与盘对齐：丢弃读缓存 / 落写缓存
 * @note  写路径（CTZ 文件）：先从旧 ctz 把 pos 之后的数据逐字节拷到
 *        新分支（COW 下旧数据仍完整可读），再冲刷缓存；全部完成后
 *        ctz.head/size 才指向新分支——在此之前掉电，元数据未提交，
 *        文件仍是旧版本（掉电一致性窗口被压到"一次元数据提交"）。
 */
static int lfs_file_flush(lfs_t *lfs, lfs_file_t *file) {
    if (file->flags & LFS_F_READING) {
        if (!(file->flags & LFS_F_INLINE)) {
            lfs_cache_drop(lfs, &file->cache);
        }
        file->flags &= ~LFS_F_READING;
    }

#ifndef LFS_READONLY
    if (file->flags & LFS_F_WRITING) {
        lfs_off_t pos = file->pos;

        if (!(file->flags & LFS_F_INLINE)) {
            // copy over anything after current branch
            lfs_file_t orig = {
                .ctz.head = file->ctz.head,
                .ctz.size = file->ctz.size,
                .flags = LFS_O_RDONLY,
                .pos = file->pos,
                .cache = lfs->rcache,
            };
            lfs_cache_drop(lfs, &lfs->rcache);

            while (file->pos < file->ctz.size) {
                // copy over a byte at a time, leave it up to caching
                // to make this efficient
                uint8_t data;
                lfs_ssize_t res = lfs_file_flushedread(lfs, &orig, &data, 1);
                if (res < 0) {
                    return res;
                }

                res = lfs_file_flushedwrite(lfs, file, &data, 1);
                if (res < 0) {
                    return res;
                }

                // keep our reference to the rcache in sync
                if (lfs->rcache.block != LFS_BLOCK_NULL) {
                    lfs_cache_drop(lfs, &orig.cache);
                    lfs_cache_drop(lfs, &lfs->rcache);
                }
            }

            // write out what we have
            while (true) {
                int err = lfs_bd_flush(lfs, &file->cache, &lfs->rcache, true);
                if (err) {
                    if (err == LFS_ERR_CORRUPT) {
                        goto relocate;
                    }
                    return err;
                }

                break;

relocate:
                LFS_DEBUG("Bad block at 0x%"PRIx32, file->block);
                err = lfs_file_relocate(lfs, file);
                if (err) {
                    return err;
                }
            }
        } else {
            file->pos = lfs_max(file->pos, file->ctz.size);
        }

        // actual file updates
        file->ctz.head = file->block;
        file->ctz.size = file->pos;
        file->flags &= ~LFS_F_WRITING;
        file->flags |= LFS_F_DIRTY;

        file->pos = pos;
    }
#endif

    return 0;
}

#ifndef LFS_READONLY
/**
 * @brief 文件提交：flush 数据 → sync 落盘 → 提交元数据（ctz/inline + attrs）
 * @note  顺序是掉电一致性的关键：数据必须先于元数据物理落盘，
 *        否则元数据可能指向没写完的块；元数据提交成功才清
 *        LFS_F_DIRTY。同一文件的其他打开句柄被标记 LFS_F_DUSTY，
 *        下次操作时强制重新同步。
 */
static int lfs_file_sync_(lfs_t *lfs, lfs_file_t *file) {
    int err = lfs_file_flush(lfs, file);
    if (err) {
        file->flags |= LFS_F_ERRED;
        return err;
    }

    if ((file->flags & LFS_F_DIRTY) &&
            !lfs_pair_isnull(file->m.pair)) {
        // before we commit metadata, we need sync the disk to make sure
        // data writes don't complete after metadata writes
        if (!(file->flags & LFS_F_INLINE)) {
            err = lfs_bd_sync(lfs, &lfs->pcache, &lfs->rcache, false);
            if (err) {
                return err;
            }
        }

        // update dir entry
        uint16_t type;
        const void *buffer;
        lfs_size_t size;
        struct lfs_ctz ctz;
        if (file->flags & LFS_F_INLINE) {
            // inline the whole file
            type = LFS_TYPE_INLINESTRUCT;
            buffer = file->cache.buffer;
            size = file->ctz.size;
        } else {
            // update the ctz reference
            type = LFS_TYPE_CTZSTRUCT;
            // copy ctz so alloc will work during a relocate
            ctz = file->ctz;
            lfs_ctz_tole32(&ctz);
            buffer = &ctz;
            size = sizeof(ctz);
        }

        // commit file data and attributes
        err = lfs_dir_commit(lfs, &file->m, LFS_MKATTRS(
                {LFS_MKTAG(type, file->id, size), buffer},
                {LFS_MKTAG(LFS_FROM_USERATTRS, file->id,
                    file->cfg->attr_count), file->cfg->attrs}));
        if (err) {
            file->flags |= LFS_F_ERRED;
            return err;
        }

        file->flags &= ~LFS_F_DIRTY;
    }

    // mark any other file handles as dirty + desync
    for (lfs_file_t *f = (lfs_file_t*)lfs->mlist; f; f = f->next) {
        if (file != f
                && f->type == LFS_TYPE_REG
                && lfs_pair_cmp(f->m.pair, file->m.pair) == 0
                && f->id == file->id) {
            f->flags |= LFS_F_DUSTY;
        }
    }

    file->flags &= ~LFS_F_ERRED & ~LFS_F_DUSTY;
    return 0;
}
#endif

/**
 * @brief 读的执行体：定位块 → 经缓存拷出，跨块循环
 * @note  inline 文件从元数据对切片读（lfs_dir_getread）；CTZ 文件由
 *        ctz_find 定位后 bd_read。pos 越过 ctz.size 返回 0（EOF）。
 */
static lfs_ssize_t lfs_file_flushedread(lfs_t *lfs, lfs_file_t *file,
        void *buffer, lfs_size_t size) {
    uint8_t *data = buffer;
    lfs_size_t nsize = size;

    if (file->pos >= file->ctz.size) {
        // eof if past end
        return 0;
    }

    size = lfs_min(size, file->ctz.size - file->pos);
    nsize = size;

    while (nsize > 0) {
        // check if we need a new block
        if (!(file->flags & LFS_F_READING) ||
                file->off == lfs->cfg->block_size) {
            if (!(file->flags & LFS_F_INLINE)) {
                int err = lfs_ctz_find(lfs, NULL, &file->cache,
                        file->ctz.head, file->ctz.size,
                        file->pos, &file->block, &file->off);
                if (err) {
                    return err;
                }
            } else {
                file->block = LFS_BLOCK_INLINE;
                file->off = file->pos;
            }

            file->flags |= LFS_F_READING;
        }

        // read as much as we can in current block
        lfs_size_t diff = lfs_min(nsize, lfs->cfg->block_size - file->off);
        if (file->flags & LFS_F_INLINE) {
            int err = lfs_dir_getread(lfs, &file->m,
                    NULL, &file->cache, lfs->cfg->block_size,
                    LFS_MKTAG(0xfff, 0x1ff, 0),
                    LFS_MKTAG(LFS_TYPE_INLINESTRUCT, file->id, 0),
                    file->off, data, diff);
            if (err) {
                return err;
            }
        } else {
            int err = lfs_bd_read(lfs,
                    NULL, &file->cache, lfs->cfg->block_size,
                    file->block, file->off, data, diff);
            if (err) {
                return err;
            }
        }

        file->pos += diff;
        file->off += diff;
        data += diff;
        nsize -= diff;
    }

    return size;
}

/** @brief 读入口：若正在写先 flush（读写在同一句柄上互斥的约定） */
static lfs_ssize_t lfs_file_read_(lfs_t *lfs, lfs_file_t *file,
        void *buffer, lfs_size_t size) {
    LFS_ASSERT((file->flags & LFS_O_RDONLY) == LFS_O_RDONLY);

#ifndef LFS_READONLY
    if (file->flags & LFS_F_WRITING) {
        // flush out any writes
        int err = lfs_file_flush(lfs, file);
        if (err) {
            return err;
        }
    }
#endif

    return lfs_file_flushedread(lfs, file, buffer, size);
}


#ifndef LFS_READONLY
/**
 * @brief 写的执行体：inline 超限先 outline；定位/追加块；经缓存写入
 * @note  追加场景只碰尾块（CObW：整条 CTZ 链无需重写，文件写入
 *        零写放大的原因）；seek 写（pos < ctz.size）从 pos-1 定位
 *        尾块继续写，其后旧数据会在 flush 时被逐字节拷到新分支。
 */
static lfs_ssize_t lfs_file_flushedwrite(lfs_t *lfs, lfs_file_t *file,
        const void *buffer, lfs_size_t size) {
    const uint8_t *data = buffer;
    lfs_size_t nsize = size;

    // inline 容量上限（默认 min(cache_size, attr_max, 块/8)）：超过即
    // 转 CTZ 文件，避免元数据对被单文件撑爆
    if ((file->flags & LFS_F_INLINE) &&
            lfs_max(file->pos+nsize, file->ctz.size) > lfs->inline_max) {
        // inline file doesn't fit anymore
        int err = lfs_file_outline(lfs, file);
        if (err) {
            file->flags |= LFS_F_ERRED;
            return err;
        }
    }

    while (nsize > 0) {
        // check if we need a new block
        if (!(file->flags & LFS_F_WRITING) ||
                file->off == lfs->cfg->block_size) {
            if (!(file->flags & LFS_F_INLINE)) {
                if (!(file->flags & LFS_F_WRITING) && file->pos > 0) {
                    // find out which block we're extending from
                    int err = lfs_ctz_find(lfs, NULL, &file->cache,
                            file->ctz.head, file->ctz.size,
                            file->pos-1, &file->block, &(lfs_off_t){0});
                    if (err) {
                        file->flags |= LFS_F_ERRED;
                        return err;
                    }

                    // mark cache as dirty since we may have read data into it
                    lfs_cache_zero(lfs, &file->cache);
                }

                // extend file with new blocks
                lfs_alloc_ckpoint(lfs);
                int err = lfs_ctz_extend(lfs, &file->cache, &lfs->rcache,
                        file->block, file->pos,
                        &file->block, &file->off);
                if (err) {
                    file->flags |= LFS_F_ERRED;
                    return err;
                }
            } else {
                file->block = LFS_BLOCK_INLINE;
                file->off = file->pos;
            }

            file->flags |= LFS_F_WRITING;
        }

        // program as much as we can in current block
        lfs_size_t diff = lfs_min(nsize, lfs->cfg->block_size - file->off);
        while (true) {
            int err = lfs_bd_prog(lfs, &file->cache, &lfs->rcache, true,
                    file->block, file->off, data, diff);
            if (err) {
                if (err == LFS_ERR_CORRUPT) {
                    goto relocate;
                }
                file->flags |= LFS_F_ERRED;
                return err;
            }

            break;
relocate:
            err = lfs_file_relocate(lfs, file);
            if (err) {
                file->flags |= LFS_F_ERRED;
                return err;
            }
        }

        file->pos += diff;
        file->off += diff;
        data += diff;
        nsize -= diff;

        lfs_alloc_ckpoint(lfs);
    }

    return size;
}

/**
 * @brief 写入口：READ 中先 flush；APPEND 跳到末尾；file_max 拒绝超限
 * @note  pos > size 的"稀疏写"用逐字节 0 填洞（littlefs 不做稀疏
 *        存储，洞就是真实的 0x00 数据）；成功才清 ERRED/DUSTY。
 */
static lfs_ssize_t lfs_file_write_(lfs_t *lfs, lfs_file_t *file,
        const void *buffer, lfs_size_t size) {
    LFS_ASSERT((file->flags & LFS_O_WRONLY) == LFS_O_WRONLY);

    if (file->flags & LFS_F_READING) {
        // drop any reads
        int err = lfs_file_flush(lfs, file);
        if (err) {
            return err;
        }
    }

    if ((file->flags & LFS_O_APPEND) && file->pos < file->ctz.size) {
        file->pos = file->ctz.size;
    }

    if (file->pos + size > lfs->file_max) {
        // Larger than file limit?
        return LFS_ERR_FBIG;
    }

    if (!(file->flags & LFS_F_WRITING) && file->pos > file->ctz.size) {
        // fill with zeros
        lfs_off_t pos = file->pos;
        file->pos = file->ctz.size;

        while (file->pos < pos) {
            lfs_ssize_t res = lfs_file_flushedwrite(lfs, file, &(uint8_t){0}, 1);
            if (res < 0) {
                return res;
            }
        }
    }

    lfs_ssize_t nsize = lfs_file_flushedwrite(lfs, file, buffer, size);
    if (nsize < 0) {
        return nsize;
    }

    file->flags &= ~LFS_F_ERRED & ~LFS_F_DUSTY;
    return nsize;
}
#endif

/**
 * @brief 重定位：同块内且缓存覆盖目标偏移时直接改 off（免 flush）；
 *        否则 flush 后改 pos。31 位文件尺寸上限保证无符号回绕烦恼。
 */
static lfs_soff_t lfs_file_seek_(lfs_t *lfs, lfs_file_t *file,
        lfs_soff_t off, int whence) {
    // find new pos
    //
    // fortunately for us, littlefs is limited to 31-bit file sizes, so we
    // don't have to worry too much about integer overflow
    lfs_off_t npos = file->pos;
    if (whence == LFS_SEEK_SET) {
        npos = off;
    } else if (whence == LFS_SEEK_CUR) {
        npos = file->pos + (lfs_off_t)off;
    } else if (whence == LFS_SEEK_END) {
        npos = (lfs_off_t)lfs_file_size_(lfs, file) + (lfs_off_t)off;
    }

    if (npos > lfs->file_max) {
        // file position out of range
        return LFS_ERR_INVAL;
    }

    if (file->pos == npos) {
        // noop - position has not changed
        return npos;
    }

    // if we're only reading and our new offset is still in the file's cache
    // we can avoid flushing and needing to reread the data
    if ((file->flags & LFS_F_READING)
            && file->off != lfs->cfg->block_size) {
        int oindex = lfs_ctz_index(lfs, &(lfs_off_t){file->pos});
        lfs_off_t noff = npos;
        int nindex = lfs_ctz_index(lfs, &noff);
        if (oindex == nindex
                && noff >= file->cache.off
                && noff < file->cache.off + file->cache.size) {
            file->pos = npos;
            file->off = noff;
            return npos;
        }
    }

    // write out everything beforehand, may be noop if rdonly
    int err = lfs_file_flush(lfs, file);
    if (err) {
        return err;
    }

    // update pos
    file->pos = npos;
    return npos;
}

#ifndef LFS_READONLY
/**
 * @brief 截断：缩小到 ≤inline_max 时退回 inline（省空间）；
 *        否则 ctz_find 定位新尾改 head/size；扩大则补 0
 * @note  截掉的尾块不擦除（没有空闲链表），等搬迁/复用时自然回收；
 *        收缩/扩张后都恢复原 pos，保持 POSIX truncate 语义。
 */
static int lfs_file_truncate_(lfs_t *lfs, lfs_file_t *file, lfs_off_t size) {
    LFS_ASSERT((file->flags & LFS_O_WRONLY) == LFS_O_WRONLY);

    if (size > LFS_FILE_MAX) {
        return LFS_ERR_INVAL;
    }

    lfs_off_t pos = file->pos;
    lfs_off_t oldsize = lfs_file_size_(lfs, file);
    if (size < oldsize) {
        // revert to inline file?
        if (size <= lfs->inline_max) {
            // flush+seek to head
            lfs_soff_t res = lfs_file_seek_(lfs, file, 0, LFS_SEEK_SET);
            if (res < 0) {
                return (int)res;
            }

            // read our data into rcache temporarily
            lfs_cache_drop(lfs, &lfs->rcache);
            res = lfs_file_flushedread(lfs, file,
                    lfs->rcache.buffer, size);
            if (res < 0) {
                return (int)res;
            }

            file->ctz.head = LFS_BLOCK_INLINE;
            file->ctz.size = size;
            file->flags |= LFS_F_DIRTY | LFS_F_READING | LFS_F_INLINE;
            file->cache.block = file->ctz.head;
            file->cache.off = 0;
            file->cache.size = lfs->cfg->cache_size;
            memcpy(file->cache.buffer, lfs->rcache.buffer, size);

        } else {
            // need to flush since directly changing metadata
            int err = lfs_file_flush(lfs, file);
            if (err) {
                return err;
            }

            // lookup new head in ctz skip list
            err = lfs_ctz_find(lfs, NULL, &file->cache,
                    file->ctz.head, file->ctz.size,
                    size-1, &file->block, &(lfs_off_t){0});
            if (err) {
                return err;
            }

            // need to set pos/block/off consistently so seeking back to
            // the old position does not get confused
            file->pos = size;
            file->ctz.head = file->block;
            file->ctz.size = size;
            file->flags |= LFS_F_DIRTY | LFS_F_READING;
        }
    } else if (size > oldsize) {
        // flush+seek if not already at end
        lfs_soff_t res = lfs_file_seek_(lfs, file, 0, LFS_SEEK_END);
        if (res < 0) {
            return (int)res;
        }

        // fill with zeros
        while (file->pos < size) {
            res = lfs_file_write_(lfs, file, &(uint8_t){0}, 1);
            if (res < 0) {
                return (int)res;
            }
        }
    }

    // restore pos
    lfs_soff_t res = lfs_file_seek_(lfs, file, pos, LFS_SEEK_SET);
    if (res < 0) {
      return (int)res;
    }

    return 0;
}
#endif

/** @brief 返回当前读写位置（RAM 内的 file->pos，无 IO） */
static lfs_soff_t lfs_file_tell_(lfs_t *lfs, lfs_file_t *file) {
    (void)lfs;
    return file->pos;
}

/** @brief 回到文件头（= seek 到 0） */
static int lfs_file_rewind_(lfs_t *lfs, lfs_file_t *file) {
    lfs_soff_t res = lfs_file_seek_(lfs, file, 0, LFS_SEEK_SET);
    if (res < 0) {
        return (int)res;
    }

    return 0;
}

/**
 * @brief 文件大小：WRITING 时取 max(pos, ctz.size)——RAM 视角已含
 *        未提交的写入；否则直接返回元数据里的 ctz.size。
 */
static lfs_soff_t lfs_file_size_(lfs_t *lfs, lfs_file_t *file) {
    (void)lfs;

#ifndef LFS_READONLY
    if (file->flags & LFS_F_WRITING) {
        return lfs_max(file->pos, file->ctz.size);
    }
#endif

    return file->ctz.size;
}


/// General fs operations ///
/** @brief stat：dir_find 后取 info；文件路径带尾 '/' 报 NOTDIR */
static int lfs_stat_(lfs_t *lfs, const char *path, struct lfs_info *info) {
    lfs_mdir_t cwd;
    lfs_stag_t tag = lfs_dir_find(lfs, &cwd, &path, NULL);
    if (tag < 0) {
        return (int)tag;
    }

    // only allow trailing slashes on dirs
    if (strchr(path, '/') != NULL
            && lfs_tag_type3(tag) != LFS_TYPE_DIR) {
        return LFS_ERR_NOTDIR;
    }

    return lfs_dir_getinfo(lfs, &cwd, lfs_tag_id(tag), info);
}

#ifndef LFS_READONLY
/**
 * @brief 删除文件/空目录：写 DELETE tag；目录则先造孤儿再断链
 * @note  目录删除时序：+1 孤儿 → 父目录 DELETE → 前驱对 drop tail
 *        （-1 孤儿）。中间任何时刻掉电，gstate 孤儿计数都能让
 *        deorphan 把链修好——"用合法中间态换原子性"的样板。
 */
static int lfs_remove_(lfs_t *lfs, const char *path) {
    // deorphan if we haven't yet, needed at most once after poweron
    int err = lfs_fs_forceconsistency(lfs);
    if (err) {
        return err;
    }

    lfs_mdir_t cwd;
    lfs_stag_t tag = lfs_dir_find(lfs, &cwd, &path, NULL);
    if (tag < 0 || lfs_tag_id(tag) == 0x3ff) {
        return (tag < 0) ? (int)tag : LFS_ERR_INVAL;
    }

    struct lfs_mlist dir;
    dir.next = lfs->mlist;
    if (lfs_tag_type3(tag) == LFS_TYPE_DIR) {
        // must be empty before removal
        lfs_block_t pair[2];
        lfs_stag_t res = lfs_dir_get(lfs, &cwd, LFS_MKTAG(0x700, 0x3ff, 0),
                LFS_MKTAG(LFS_TYPE_STRUCT, lfs_tag_id(tag), 8), pair);
        if (res < 0) {
            return (int)res;
        }
        lfs_pair_fromle32(pair);

        err = lfs_dir_fetch(lfs, &dir.m, pair);
        if (err) {
            return err;
        }

        if (dir.m.count > 0 || dir.m.split) {
            return LFS_ERR_NOTEMPTY;
        }

        // mark fs as orphaned
        err = lfs_fs_preporphans(lfs, +1);
        if (err) {
            return err;
        }

        // I know it's crazy but yes, dir can be changed by our parent's
        // commit (if predecessor is child)
        dir.type = 0;
        dir.id = 0;
        lfs->mlist = &dir;
    }

    // delete the entry
    err = lfs_dir_commit(lfs, &cwd, LFS_MKATTRS(
            {LFS_MKTAG(LFS_TYPE_DELETE, lfs_tag_id(tag), 0), NULL}));
    if (err) {
        lfs->mlist = dir.next;
        return err;
    }

    lfs->mlist = dir.next;
    if (lfs_gstate_hasorphans(&lfs->gstate)) {
        LFS_ASSERT(lfs_tag_type3(tag) == LFS_TYPE_DIR);

        // fix orphan
        err = lfs_fs_preporphans(lfs, -1);
        if (err) {
            return err;
        }

        err = lfs_fs_pred(lfs, dir.m.pair, &cwd);
        if (err) {
            return err;
        }

        err = lfs_dir_drop(lfs, &cwd, &dir.m);
        if (err) {
            return err;
        }
    }

    return 0;
}
#endif

#ifndef LFS_READONLY
/**
 * @brief 重命名/移动：目标对写 CREATE+move 标记，源对写 DELETE
 * @note  跨元数据对时用 gstate 的 MOVESTATE 实现两段式原子：
 *        1) 目标对提交 {DELETE 旧, CREATE, NAME, FROM_MOVE} + gstate 记 move；
 *        2) 源对提交 DELETE + 清 gstate。掉电落在中间 => 挂载后
 *        demove 删源，效果等于已完成移动（文件绝不丢失/复制）。
 *        同对内退化为一次提交（含 DELETE 引起的 id 平移修正）。
 */
static int lfs_rename_(lfs_t *lfs, const char *oldpath, const char *newpath) {
    // deorphan if we haven't yet, needed at most once after poweron
    int err = lfs_fs_forceconsistency(lfs);
    if (err) {
        return err;
    }

    // find old entry
    lfs_mdir_t oldcwd;
    lfs_stag_t oldtag = lfs_dir_find(lfs, &oldcwd, &oldpath, NULL);
    if (oldtag < 0 || lfs_tag_id(oldtag) == 0x3ff) {
        return (oldtag < 0) ? (int)oldtag : LFS_ERR_INVAL;
    }

    // find new entry
    lfs_mdir_t newcwd;
    uint16_t newid;
    lfs_stag_t prevtag = lfs_dir_find(lfs, &newcwd, &newpath, &newid);
    if ((prevtag < 0 || lfs_tag_id(prevtag) == 0x3ff) &&
            !(prevtag == LFS_ERR_NOENT && lfs_path_islast(newpath))) {
        return (prevtag < 0) ? (int)prevtag : LFS_ERR_INVAL;
    }

    // if we're in the same pair there's a few special cases...
    bool samepair = (lfs_pair_cmp(oldcwd.pair, newcwd.pair) == 0);
    uint16_t newoldid = lfs_tag_id(oldtag);

    struct lfs_mlist prevdir;
    prevdir.next = lfs->mlist;
    if (prevtag == LFS_ERR_NOENT) {
        // if we're a file, don't allow trailing slashes
        if (lfs_path_isdir(newpath)
                && lfs_tag_type3(oldtag) != LFS_TYPE_DIR) {
            return LFS_ERR_NOTDIR;
        }

        // check that name fits
        lfs_size_t nlen = lfs_path_namelen(newpath);
        if (nlen > lfs->name_max) {
            return LFS_ERR_NAMETOOLONG;
        }

        // there is a small chance we are being renamed in the same
        // directory/ to an id less than our old id, the global update
        // to handle this is a bit messy
        if (samepair && newid <= newoldid) {
            newoldid += 1;
        }
    } else if (lfs_tag_type3(prevtag) != lfs_tag_type3(oldtag)) {
        return (lfs_tag_type3(prevtag) == LFS_TYPE_DIR)
                ? LFS_ERR_ISDIR
                : LFS_ERR_NOTDIR;
    } else if (samepair && newid == newoldid) {
        // we're renaming to ourselves??
        return 0;
    } else if (lfs_tag_type3(prevtag) == LFS_TYPE_DIR) {
        // must be empty before removal
        lfs_block_t prevpair[2];
        lfs_stag_t res = lfs_dir_get(lfs, &newcwd, LFS_MKTAG(0x700, 0x3ff, 0),
                LFS_MKTAG(LFS_TYPE_STRUCT, newid, 8), prevpair);
        if (res < 0) {
            return (int)res;
        }
        lfs_pair_fromle32(prevpair);

        // must be empty before removal
        err = lfs_dir_fetch(lfs, &prevdir.m, prevpair);
        if (err) {
            return err;
        }

        if (prevdir.m.count > 0 || prevdir.m.split) {
            return LFS_ERR_NOTEMPTY;
        }

        // mark fs as orphaned
        err = lfs_fs_preporphans(lfs, +1);
        if (err) {
            return err;
        }

        // I know it's crazy but yes, dir can be changed by our parent's
        // commit (if predecessor is child)
        prevdir.type = 0;
        prevdir.id = 0;
        lfs->mlist = &prevdir;
    }

    if (!samepair) {
        lfs_fs_prepmove(lfs, newoldid, oldcwd.pair);
    }

    // move over all attributes
    err = lfs_dir_commit(lfs, &newcwd, LFS_MKATTRS(
            {LFS_MKTAG_IF(prevtag != LFS_ERR_NOENT,
                LFS_TYPE_DELETE, newid, 0), NULL},
            {LFS_MKTAG(LFS_TYPE_CREATE, newid, 0), NULL},
            {LFS_MKTAG(lfs_tag_type3(oldtag),
                newid, lfs_path_namelen(newpath)), newpath},
            {LFS_MKTAG(LFS_FROM_MOVE, newid, lfs_tag_id(oldtag)), &oldcwd},
            {LFS_MKTAG_IF(samepair,
                LFS_TYPE_DELETE, newoldid, 0), NULL}));
    if (err) {
        lfs->mlist = prevdir.next;
        return err;
    }

    // let commit clean up after move (if we're different! otherwise move
    // logic already fixed it for us)
    if (!samepair && lfs_gstate_hasmove(&lfs->gstate)) {
        // prep gstate and delete move id
        lfs_fs_prepmove(lfs, 0x3ff, NULL);
        err = lfs_dir_commit(lfs, &oldcwd, LFS_MKATTRS(
                {LFS_MKTAG(LFS_TYPE_DELETE, lfs_tag_id(oldtag), 0), NULL}));
        if (err) {
            lfs->mlist = prevdir.next;
            return err;
        }
    }

    lfs->mlist = prevdir.next;
    if (lfs_gstate_hasorphans(&lfs->gstate)) {
        LFS_ASSERT(prevtag != LFS_ERR_NOENT
                && lfs_tag_type3(prevtag) == LFS_TYPE_DIR);

        // fix orphan
        err = lfs_fs_preporphans(lfs, -1);
        if (err) {
            return err;
        }

        err = lfs_fs_pred(lfs, prevdir.m.pair, &newcwd);
        if (err) {
            return err;
        }

        err = lfs_dir_drop(lfs, &newcwd, &prevdir.m);
        if (err) {
            return err;
        }
    }

    return 0;
}
#endif

/**
 * @brief 读自定义属性：USERATTR+type tag 切片读
 * @note  root 特判（id 0x3ff → root 对的 id 0）；不存在返回
 *        LFS_ERR_NOATTR；返回值是盘上实际长度（可大于缓冲）。
 */
static lfs_ssize_t lfs_getattr_(lfs_t *lfs, const char *path,
        uint8_t type, void *buffer, lfs_size_t size) {
    lfs_mdir_t cwd;
    lfs_stag_t tag = lfs_dir_find(lfs, &cwd, &path, NULL);
    if (tag < 0) {
        return tag;
    }

    uint16_t id = lfs_tag_id(tag);
    if (id == 0x3ff) {
        // special case for root
        id = 0;
        int err = lfs_dir_fetch(lfs, &cwd, lfs->root);
        if (err) {
            return err;
        }
    }

    tag = lfs_dir_get(lfs, &cwd, LFS_MKTAG(0x7ff, 0x3ff, 0),
            LFS_MKTAG(LFS_TYPE_USERATTR + type,
                id, lfs_min(size, lfs->attr_max)),
            buffer);
    if (tag < 0) {
        if (tag == LFS_ERR_NOENT) {
            return LFS_ERR_NOATTR;
        }

        return tag;
    }

    return lfs_tag_size(tag);
}

#ifndef LFS_READONLY
/** @brief 写/删自定义属性的公共体（size=0x3ff 即删除标记） */
static int lfs_commitattr(lfs_t *lfs, const char *path,
        uint8_t type, const void *buffer, lfs_size_t size) {
    lfs_mdir_t cwd;
    lfs_stag_t tag = lfs_dir_find(lfs, &cwd, &path, NULL);
    if (tag < 0) {
        return tag;
    }

    uint16_t id = lfs_tag_id(tag);
    if (id == 0x3ff) {
        // special case for root
        id = 0;
        int err = lfs_dir_fetch(lfs, &cwd, lfs->root);
        if (err) {
            return err;
        }
    }

    return lfs_dir_commit(lfs, &cwd, LFS_MKATTRS(
            {LFS_MKTAG(LFS_TYPE_USERATTR + type, id, size), buffer}));
}
#endif

#ifndef LFS_READONLY
/** @brief 设置属性：超 attr_max 先拒绝，再走 commitattr */
static int lfs_setattr_(lfs_t *lfs, const char *path,
        uint8_t type, const void *buffer, lfs_size_t size) {
    if (size > lfs->attr_max) {
        return LFS_ERR_NOSPC;
    }

    return lfs_commitattr(lfs, path, type, buffer, size);
}
#endif

#ifndef LFS_READONLY
/** @brief 删除属性：size=0x3ff 的删除 tag；属性本就不存在时无操作 */
static int lfs_removeattr_(lfs_t *lfs, const char *path, uint8_t type) {
    return lfs_commitattr(lfs, path, type, NULL, 0x3ff);
}
#endif


/// Filesystem operations ///

// compile time checks, see lfs.h for why these limits exist
#if LFS_NAME_MAX > 1022
#error "Invalid LFS_NAME_MAX, must be <= 1022"
#endif

#if LFS_FILE_MAX > 2147483647
#error "Invalid LFS_FILE_MAX, must be <= 2147483647"
#endif

#if LFS_ATTR_MAX > 1022
#error "Invalid LFS_ATTR_MAX, must be <= 1022"
#endif

// common filesystem initialization
/**
 * @brief 公共初始化（format/mount/migrate 共用）：校验 config →
 *        分配三个缓存 → 解析上限 → 清零状态
 * @note  RAM 预算 = 2*cache_size（rcache/pcache）+ lookahead_size
 *        + 每打开一个文件再加 cache_size，全部可用 cfg->*_buffer
 *        静态提供（无堆系统友好）；这一堆 LFS_ASSERT 把配置约束
 *        （对齐倍数、CTZ 指针不溢出块、block_cycles 非零等）全部
 *        前置到挂载时刻，而不是写盘时才炸。
 */
static int lfs_init(lfs_t *lfs, const struct lfs_config *cfg) {
    lfs->cfg = cfg;
    lfs->block_count = cfg->block_count;  // May be 0
    int err = 0;

#ifdef LFS_MULTIVERSION
    // this driver only supports minor version < current minor version
    LFS_ASSERT(!lfs->cfg->disk_version || (
            (0xffff & (lfs->cfg->disk_version >> 16))
                    == LFS_DISK_VERSION_MAJOR
                && (0xffff & (lfs->cfg->disk_version >> 0))
                    <= LFS_DISK_VERSION_MINOR));
#endif

    // check that bool is a truthy-preserving type
    //
    // note the most common reason for this failure is a before-c99 compiler,
    // which littlefs currently does not support
    LFS_ASSERT((bool)0x80000000);

    // check that the required io functions are provided
    LFS_ASSERT(lfs->cfg->read != NULL);
#ifndef LFS_READONLY
    LFS_ASSERT(lfs->cfg->prog != NULL);
    LFS_ASSERT(lfs->cfg->erase != NULL);
    LFS_ASSERT(lfs->cfg->sync != NULL);
#endif

    // validate that the lfs-cfg sizes were initiated properly before
    // performing any arithmetic logics with them
    LFS_ASSERT(lfs->cfg->read_size != 0);
    LFS_ASSERT(lfs->cfg->prog_size != 0);
    LFS_ASSERT(lfs->cfg->cache_size != 0);

    // check that block size is a multiple of cache size is a multiple
    // of prog and read sizes
    LFS_ASSERT(lfs->cfg->cache_size % lfs->cfg->read_size == 0);
    LFS_ASSERT(lfs->cfg->cache_size % lfs->cfg->prog_size == 0);
    LFS_ASSERT(lfs->cfg->block_size % lfs->cfg->cache_size == 0);

    // CTZ 指针区上限：4*npw2(0xffffffff/(block_size-8)) <= block_size，
    // 32 位文件尺寸下块 >=104 字节即安全（这里用精确公式而非 128 拍脑袋）
    // check that the block size is large enough to fit all ctz pointers
    LFS_ASSERT(lfs->cfg->block_size >= 128);
    // this is the exact calculation for all ctz pointers, if this fails
    // and the simpler assert above does not, math must be broken
    LFS_ASSERT(4*lfs_npw2(0xffffffff / (lfs->cfg->block_size-2*4))
            <= lfs->cfg->block_size);

    // block_cycles = 0 is no longer supported.
    //
    // block_cycles is the number of erase cycles before littlefs evicts
    // metadata logs as a part of wear leveling. Suggested values are in the
    // range of 100-1000, or set block_cycles to -1 to disable block-level
    // wear-leveling.
    LFS_ASSERT(lfs->cfg->block_cycles != 0);

    // check that compact_thresh makes sense
    //
    // metadata can't be compacted below block_size/2, and metadata can't
    // exceed a block_size
    LFS_ASSERT(lfs->cfg->compact_thresh == 0
            || lfs->cfg->compact_thresh >= lfs->cfg->block_size/2);
    LFS_ASSERT(lfs->cfg->compact_thresh == (lfs_size_t)-1
            || lfs->cfg->compact_thresh <= lfs->cfg->block_size);

    // check that metadata_max is a multiple of read_size and prog_size,
    // and a factor of the block_size
    LFS_ASSERT(!lfs->cfg->metadata_max
            || lfs->cfg->metadata_max % lfs->cfg->read_size == 0);
    LFS_ASSERT(!lfs->cfg->metadata_max
            || lfs->cfg->metadata_max % lfs->cfg->prog_size == 0);
    LFS_ASSERT(!lfs->cfg->metadata_max
            || lfs->cfg->block_size % lfs->cfg->metadata_max == 0);

    // setup read cache
    if (lfs->cfg->read_buffer) {
        lfs->rcache.buffer = lfs->cfg->read_buffer;
    } else {
        lfs->rcache.buffer = lfs_malloc(lfs->cfg->cache_size);
        if (!lfs->rcache.buffer) {
            err = LFS_ERR_NOMEM;
            goto cleanup;
        }
    }

    // setup program cache
    if (lfs->cfg->prog_buffer) {
        lfs->pcache.buffer = lfs->cfg->prog_buffer;
    } else {
        lfs->pcache.buffer = lfs_malloc(lfs->cfg->cache_size);
        if (!lfs->pcache.buffer) {
            err = LFS_ERR_NOMEM;
            goto cleanup;
        }
    }

    // zero to avoid information leaks
    lfs_cache_zero(lfs, &lfs->rcache);
    lfs_cache_zero(lfs, &lfs->pcache);

    // setup lookahead buffer, note mount finishes initializing this after
    // we establish a decent pseudo-random seed
    LFS_ASSERT(lfs->cfg->lookahead_size > 0);
    if (lfs->cfg->lookahead_buffer) {
        lfs->lookahead.buffer = lfs->cfg->lookahead_buffer;
    } else {
        lfs->lookahead.buffer = lfs_malloc(lfs->cfg->lookahead_size);
        if (!lfs->lookahead.buffer) {
            err = LFS_ERR_NOMEM;
            goto cleanup;
        }
    }

    // check that the size limits are sane
    LFS_ASSERT(lfs->cfg->name_max <= LFS_NAME_MAX);
    lfs->name_max = lfs->cfg->name_max;
    if (!lfs->name_max) {
        lfs->name_max = LFS_NAME_MAX;
    }

    LFS_ASSERT(lfs->cfg->file_max <= LFS_FILE_MAX);
    lfs->file_max = lfs->cfg->file_max;
    if (!lfs->file_max) {
        lfs->file_max = LFS_FILE_MAX;
    }

    LFS_ASSERT(lfs->cfg->attr_max <= LFS_ATTR_MAX);
    lfs->attr_max = lfs->cfg->attr_max;
    if (!lfs->attr_max) {
        lfs->attr_max = LFS_ATTR_MAX;
    }

    LFS_ASSERT(lfs->cfg->metadata_max <= lfs->cfg->block_size);

    LFS_ASSERT(lfs->cfg->inline_max == (lfs_size_t)-1
            || lfs->cfg->inline_max <= lfs->cfg->cache_size);
    LFS_ASSERT(lfs->cfg->inline_max == (lfs_size_t)-1
            || lfs->cfg->inline_max <= lfs->attr_max);
    LFS_ASSERT(lfs->cfg->inline_max == (lfs_size_t)-1
            || lfs->cfg->inline_max <= ((lfs->cfg->metadata_max)
                ? lfs->cfg->metadata_max
                : lfs->cfg->block_size)/8);
    lfs->inline_max = lfs->cfg->inline_max;
    if (lfs->inline_max == (lfs_size_t)-1) {
        lfs->inline_max = 0;
    } else if (lfs->inline_max == 0) {
        lfs->inline_max = lfs_min(
                lfs->cfg->cache_size,
                lfs_min(
                    lfs->attr_max,
                    ((lfs->cfg->metadata_max)
                        ? lfs->cfg->metadata_max
                        : lfs->cfg->block_size)/8));
    }

    // setup default state
    lfs->root[0] = LFS_BLOCK_NULL;
    lfs->root[1] = LFS_BLOCK_NULL;
    lfs->mlist = NULL;
    lfs->seed = 0;
    lfs->gdisk = (lfs_gstate_t){0};
    lfs->gstate = (lfs_gstate_t){0};
    lfs->gdelta = (lfs_gstate_t){0};
#ifdef LFS_MIGRATE
    lfs->lfs1 = NULL;
#endif

    return 0;

cleanup:
    lfs_deinit(lfs);
    return err;
}

/** @brief 释放三个缓存（用户静态提供的不释放）；不碰盘 */
static int lfs_deinit(lfs_t *lfs) {
    // free allocated memory
    if (!lfs->cfg->read_buffer) {
        lfs_free(lfs->rcache.buffer);
    }

    if (!lfs->cfg->prog_buffer) {
        lfs_free(lfs->pcache.buffer);
    }

    if (!lfs->cfg->lookahead_buffer) {
        lfs_free(lfs->lookahead.buffer);
    }

    return 0;
}



#ifndef LFS_READONLY
/**
 * @brief 格式化：初始化 → 根对分配 → 写超块（magic+配置） → 强制 compact
 * @note  第二次空 commit 强制 compact 是防"同块旧版本数据"残留
 *        （旧 2.0 盘的残迹可能被误挂载）；结尾 fetch 自检一次。
 *        超块条目固定占 id 0，magic "littlefs" 恒在偏移 8 处。
 */
static int lfs_format_(lfs_t *lfs, const struct lfs_config *cfg) {
    int err = 0;
    {
        err = lfs_init(lfs, cfg);
        if (err) {
            return err;
        }

        LFS_ASSERT(cfg->block_count != 0);

        // create free lookahead
        memset(lfs->lookahead.buffer, 0, lfs->cfg->lookahead_size);
        lfs->lookahead.start = 0;
        lfs->lookahead.size = lfs_min(8*lfs->cfg->lookahead_size,
                lfs->block_count);
        lfs->lookahead.next = 0;
        lfs_alloc_ckpoint(lfs);

        // create root dir
        lfs_mdir_t root;
        err = lfs_dir_alloc(lfs, &root);
        if (err) {
            goto cleanup;
        }

        // write one superblock
        lfs_superblock_t superblock = {
            .version     = lfs_fs_disk_version(lfs),
            .block_size  = lfs->cfg->block_size,
            .block_count = lfs->block_count,
            .name_max    = lfs->name_max,
            .file_max    = lfs->file_max,
            .attr_max    = lfs->attr_max,
        };

        lfs_superblock_tole32(&superblock);
        err = lfs_dir_commit(lfs, &root, LFS_MKATTRS(
                {LFS_MKTAG(LFS_TYPE_CREATE, 0, 0), NULL},
                {LFS_MKTAG(LFS_TYPE_SUPERBLOCK, 0, 8), "littlefs"},
                {LFS_MKTAG(LFS_TYPE_INLINESTRUCT, 0, sizeof(superblock)),
                    &superblock}));
        if (err) {
            goto cleanup;
        }

        // force compaction to prevent accidentally mounting any
        // older version of littlefs that may live on disk
        root.erased = false;
        err = lfs_dir_commit(lfs, &root, NULL, 0);
        if (err) {
            goto cleanup;
        }

        // sanity check that fetch works
        err = lfs_dir_fetch(lfs, &root, (const lfs_block_t[2]){0, 1});
        if (err) {
            goto cleanup;
        }
    }

cleanup:
    lfs_deinit(lfs);
    return err;

}
#endif

struct lfs_tortoise_t {
    lfs_block_t pair[2];
    lfs_size_t i;
    lfs_size_t period;
};

/**
 * @brief Brent 龟兔算法检测 tail 链成环（损坏盘防御，O(1) RAM）
 * @note  兔子每步走一格，乌龟按 1,2,4,8... 周期驻留；同格相遇即环。
 *        挂载/遍历/找前驱都要沿 tail 链走，环会让它们死循环。
 */
static int lfs_tortoise_detectcycles(
    const lfs_mdir_t *dir, struct lfs_tortoise_t *tortoise) {
    // detect cycles with Brent's algorithm
    if (lfs_pair_issync(dir->tail, tortoise->pair)) {
        LFS_WARN("Cycle detected in tail list");
        return LFS_ERR_CORRUPT;
    }
    if (tortoise->i == tortoise->period) {
        tortoise->pair[0] = dir->tail[0];
        tortoise->pair[1] = dir->tail[1];
        tortoise->i = 0;
        tortoise->period *= 2;
    }
    tortoise->i += 1;

    return LFS_ERR_OK;
}

/**
 * @brief 挂载：沿 tail 链扫全盘 → 找 superblock → 校验版本/配置 →
 *        聚合 gstate → 随机化分配起点
 * @note  超块链从块 {0,1} 开始，最后一个含 superblock 的对就是
 *        root 目录（超块链尾部=root，SPEC.md 的关键设计）；
 *        盘是旧次版本时置 needssuperblock 位，首次写前重写超块；
 *        cfg->block_count=0 表示"以盘上为准"。
 */
static int lfs_mount_(lfs_t *lfs, const struct lfs_config *cfg) {
    int err = lfs_init(lfs, cfg);
    if (err) {
        return err;
    }

    // scan directory blocks for superblock and any global updates
    lfs_mdir_t dir = {.tail = {0, 1}};
    struct lfs_tortoise_t tortoise = {
        .pair = {LFS_BLOCK_NULL, LFS_BLOCK_NULL},
        .i = 1,
        .period = 1,
    };
    while (!lfs_pair_isnull(dir.tail)) {
        err = lfs_tortoise_detectcycles(&dir, &tortoise);
        if (err < 0) {
            goto cleanup;
        }

        // fetch next block in tail list
        lfs_stag_t tag = lfs_dir_fetchmatch(lfs, &dir, dir.tail,
                LFS_MKTAG(0x7ff, 0x3ff, 0),
                LFS_MKTAG(LFS_TYPE_SUPERBLOCK, 0, 8),
                NULL,
                lfs_dir_find_match, &(struct lfs_dir_find_match){
                    lfs, "littlefs", 8});
        if (tag < 0) {
            err = tag;
            goto cleanup;
        }

        // has superblock?
        if (tag && !lfs_tag_isdelete(tag)) {
            // update root
            lfs->root[0] = dir.pair[0];
            lfs->root[1] = dir.pair[1];

            // grab superblock
            lfs_superblock_t superblock;
            tag = lfs_dir_get(lfs, &dir, LFS_MKTAG(0x7ff, 0x3ff, 0),
                    LFS_MKTAG(LFS_TYPE_INLINESTRUCT, 0, sizeof(superblock)),
                    &superblock);
            if (tag < 0) {
                err = tag;
                goto cleanup;
            }
            lfs_superblock_fromle32(&superblock);

            // check version
            uint16_t major_version = (0xffff & (superblock.version >> 16));
            uint16_t minor_version = (0xffff & (superblock.version >>  0));
            if (major_version != lfs_fs_disk_version_major(lfs)
                    || minor_version > lfs_fs_disk_version_minor(lfs)) {
                LFS_ERROR("Invalid version "
                        "v%"PRIu16".%"PRIu16" != v%"PRIu16".%"PRIu16,
                        major_version,
                        minor_version,
                        lfs_fs_disk_version_major(lfs),
                        lfs_fs_disk_version_minor(lfs));
                err = LFS_ERR_INVAL;
                goto cleanup;
            }

            // found older minor version? set an in-device only bit in the
            // gstate so we know we need to rewrite the superblock before
            // the first write
            bool needssuperblock = false;
            if (minor_version < lfs_fs_disk_version_minor(lfs)) {
                LFS_DEBUG("Found older minor version "
                        "v%"PRIu16".%"PRIu16" < v%"PRIu16".%"PRIu16,
                        major_version,
                        minor_version,
                        lfs_fs_disk_version_major(lfs),
                        lfs_fs_disk_version_minor(lfs));
                needssuperblock = true;
            }
            // note this bit is reserved on disk, so fetching more gstate
            // will not interfere here
            lfs_fs_prepsuperblock(lfs, needssuperblock);

            // check superblock configuration
            if (superblock.name_max) {
                if (superblock.name_max > lfs->name_max) {
                    LFS_ERROR("Unsupported name_max (%"PRIu32" > %"PRIu32")",
                            superblock.name_max, lfs->name_max);
                    err = LFS_ERR_INVAL;
                    goto cleanup;
                }

                lfs->name_max = superblock.name_max;
            }

            if (superblock.file_max) {
                if (superblock.file_max > lfs->file_max) {
                    LFS_ERROR("Unsupported file_max (%"PRIu32" > %"PRIu32")",
                            superblock.file_max, lfs->file_max);
                    err = LFS_ERR_INVAL;
                    goto cleanup;
                }

                lfs->file_max = superblock.file_max;
            }

            if (superblock.attr_max) {
                if (superblock.attr_max > lfs->attr_max) {
                    LFS_ERROR("Unsupported attr_max (%"PRIu32" > %"PRIu32")",
                            superblock.attr_max, lfs->attr_max);
                    err = LFS_ERR_INVAL;
                    goto cleanup;
                }

                lfs->attr_max = superblock.attr_max;

                // we also need to update inline_max in case attr_max changed
                lfs->inline_max = lfs_min(lfs->inline_max, lfs->attr_max);
            }

            // this is where we get the block_count from disk if block_count=0
            if (lfs->cfg->block_count
                    && superblock.block_count != lfs->cfg->block_count) {
                LFS_ERROR("Invalid block count (%"PRIu32" != %"PRIu32")",
                        superblock.block_count, lfs->cfg->block_count);
                err = LFS_ERR_INVAL;
                goto cleanup;
            }

            lfs->block_count = superblock.block_count;

            if (superblock.block_size != lfs->cfg->block_size) {
                LFS_ERROR("Invalid block size (%"PRIu32" != %"PRIu32")",
                        superblock.block_size, lfs->cfg->block_size);
                err = LFS_ERR_INVAL;
                goto cleanup;
            }
        }

        // has gstate?
        err = lfs_dir_getgstate(lfs, &dir, &lfs->gstate);
        if (err) {
            goto cleanup;
        }
    }

    // update littlefs with gstate
    if (!lfs_gstate_iszero(&lfs->gstate)) {
        LFS_DEBUG("Found pending gstate 0x%08"PRIx32"%08"PRIx32"%08"PRIx32,
                lfs->gstate.tag,
                lfs->gstate.pair[0],
                lfs->gstate.pair[1]);
    }
    // gstate 可能聚合自"未写区域全 0xff"：valid 位回绕 +1 修正，
    // 让 RAM 里的 gstate 恒为规范形态
    lfs->gstate.tag += !lfs_tag_isvalid(lfs->gstate.tag);
    lfs->gdisk = lfs->gstate;

    // setup free lookahead, to distribute allocations uniformly across
    // boots, we start the allocator at a random location
    // 伪随机起点：seed 来自各对 CRC 的再 CRC（跨掉电稳定），
    // 使分配窗口在盘上均匀分布——动态磨损均衡的另一半
    lfs->lookahead.start = lfs->seed % lfs->block_count;
    lfs_alloc_drop(lfs);

    return 0;

cleanup:
    lfs_unmount_(lfs);
    return err;
}

/**
 * @brief 卸载实现：只释放 RAM（所有写已在各 sync/commit 时提交，
 *        卸载本身没有"必须落盘"的尾巴——这也是掉电安全的一种体现）
 */
static int lfs_unmount_(lfs_t *lfs) {
    return lfs_deinit(lfs);
}


/// Filesystem filesystem operations ///
/**
 * @brief 盘上信息：版本/几何/上限
 * @note  needssuperblock（挂载到旧次版本盘）时真实版本要从盘上
 *        superblock 读，其余字段直接用 RAM 缓存值。
 */
static int lfs_fs_stat_(lfs_t *lfs, struct lfs_fsinfo *fsinfo) {
    // if the superblock is up-to-date, we must be on the most recent
    // minor version of littlefs
    if (!lfs_gstate_needssuperblock(&lfs->gstate)) {
        fsinfo->disk_version = lfs_fs_disk_version(lfs);

    // otherwise we need to read the minor version on disk
    } else {
        // fetch the superblock
        lfs_mdir_t dir;
        int err = lfs_dir_fetch(lfs, &dir, lfs->root);
        if (err) {
            return err;
        }

        lfs_superblock_t superblock;
        lfs_stag_t tag = lfs_dir_get(lfs, &dir, LFS_MKTAG(0x7ff, 0x3ff, 0),
                LFS_MKTAG(LFS_TYPE_INLINESTRUCT, 0, sizeof(superblock)),
                &superblock);
        if (tag < 0) {
            return tag;
        }
        lfs_superblock_fromle32(&superblock);

        // read the on-disk version
        fsinfo->disk_version = superblock.version;
    }

    // filesystem geometry
    fsinfo->block_size = lfs->cfg->block_size;
    fsinfo->block_count = lfs->block_count;

    // other on-disk configuration, we cache all of these for internal use
    fsinfo->name_max = lfs->name_max;
    fsinfo->file_max = lfs->file_max;
    fsinfo->attr_max = lfs->attr_max;

    return 0;
}

/**
 * @brief 全盘在用块遍历（分配器扫描/容量统计/收缩检查的地基）
 * @param  includeorphans 是否把 DIRSTRUCT 指向的对也计入
 *        （deorphan 前的扫描要计入，否则孤儿会被当成空闲块分配出去）
 * @note  覆盖三类引用：tail 链上的元数据对、各文件 CTZ 数据块、
 *        打开中脏文件的 RAM 分支（未提交也要算，防止分配到正在
 *        使用的块——这是"打开文件也是文件系统状态"的体现）。
 */
int lfs_fs_traverse_(lfs_t *lfs,
        int (*cb)(void *data, lfs_block_t block), void *data,
        bool includeorphans) {
    // iterate over metadata pairs
    lfs_mdir_t dir = {.tail = {0, 1}};

#ifdef LFS_MIGRATE
    // also consider v1 blocks during migration
    if (lfs->lfs1) {
        int err = lfs1_traverse(lfs, cb, data);
        if (err) {
            return err;
        }

        dir.tail[0] = lfs->root[0];
        dir.tail[1] = lfs->root[1];
    }
#endif

    struct lfs_tortoise_t tortoise = {
        .pair = {LFS_BLOCK_NULL, LFS_BLOCK_NULL},
        .i = 1,
        .period = 1,
    };
    int err = LFS_ERR_OK;
    while (!lfs_pair_isnull(dir.tail)) {
        err = lfs_tortoise_detectcycles(&dir, &tortoise);
        if (err < 0) {
            return LFS_ERR_CORRUPT;
        }

        for (int i = 0; i < 2; i++) {
            int err = cb(data, dir.tail[i]);
            if (err) {
                return err;
            }
        }

        // iterate through ids in directory
        int err = lfs_dir_fetch(lfs, &dir, dir.tail);
        if (err) {
            return err;
        }

        for (uint16_t id = 0; id < dir.count; id++) {
            struct lfs_ctz ctz;
            lfs_stag_t tag = lfs_dir_get(lfs, &dir, LFS_MKTAG(0x700, 0x3ff, 0),
                    LFS_MKTAG(LFS_TYPE_STRUCT, id, sizeof(ctz)), &ctz);
            if (tag < 0) {
                if (tag == LFS_ERR_NOENT) {
                    continue;
                }
                return tag;
            }
            lfs_ctz_fromle32(&ctz);

            if (lfs_tag_type3(tag) == LFS_TYPE_CTZSTRUCT) {
                err = lfs_ctz_traverse(lfs, NULL, &lfs->rcache,
                        ctz.head, ctz.size, cb, data);
                if (err) {
                    return err;
                }
            } else if (includeorphans &&
                    lfs_tag_type3(tag) == LFS_TYPE_DIRSTRUCT) {
                for (int i = 0; i < 2; i++) {
                    err = cb(data, (&ctz.head)[i]);
                    if (err) {
                        return err;
                    }
                }
            }
        }
    }

#ifndef LFS_READONLY
    // iterate over any open files
    for (lfs_file_t *f = (lfs_file_t*)lfs->mlist; f; f = f->next) {
        if (f->type != LFS_TYPE_REG) {
            continue;
        }

        if (((f->flags & LFS_F_DIRTY) || (f->flags & LFS_F_DUSTY))
                && !(f->flags & LFS_F_INLINE)) {
            int err = lfs_ctz_traverse(lfs, &f->cache, &lfs->rcache,
                    f->ctz.head, f->ctz.size, cb, data);
            if (err) {
                return err;
            }
        }

        if ((f->flags & LFS_F_WRITING) && !(f->flags & LFS_F_INLINE)) {
            int err = lfs_ctz_traverse(lfs, &f->cache, &lfs->rcache,
                    f->block, f->pos, cb, data);
            if (err) {
                return err;
            }
        }
    }
#endif

    return 0;
}

#ifndef LFS_READONLY
/**
 * @brief 在目录链上找 pair 的前驱对（无前驱=NOENT）
 * @note  drop 空对/修孤儿时都要"改前驱的 tail"，先得找到前驱；
 *        同样用龟兔防环。
 */
static int lfs_fs_pred(lfs_t *lfs,
        const lfs_block_t pair[2], lfs_mdir_t *pdir) {
    // iterate over all directory directory entries
    pdir->tail[0] = 0;
    pdir->tail[1] = 1;
    struct lfs_tortoise_t tortoise = {
        .pair = {LFS_BLOCK_NULL, LFS_BLOCK_NULL},
        .i = 1,
        .period = 1,
    };
    int err = LFS_ERR_OK;
    while (!lfs_pair_isnull(pdir->tail)) {
        err = lfs_tortoise_detectcycles(pdir, &tortoise);
        if (err < 0) {
            return LFS_ERR_CORRUPT;
        }

        if (lfs_pair_cmp(pdir->tail, pair) == 0) {
            return 0;
        }

        int err = lfs_dir_fetch(lfs, pdir, pdir->tail);
        if (err) {
            return err;
        }
    }

    return LFS_ERR_NOENT;
}
#endif

#ifndef LFS_READONLY
struct lfs_fs_parent_match {
    lfs_t *lfs;
    const lfs_block_t pair[2];
};
#endif

#ifndef LFS_READONLY
/** @brief fetchmatch 回调：比较磁盘上的 DIRSTRUCT 指针与目标对 */
static int lfs_fs_parent_match(void *data,
        lfs_tag_t tag, const void *buffer) {
    struct lfs_fs_parent_match *find = data;
    lfs_t *lfs = find->lfs;
    const struct lfs_diskoff *disk = buffer;
    (void)tag;

    lfs_block_t child[2];
    int err = lfs_bd_read(lfs,
            &lfs->pcache, &lfs->rcache, lfs->cfg->block_size,
            disk->block, disk->off, &child, sizeof(child));
    if (err) {
        return err;
    }

    lfs_pair_fromle32(child);
    return (lfs_pair_cmp(child, find->pair) == 0) ? LFS_CMP_EQ : LFS_CMP_LT;
}
#endif

#ifndef LFS_READONLY
/**
 * @brief 全链反查"谁引用了这个子目录"（O(n) 扫描）
 * @note  只在孤儿修复/搬迁连锁时用；找到的 tag 即父对里
 *        DIRSTRUCT 条目的 tag（含 id）。
 */
static lfs_stag_t lfs_fs_parent(lfs_t *lfs, const lfs_block_t pair[2],
        lfs_mdir_t *parent) {
    // use fetchmatch with callback to find pairs
    parent->tail[0] = 0;
    parent->tail[1] = 1;
    struct lfs_tortoise_t tortoise = {
        .pair = {LFS_BLOCK_NULL, LFS_BLOCK_NULL},
        .i = 1,
        .period = 1,
    };
    int err = LFS_ERR_OK;
    while (!lfs_pair_isnull(parent->tail)) {
        err = lfs_tortoise_detectcycles(parent, &tortoise);
        if (err < 0) {
            return err;
        }

        lfs_stag_t tag = lfs_dir_fetchmatch(lfs, parent, parent->tail,
                LFS_MKTAG(0x7ff, 0, 0x3ff),
                LFS_MKTAG(LFS_TYPE_DIRSTRUCT, 0, 8),
                NULL,
                lfs_fs_parent_match, &(struct lfs_fs_parent_match){
                    lfs, {pair[0], pair[1]}});
        if (tag && tag != LFS_ERR_NOENT) {
            return tag;
        }
    }

    return LFS_ERR_NOENT;
}
#endif

/** @brief 置/清 gstate 的 needssuperblock 位（tag 的 size bit9） */
static void lfs_fs_prepsuperblock(lfs_t *lfs, bool needssuperblock) {
    lfs->gstate.tag = (lfs->gstate.tag & ~LFS_MKTAG(0, 0, 0x200))
            | (uint32_t)needssuperblock << 9;
}

#ifndef LFS_READONLY
/**
 * @brief 孤儿计数 ±1，并把"有孤儿"同步进 tag 最高位
 * @note  size 字段低 9 位是计数，bit31 是标志（提交到盘上就是
 *        MOVESTATE 的 sync 位语义）；两处必须一致，否则挂载聚合
 *        后的状态会失真。这条位运算就是"孤儿"在 gstate 里的全部分身。
 */
static int lfs_fs_preporphans(lfs_t *lfs, int8_t orphans) {
    LFS_ASSERT(lfs_tag_size(lfs->gstate.tag) > 0x000 || orphans >= 0);
    LFS_ASSERT(lfs_tag_size(lfs->gstate.tag) < 0x1ff || orphans <= 0);
    lfs->gstate.tag += orphans;
    lfs->gstate.tag = ((lfs->gstate.tag & ~LFS_MKTAG(0x800, 0, 0)) |
            ((uint32_t)lfs_gstate_hasorphans(&lfs->gstate) << 31));

    return 0;
}
#endif

#ifndef LFS_READONLY
/**
 * @brief 设置/清除 gstate 的 move 字段（id=0x3ff 即清除）
 * @note  move 字段记录"源对+源 id"，跨对 rename 的第一段提交
 *        前设置、第二段提交时清除。
 */
static void lfs_fs_prepmove(lfs_t *lfs,
        uint16_t id, const lfs_block_t pair[2]) {
    lfs->gstate.tag = ((lfs->gstate.tag & ~LFS_MKTAG(0x7ff, 0x3ff, 0)) |
            ((id != 0x3ff) ? LFS_MKTAG(LFS_TYPE_DELETE, id, 0) : 0));
    lfs->gstate.pair[0] = (id != 0x3ff) ? pair[0] : 0;
    lfs->gstate.pair[1] = (id != 0x3ff) ? pair[1] : 0;
}
#endif

#ifndef LFS_READONLY
/** @brief 首写前重写超块（挂载到旧次版本盘后的就地升级） */
static int lfs_fs_desuperblock(lfs_t *lfs) {
    if (!lfs_gstate_needssuperblock(&lfs->gstate)) {
        return 0;
    }

    LFS_DEBUG("Rewriting superblock {0x%"PRIx32", 0x%"PRIx32"}",
            lfs->root[0],
            lfs->root[1]);

    lfs_mdir_t root;
    int err = lfs_dir_fetch(lfs, &root, lfs->root);
    if (err) {
        return err;
    }

    // write a new superblock
    lfs_superblock_t superblock = {
        .version     = lfs_fs_disk_version(lfs),
        .block_size  = lfs->cfg->block_size,
        .block_count = lfs->block_count,
        .name_max    = lfs->name_max,
        .file_max    = lfs->file_max,
        .attr_max    = lfs->attr_max,
    };

    lfs_superblock_tole32(&superblock);
    err = lfs_dir_commit(lfs, &root, LFS_MKATTRS(
            {LFS_MKTAG(LFS_TYPE_INLINESTRUCT, 0, sizeof(superblock)),
                &superblock}));
    if (err) {
        return err;
    }

    lfs_fs_prepsuperblock(lfs, false);
    return 0;
}
#endif

#ifndef LFS_READONLY
/**
 * @brief 完成掉电遗留的 move：删源条目并清 gstate
 * @note  目标条目已在（move 前半已提交成功），所以"删除源"就是
 *        move 的语义收尾；这是掉电后文件同时出现在源和目标的
 *        唯一消除点（mkconsistent/首次写都会走到）。
 */
static int lfs_fs_demove(lfs_t *lfs) {
    if (!lfs_gstate_hasmove(&lfs->gdisk)) {
        return 0;
    }

    // Fix bad moves
    LFS_DEBUG("Fixing move {0x%"PRIx32", 0x%"PRIx32"} 0x%"PRIx16,
            lfs->gdisk.pair[0],
            lfs->gdisk.pair[1],
            lfs_tag_id(lfs->gdisk.tag));

    // no other gstate is supported at this time, so if we found something else
    // something most likely went wrong in gstate calculation
    LFS_ASSERT(lfs_tag_type3(lfs->gdisk.tag) == LFS_TYPE_DELETE);

    // fetch and delete the moved entry
    lfs_mdir_t movedir;
    int err = lfs_dir_fetch(lfs, &movedir, lfs->gdisk.pair);
    if (err) {
        return err;
    }

    // prep gstate and delete move id
    uint16_t moveid = lfs_tag_id(lfs->gdisk.tag);
    lfs_fs_prepmove(lfs, 0x3ff, NULL);
    err = lfs_dir_commit(lfs, &movedir, LFS_MKATTRS(
            {LFS_MKTAG(LFS_TYPE_DELETE, moveid, 0), NULL}));
    if (err) {
        return err;
    }

    return 0;
}
#endif

#ifndef LFS_READONLY
/**
 * @brief 孤儿回收核心：两遍扫描，pass0 修 half-orphan，pass1 修全孤儿
 * @note  pass0：链上有父但父指针与链不同步（搬迁遗留）→ 改写父指针；
 *        pass1：链上有但无父（remove/rename 遗留）→ 从链上摘除，
 *        块自然回到空闲池。修复过程可能制造新孤儿 => moreorphans
 *        重来。powerloss=false 时只修 half-orphan：主动操作留下的
 *        孤儿是"有意为之的中间态"，马上会被后续步骤收尾，不能误摘。
 */
static int lfs_fs_deorphan(lfs_t *lfs, bool powerloss) {
    if (!lfs_gstate_hasorphans(&lfs->gstate)) {
        return 0;
    }

    // Check for orphans in two separate passes:
    // - 1 for half-orphans (relocations)
    // - 2 for full-orphans (removes/renames)
    //
    // Two separate passes are needed as half-orphans can contain outdated
    // references to full-orphans, effectively hiding them from the deorphan
    // search.
    //
    int pass = 0;
    while (pass < 2) {
        // Fix any orphans
        lfs_mdir_t pdir = {.split = true, .tail = {0, 1}};
        lfs_mdir_t dir;
        bool moreorphans = false;

        // iterate over all directory directory entries
        while (!lfs_pair_isnull(pdir.tail)) {
            int err = lfs_dir_fetch(lfs, &dir, pdir.tail);
            if (err) {
                return err;
            }

            // check head blocks for orphans
            if (!pdir.split) {
                // check if we have a parent
                lfs_mdir_t parent;
                lfs_stag_t tag = lfs_fs_parent(lfs, pdir.tail, &parent);
                if (tag < 0 && tag != LFS_ERR_NOENT) {
                    return tag;
                }

                if (pass == 0 && tag != LFS_ERR_NOENT) {
                    lfs_block_t pair[2];
                    lfs_stag_t state = lfs_dir_get(lfs, &parent,
                            LFS_MKTAG(0x7ff, 0x3ff, 0), tag, pair);
                    if (state < 0) {
                        return state;
                    }
                    lfs_pair_fromle32(pair);

                    if (!lfs_pair_issync(pair, pdir.tail)) {
                        // we have desynced
                        LFS_DEBUG("Fixing half-orphan "
                                "{0x%"PRIx32", 0x%"PRIx32"} "
                                "-> {0x%"PRIx32", 0x%"PRIx32"}",
                                pdir.tail[0], pdir.tail[1], pair[0], pair[1]);

                        // fix pending move in this pair? this looks like an
                        // optimization but is in fact _required_ since
                        // relocating may outdate the move.
                        uint16_t moveid = 0x3ff;
                        if (lfs_gstate_hasmovehere(&lfs->gstate, pdir.pair)) {
                            moveid = lfs_tag_id(lfs->gstate.tag);
                            LFS_DEBUG("Fixing move while fixing orphans "
                                    "{0x%"PRIx32", 0x%"PRIx32"} 0x%"PRIx16"\n",
                                    pdir.pair[0], pdir.pair[1], moveid);
                            lfs_fs_prepmove(lfs, 0x3ff, NULL);
                        }

                        lfs_pair_tole32(pair);
                        state = lfs_dir_orphaningcommit(lfs, &pdir, LFS_MKATTRS(
                                {LFS_MKTAG_IF(moveid != 0x3ff,
                                    LFS_TYPE_DELETE, moveid, 0), NULL},
                                {LFS_MKTAG(LFS_TYPE_SOFTTAIL, 0x3ff, 8),
                                    pair}));
                        lfs_pair_fromle32(pair);
                        if (state < 0) {
                            return state;
                        }

                        // did our commit create more orphans?
                        if (state == LFS_OK_ORPHANED) {
                            moreorphans = true;
                        }

                        // refetch tail
                        continue;
                    }
                }

                // note we only check for full orphans if we may have had a
                // power-loss, otherwise orphans are created intentionally
                // during operations such as lfs_mkdir
                // 全孤儿只在"怀疑掉电"时处理：mkdir 等主动操作留下的
                // 孤儿中间态，随后就会被同一调用的下一步收尾
                if (pass == 1 && tag == LFS_ERR_NOENT && powerloss) {
                    // we are an orphan
                    LFS_DEBUG("Fixing orphan {0x%"PRIx32", 0x%"PRIx32"}",
                            pdir.tail[0], pdir.tail[1]);

                    // steal state
                    err = lfs_dir_getgstate(lfs, &dir, &lfs->gdelta);
                    if (err) {
                        return err;
                    }

                    // steal tail
                    lfs_pair_tole32(dir.tail);
                    int state = lfs_dir_orphaningcommit(lfs, &pdir, LFS_MKATTRS(
                            {LFS_MKTAG(LFS_TYPE_TAIL + dir.split, 0x3ff, 8),
                                dir.tail}));
                    lfs_pair_fromle32(dir.tail);
                    if (state < 0) {
                        return state;
                    }

                    // did our commit create more orphans?
                    if (state == LFS_OK_ORPHANED) {
                        moreorphans = true;
                    }

                    // refetch tail
                    continue;
                }
            }

            pdir = dir;
        }

        pass = moreorphans ? 0 : pass+1;
    }

    // mark orphans as fixed
    return lfs_fs_preporphans(lfs, -lfs_gstate_getorphans(&lfs->gstate));
}
#endif

#ifndef LFS_READONLY
/**
 * @brief 三连清：重写超块 → 完成 move → 回收孤儿
 * @note  所有写型 API（open/mkdir/remove/rename…）入口都会调用，
 *        保证"第一次写之前，上次掉电遗留的全部中间态已清干净"。
 *        这就是 littlefs 不需要 fsck 的实现方式：修复被摊进
 *        每次写操作的第一步。
 */
static int lfs_fs_forceconsistency(lfs_t *lfs) {
    int err = lfs_fs_desuperblock(lfs);
    if (err) {
        return err;
    }

    err = lfs_fs_demove(lfs);
    if (err) {
        return err;
    }

    err = lfs_fs_deorphan(lfs, true);
    if (err) {
        return err;
    }

    return 0;
}
#endif

#ifndef LFS_READONLY
/**
 * @brief API 版 forceconsistency：额外把 RAM 中未落盘的 gstate 差量
 *        用一次空 commit 写到 root（对外承诺"返回后无遗留状态"）
 */
static int lfs_fs_mkconsistent_(lfs_t *lfs) {
    // lfs_fs_forceconsistency does most of the work here
    int err = lfs_fs_forceconsistency(lfs);
    if (err) {
        return err;
    }

    // do we have any pending gstate?
    lfs_gstate_t delta = {0};
    lfs_gstate_xor(&delta, &lfs->gdisk);
    lfs_gstate_xor(&delta, &lfs->gstate);
    if (!lfs_gstate_iszero(&delta)) {
        // lfs_dir_commit will implicitly write out any pending gstate
        lfs_mdir_t root;
        err = lfs_dir_fetch(lfs, &root, lfs->root);
        if (err) {
            return err;
        }

        err = lfs_dir_commit(lfs, &root, NULL, 0);
        if (err) {
            return err;
        }
    }

    return 0;
}
#endif

/** @brief traverse 回调：块计数累加器 */
static int lfs_fs_size_count(void *p, lfs_block_t block) {
    (void)block;
    lfs_size_t *size = p;
    *size += 1;
    return 0;
}

/**
 * @brief 统计在用块数
 * @note  共享 COW 块会被重复计数，所以文档称 best effort——
 *        结果是"分配视角的占用量"，只会偏大不会偏小。
 */
static lfs_ssize_t lfs_fs_size_(lfs_t *lfs) {
    lfs_size_t size = 0;
    int err = lfs_fs_traverse_(lfs, lfs_fs_size_count, &size, false);
    if (err) {
        return err;
    }

    return size;
}

// explicit garbage collection
#ifndef LFS_READONLY
/**
 * @brief 显式保洁：forceconsistency → 压实超阈值元数据 → 预热 lookahead
 * @note  压实手法：把 mdir 置 erased=false 后提交空 commit，
 *        走 relocatingcommit 的 compact 分支；让昂贵的整理工作
 *        可以被调度到空闲时段提前做（而非在关键写路径上做）。
 */
static int lfs_fs_gc_(lfs_t *lfs) {
    // force consistency, even if we're not necessarily going to write,
    // because this function is supposed to take care of janitorial work
    // isn't it?
    int err = lfs_fs_forceconsistency(lfs);
    if (err) {
        return err;
    }

    // try to compact metadata pairs, note we can't really accomplish
    // anything if compact_thresh doesn't at least leave a prog_size
    // available
    if (lfs->cfg->compact_thresh
            < lfs->cfg->block_size - lfs->cfg->prog_size) {
        // iterate over all mdirs
        lfs_mdir_t mdir = {.tail = {0, 1}};
        while (!lfs_pair_isnull(mdir.tail)) {
            err = lfs_dir_fetch(lfs, &mdir, mdir.tail);
            if (err) {
                return err;
            }

            // not erased? exceeds our compaction threshold?
            // 默认阈值 ≈88%：dir.off 超过 block_size*7/8 才压，
            // 与 superblock 扩张的 88% 判据呼应
            if (!mdir.erased || ((lfs->cfg->compact_thresh == 0)
                    ? mdir.off > lfs->cfg->block_size - lfs->cfg->block_size/8
                    : mdir.off > lfs->cfg->compact_thresh)) {
                // the easiest way to trigger a compaction is to mark
                // the mdir as unerased and add an empty commit
                mdir.erased = false;
                err = lfs_dir_commit(lfs, &mdir, NULL, 0);
                if (err) {
                    return err;
                }
            }
        }
    }

    // try to populate the lookahead buffer, unless it's already full
    if (lfs->lookahead.size < lfs_min(
            8 * lfs->cfg->lookahead_size,
            lfs->block_count)) {
        err = lfs_alloc_scan(lfs);
        if (err) {
            return err;
        }
    }

    return 0;
}
#endif

#ifndef LFS_READONLY
#ifdef LFS_SHRINKNONRELOCATING
/** @brief 收缩检查回调：发现块号越界即在用 => 报 NOTEMPTY 中止收缩 */
static int lfs_shrink_checkblock(void *data, lfs_block_t block) {
    lfs_size_t threshold = *((lfs_size_t*)data);
    if (block >= threshold) {
        return LFS_ERR_NOTEMPTY;
    }
    return 0;
}
#endif

/**
 * @brief 扩容：改 RAM block_count 并把新值写进超块
 * @note  littlefs 不回收扩张（超块链不能缩短），收缩仅在
 *        LFS_SHRINKNONRELOCATING 且越界块全空闲时允许。
 */
static int lfs_fs_grow_(lfs_t *lfs, lfs_size_t block_count) {
    int err;

    if (block_count == lfs->block_count) {
        return 0;
    }

    
#ifndef LFS_SHRINKNONRELOCATING
    // shrinking is not supported
    LFS_ASSERT(block_count >= lfs->block_count);
#endif
#ifdef LFS_SHRINKNONRELOCATING
    if (block_count < lfs->block_count) {
        err = lfs_fs_traverse_(lfs, lfs_shrink_checkblock, &block_count, true);
        if (err) {
            return err;
        }
    }
#endif

    lfs->block_count = block_count;

    // fetch the root
    lfs_mdir_t root;
    err = lfs_dir_fetch(lfs, &root, lfs->root);
    if (err) {
        return err;
    }

    // update the superblock
    lfs_superblock_t superblock;
    lfs_stag_t tag = lfs_dir_get(lfs, &root, LFS_MKTAG(0x7ff, 0x3ff, 0),
            LFS_MKTAG(LFS_TYPE_INLINESTRUCT, 0, sizeof(superblock)),
            &superblock);
    if (tag < 0) {
        return tag;
    }
    lfs_superblock_fromle32(&superblock);

    superblock.block_count = lfs->block_count;

    lfs_superblock_tole32(&superblock);
    err = lfs_dir_commit(lfs, &root, LFS_MKATTRS(
            {tag, &superblock}));
    if (err) {
        return err;
    }
    return 0;
}
#endif

#ifdef LFS_MIGRATE
////// Migration from littelfs v1 below this //////

/// Version info ///

// Software library version
// Major (top-nibble), incremented on backwards incompatible changes
// Minor (bottom-nibble), incremented on feature additions
#define LFS1_VERSION 0x00010007
#define LFS1_VERSION_MAJOR (0xffff & (LFS1_VERSION >> 16))
#define LFS1_VERSION_MINOR (0xffff & (LFS1_VERSION >>  0))

// Version of On-disk data structures
// Major (top-nibble), incremented on backwards incompatible changes
// Minor (bottom-nibble), incremented on feature additions
#define LFS1_DISK_VERSION 0x00010001
#define LFS1_DISK_VERSION_MAJOR (0xffff & (LFS1_DISK_VERSION >> 16))
#define LFS1_DISK_VERSION_MINOR (0xffff & (LFS1_DISK_VERSION >>  0))


/// v1 Definitions ///

// File types
enum lfs1_type {
    LFS1_TYPE_REG        = 0x11,
    LFS1_TYPE_DIR        = 0x22,
    LFS1_TYPE_SUPERBLOCK = 0x2e,
};

typedef struct lfs1 {
    lfs_block_t root[2];
} lfs1_t;

typedef struct lfs1_entry {
    lfs_off_t off;

    struct lfs1_disk_entry {
        uint8_t type;
        uint8_t elen;
        uint8_t alen;
        uint8_t nlen;
        union {
            struct {
                lfs_block_t head;
                lfs_size_t size;
            } file;
            lfs_block_t dir[2];
        } u;
    } d;
} lfs1_entry_t;

typedef struct lfs1_dir {
    struct lfs1_dir *next;
    lfs_block_t pair[2];
    lfs_off_t off;

    lfs_block_t head[2];
    lfs_off_t pos;

    struct lfs1_disk_dir {
        uint32_t rev;
        lfs_size_t size;
        lfs_block_t tail[2];
    } d;
} lfs1_dir_t;

typedef struct lfs1_superblock {
    lfs_off_t off;

    struct lfs1_disk_superblock {
        uint8_t type;
        uint8_t elen;
        uint8_t alen;
        uint8_t nlen;
        lfs_block_t root[2];
        uint32_t block_size;
        uint32_t block_count;
        uint32_t version;
        char magic[8];
    } d;
} lfs1_superblock_t;


/// Low-level wrappers v1->v2 ///
/** @brief v1 迁移辅助：CRC 累加的指针包装 */
static void lfs1_crc(uint32_t *crc, const void *buffer, size_t size) {
    *crc = lfs_crc(*crc, buffer, size);
}

/** @brief v1 迁移辅助：带双缓存的块读（v1 盘格式仍走 v2 的缓存层） */
static int lfs1_bd_read(lfs_t *lfs, lfs_block_t block,
        lfs_off_t off, void *buffer, lfs_size_t size) {
    // if we ever do more than writes to alternating pairs,
    // this may need to consider pcache
    return lfs_bd_read(lfs, &lfs->pcache, &lfs->rcache, size,
            block, off, buffer, size);
}

/** @brief v1 迁移辅助：逐字节流式 CRC（v1 目录校验用） */
static int lfs1_bd_crc(lfs_t *lfs, lfs_block_t block,
        lfs_off_t off, lfs_size_t size, uint32_t *crc) {
    for (lfs_off_t i = 0; i < size; i++) {
        uint8_t c;
        int err = lfs1_bd_read(lfs, block, off+i, &c, 1);
        if (err) {
            return err;
        }

        lfs1_crc(crc, &c, 1);
    }

    return 0;
}


/// Endian swapping functions ///
/** @brief v1 目录头 {rev,size,tail} 端序转换（读盘） */
static void lfs1_dir_fromle32(struct lfs1_disk_dir *d) {
    d->rev     = lfs_fromle32(d->rev);
    d->size    = lfs_fromle32(d->size);
    d->tail[0] = lfs_fromle32(d->tail[0]);
    d->tail[1] = lfs_fromle32(d->tail[1]);
}

/** @brief v1 目录头端序转换（写盘） */
static void lfs1_dir_tole32(struct lfs1_disk_dir *d) {
    d->rev     = lfs_tole32(d->rev);
    d->size    = lfs_tole32(d->size);
    d->tail[0] = lfs_tole32(d->tail[0]);
    d->tail[1] = lfs_tole32(d->tail[1]);
}

/** @brief v1 条目端序转换（读盘） */
static void lfs1_entry_fromle32(struct lfs1_disk_entry *d) {
    d->u.dir[0] = lfs_fromle32(d->u.dir[0]);
    d->u.dir[1] = lfs_fromle32(d->u.dir[1]);
}

/** @brief v1 条目端序转换（写盘） */
static void lfs1_entry_tole32(struct lfs1_disk_entry *d) {
    d->u.dir[0] = lfs_tole32(d->u.dir[0]);
    d->u.dir[1] = lfs_tole32(d->u.dir[1]);
}

/** @brief v1 超块端序转换（读盘） */
static void lfs1_superblock_fromle32(struct lfs1_disk_superblock *d) {
    d->root[0]     = lfs_fromle32(d->root[0]);
    d->root[1]     = lfs_fromle32(d->root[1]);
    d->block_size  = lfs_fromle32(d->block_size);
    d->block_count = lfs_fromle32(d->block_count);
    d->version     = lfs_fromle32(d->version);
}


///// Metadata pair and directory operations ///
/** @brief v1 条目尺寸 = 4 字节头 + elen + alen + nlen */
static inline lfs_size_t lfs1_entry_size(const lfs1_entry_t *entry) {
    return 4 + entry->d.elen + entry->d.alen + entry->d.nlen;
}

/**
 * @brief v1 目录对读取：按 rev 选新块 + 整块 CRC 校验
 * @note  v1 的目录是"整块提交"（无 per-commit CRC 链），校验失败
 *        直接弃块，只信 CRC=0 的那块。
 */
static int lfs1_dir_fetch(lfs_t *lfs,
        lfs1_dir_t *dir, const lfs_block_t pair[2]) {
    // copy out pair, otherwise may be aliasing dir
    const lfs_block_t tpair[2] = {pair[0], pair[1]};
    bool valid = false;

    // check both blocks for the most recent revision
    for (int i = 0; i < 2; i++) {
        struct lfs1_disk_dir test;
        int err = lfs1_bd_read(lfs, tpair[i], 0, &test, sizeof(test));
        lfs1_dir_fromle32(&test);
        if (err) {
            if (err == LFS_ERR_CORRUPT) {
                continue;
            }
            return err;
        }

        if (valid && lfs_scmp(test.rev, dir->d.rev) < 0) {
            continue;
        }

        if ((0x7fffffff & test.size) < sizeof(test)+4 ||
            (0x7fffffff & test.size) > lfs->cfg->block_size) {
            continue;
        }

        uint32_t crc = 0xffffffff;
        lfs1_dir_tole32(&test);
        lfs1_crc(&crc, &test, sizeof(test));
        lfs1_dir_fromle32(&test);
        err = lfs1_bd_crc(lfs, tpair[i], sizeof(test),
                (0x7fffffff & test.size) - sizeof(test), &crc);
        if (err) {
            if (err == LFS_ERR_CORRUPT) {
                continue;
            }
            return err;
        }

        if (crc != 0) {
            continue;
        }

        valid = true;

        // setup dir in case it's valid
        dir->pair[0] = tpair[(i+0) % 2];
        dir->pair[1] = tpair[(i+1) % 2];
        dir->off = sizeof(dir->d);
        dir->d = test;
    }

    if (!valid) {
        LFS_ERROR("Corrupted dir pair at {0x%"PRIx32", 0x%"PRIx32"}",
                tpair[0], tpair[1]);
        return LFS_ERR_CORRUPT;
    }

    return 0;
}

/**
 * @brief v1 目录条目迭代：跨块沿 tail；size 位31=还有后续块
 */
static int lfs1_dir_next(lfs_t *lfs, lfs1_dir_t *dir, lfs1_entry_t *entry) {
    while (dir->off + sizeof(entry->d) > (0x7fffffff & dir->d.size)-4) {
        if (!(0x80000000 & dir->d.size)) {
            entry->off = dir->off;
            return LFS_ERR_NOENT;
        }

        int err = lfs1_dir_fetch(lfs, dir, dir->d.tail);
        if (err) {
            return err;
        }

        dir->off = sizeof(dir->d);
        dir->pos += sizeof(dir->d) + 4;
    }

    int err = lfs1_bd_read(lfs, dir->pair[0], dir->off,
            &entry->d, sizeof(entry->d));
    lfs1_entry_fromle32(&entry->d);
    if (err) {
        return err;
    }

    entry->off = dir->off;
    dir->off += lfs1_entry_size(entry);
    dir->pos += lfs1_entry_size(entry);
    return 0;
}

/// littlefs v1 specific operations ///
/**
 * @brief 遍历 v1 盘全部在用块（迁移期间的分配器扫描地基）
 * @note  除 v1 自身的目录/文件块外，还要兼顾"已经迁了一半"的
 *        v2 目录链（迁移是逐目录完成的，中途可能掉电）。
 */
int lfs1_traverse(lfs_t *lfs, int (*cb)(void*, lfs_block_t), void *data) {
    if (lfs_pair_isnull(lfs->lfs1->root)) {
        return 0;
    }

    // iterate over metadata pairs
    lfs1_dir_t dir;
    lfs1_entry_t entry;
    lfs_block_t cwd[2] = {0, 1};

    while (true) {
        for (int i = 0; i < 2; i++) {
            int err = cb(data, cwd[i]);
            if (err) {
                return err;
            }
        }

        int err = lfs1_dir_fetch(lfs, &dir, cwd);
        if (err) {
            return err;
        }

        // iterate over contents
        while (dir.off + sizeof(entry.d) <= (0x7fffffff & dir.d.size)-4) {
            err = lfs1_bd_read(lfs, dir.pair[0], dir.off,
                    &entry.d, sizeof(entry.d));
            lfs1_entry_fromle32(&entry.d);
            if (err) {
                return err;
            }

            dir.off += lfs1_entry_size(&entry);
            if ((0x70 & entry.d.type) == (0x70 & LFS1_TYPE_REG)) {
                err = lfs_ctz_traverse(lfs, NULL, &lfs->rcache,
                        entry.d.u.file.head, entry.d.u.file.size, cb, data);
                if (err) {
                    return err;
                }
            }
        }

        // we also need to check if we contain a threaded v2 directory
        lfs_mdir_t dir2 = {.split=true, .tail={cwd[0], cwd[1]}};
        while (dir2.split) {
            err = lfs_dir_fetch(lfs, &dir2, dir2.tail);
            if (err) {
                break;
            }

            for (int i = 0; i < 2; i++) {
                err = cb(data, dir2.pair[i]);
                if (err) {
                    return err;
                }
            }
        }

        cwd[0] = dir.d.tail[0];
        cwd[1] = dir.d.tail[1];

        if (lfs_pair_isnull(cwd)) {
            break;
        }
    }

    return 0;
}

/**
 * @brief 检查 v1 条目是否已被 move（type bit7 且盘上另有副本）
 * @note  v1 的 move 实现是"打标记+全盘查重"，迁移时必须重演这套
 *        判定，否则会复制出重复文件。
 */
static int lfs1_moved(lfs_t *lfs, const void *e) {
    if (lfs_pair_isnull(lfs->lfs1->root)) {
        return 0;
    }

    // skip superblock
    lfs1_dir_t cwd;
    int err = lfs1_dir_fetch(lfs, &cwd, (const lfs_block_t[2]){0, 1});
    if (err) {
        return err;
    }

    // iterate over all directory directory entries
    lfs1_entry_t entry;
    while (!lfs_pair_isnull(cwd.d.tail)) {
        err = lfs1_dir_fetch(lfs, &cwd, cwd.d.tail);
        if (err) {
            return err;
        }

        while (true) {
            err = lfs1_dir_next(lfs, &cwd, &entry);
            if (err && err != LFS_ERR_NOENT) {
                return err;
            }

            if (err == LFS_ERR_NOENT) {
                break;
            }

            if (!(0x80 & entry.d.type) &&
                 memcmp(&entry.d.u, e, sizeof(entry.d.u)) == 0) {
                return true;
            }
        }
    }

    return false;
}

/// Filesystem operations ///
/**
 * @brief 挂载 v1 盘（迁移第一步）：校验 magic 与版本，取 root 指针
 */
static int lfs1_mount(lfs_t *lfs, struct lfs1 *lfs1,
        const struct lfs_config *cfg) {
    int err = 0;
    {
        err = lfs_init(lfs, cfg);
        if (err) {
            return err;
        }

        lfs->lfs1 = lfs1;
        lfs->lfs1->root[0] = LFS_BLOCK_NULL;
        lfs->lfs1->root[1] = LFS_BLOCK_NULL;

        // setup free lookahead
        lfs->lookahead.start = 0;
        lfs->lookahead.size = 0;
        lfs->lookahead.next = 0;
        lfs_alloc_ckpoint(lfs);

        // load superblock
        lfs1_dir_t dir;
        lfs1_superblock_t superblock;
        err = lfs1_dir_fetch(lfs, &dir, (const lfs_block_t[2]){0, 1});
        if (err && err != LFS_ERR_CORRUPT) {
            goto cleanup;
        }

        if (!err) {
            err = lfs1_bd_read(lfs, dir.pair[0], sizeof(dir.d),
                    &superblock.d, sizeof(superblock.d));
            lfs1_superblock_fromle32(&superblock.d);
            if (err) {
                goto cleanup;
            }

            lfs->lfs1->root[0] = superblock.d.root[0];
            lfs->lfs1->root[1] = superblock.d.root[1];
        }

        if (err || memcmp(superblock.d.magic, "littlefs", 8) != 0) {
            LFS_ERROR("Invalid superblock at {0x%"PRIx32", 0x%"PRIx32"}",
                    0, 1);
            err = LFS_ERR_CORRUPT;
            goto cleanup;
        }

        uint16_t major_version = (0xffff & (superblock.d.version >> 16));
        uint16_t minor_version = (0xffff & (superblock.d.version >>  0));
        if ((major_version != LFS1_DISK_VERSION_MAJOR ||
             minor_version > LFS1_DISK_VERSION_MINOR)) {
            LFS_ERROR("Invalid version v%d.%d", major_version, minor_version);
            err = LFS_ERR_INVAL;
            goto cleanup;
        }

        return 0;
    }

cleanup:
    lfs_deinit(lfs);
    return err;
}

/** @brief v1 侧卸载（同 lfs_deinit） */
static int lfs1_unmount(lfs_t *lfs) {
    return lfs_deinit(lfs);
}

/// v1 migration ///
/**
 * @brief v1→v2 迁移：逐目录重建为 v2 元数据对，最后重写超块
 * @note  迁移期间禁止磨损搬迁（见 lfs_dir_compact 的 lfs1 分支）——
 *        relocating 会破坏 v1 的目录指针。每个 v1 目录：新建 v2 对
 *        →逐条目搬（move 标记需全盘查重）→把 v1 尾链进 v2；
 *        最后把新超块写进 {0,1} 对（复用 v1 的块，rev 延续），
 *        并强制 compact 防止再被 v1 挂载。任何一步掉电：
 *        v1 数据未动（只追加），重跑迁移即可。
 */
static int lfs_migrate_(lfs_t *lfs, const struct lfs_config *cfg) {
    struct lfs1 lfs1;

    // Indeterminate filesystem size not allowed for migration.
    LFS_ASSERT(cfg->block_count != 0);

    int err = lfs1_mount(lfs, &lfs1, cfg);
    if (err) {
        return err;
    }

    {
        // iterate through each directory, copying over entries
        // into new directory
        lfs1_dir_t dir1;
        lfs_mdir_t dir2;
        dir1.d.tail[0] = lfs->lfs1->root[0];
        dir1.d.tail[1] = lfs->lfs1->root[1];
        while (!lfs_pair_isnull(dir1.d.tail)) {
            // iterate old dir
            err = lfs1_dir_fetch(lfs, &dir1, dir1.d.tail);
            if (err) {
                goto cleanup;
            }

            // create new dir and bind as temporary pretend root
            err = lfs_dir_alloc(lfs, &dir2);
            if (err) {
                goto cleanup;
            }

            dir2.rev = dir1.d.rev;
            dir1.head[0] = dir1.pair[0];
            dir1.head[1] = dir1.pair[1];
            lfs->root[0] = dir2.pair[0];
            lfs->root[1] = dir2.pair[1];

            err = lfs_dir_commit(lfs, &dir2, NULL, 0);
            if (err) {
                goto cleanup;
            }

            while (true) {
                lfs1_entry_t entry1;
                err = lfs1_dir_next(lfs, &dir1, &entry1);
                if (err && err != LFS_ERR_NOENT) {
                    goto cleanup;
                }

                if (err == LFS_ERR_NOENT) {
                    break;
                }

                // check that entry has not been moved
                if (entry1.d.type & 0x80) {
                    int moved = lfs1_moved(lfs, &entry1.d.u);
                    if (moved < 0) {
                        err = moved;
                        goto cleanup;
                    }

                    if (moved) {
                        continue;
                    }

                    entry1.d.type &= ~0x80;
                }

                // also fetch name
                char name[LFS_NAME_MAX+1];
                memset(name, 0, sizeof(name));
                err = lfs1_bd_read(lfs, dir1.pair[0],
                        entry1.off + 4+entry1.d.elen+entry1.d.alen,
                        name, entry1.d.nlen);
                if (err) {
                    goto cleanup;
                }

                bool isdir = (entry1.d.type == LFS1_TYPE_DIR);

                // create entry in new dir
                err = lfs_dir_fetch(lfs, &dir2, lfs->root);
                if (err) {
                    goto cleanup;
                }

                uint16_t id;
                err = lfs_dir_find(lfs, &dir2, &(const char*){name}, &id);
                if (!(err == LFS_ERR_NOENT && id != 0x3ff)) {
                    err = (err < 0) ? err : LFS_ERR_EXIST;
                    goto cleanup;
                }

                lfs1_entry_tole32(&entry1.d);
                err = lfs_dir_commit(lfs, &dir2, LFS_MKATTRS(
                        {LFS_MKTAG(LFS_TYPE_CREATE, id, 0), NULL},
                        {LFS_MKTAG_IF_ELSE(isdir,
                            LFS_TYPE_DIR, id, entry1.d.nlen,
                            LFS_TYPE_REG, id, entry1.d.nlen),
                                name},
                        {LFS_MKTAG_IF_ELSE(isdir,
                            LFS_TYPE_DIRSTRUCT, id, sizeof(entry1.d.u),
                            LFS_TYPE_CTZSTRUCT, id, sizeof(entry1.d.u)),
                                &entry1.d.u}));
                lfs1_entry_fromle32(&entry1.d);
                if (err) {
                    goto cleanup;
                }
            }

            if (!lfs_pair_isnull(dir1.d.tail)) {
                // find last block and update tail to thread into fs
                err = lfs_dir_fetch(lfs, &dir2, lfs->root);
                if (err) {
                    goto cleanup;
                }

                while (dir2.split) {
                    err = lfs_dir_fetch(lfs, &dir2, dir2.tail);
                    if (err) {
                        goto cleanup;
                    }
                }

                lfs_pair_tole32(dir2.pair);
                err = lfs_dir_commit(lfs, &dir2, LFS_MKATTRS(
                        {LFS_MKTAG(LFS_TYPE_SOFTTAIL, 0x3ff, 8), dir1.d.tail}));
                lfs_pair_fromle32(dir2.pair);
                if (err) {
                    goto cleanup;
                }
            }

            // Copy over first block to thread into fs. Unfortunately
            // if this fails there is not much we can do.
            LFS_DEBUG("Migrating {0x%"PRIx32", 0x%"PRIx32"} "
                        "-> {0x%"PRIx32", 0x%"PRIx32"}",
                    lfs->root[0], lfs->root[1], dir1.head[0], dir1.head[1]);

            err = lfs_bd_erase(lfs, dir1.head[1]);
            if (err) {
                goto cleanup;
            }

            err = lfs_dir_fetch(lfs, &dir2, lfs->root);
            if (err) {
                goto cleanup;
            }

            for (lfs_off_t i = 0; i < dir2.off; i++) {
                uint8_t dat;
                err = lfs_bd_read(lfs,
                        NULL, &lfs->rcache, dir2.off,
                        dir2.pair[0], i, &dat, 1);
                if (err) {
                    goto cleanup;
                }

                err = lfs_bd_prog(lfs,
                        &lfs->pcache, &lfs->rcache, true,
                        dir1.head[1], i, &dat, 1);
                if (err) {
                    goto cleanup;
                }
            }

            err = lfs_bd_flush(lfs, &lfs->pcache, &lfs->rcache, true);
            if (err) {
                goto cleanup;
            }
        }

        // Create new superblock. This marks a successful migration!
        err = lfs1_dir_fetch(lfs, &dir1, (const lfs_block_t[2]){0, 1});
        if (err) {
            goto cleanup;
        }

        dir2.pair[0] = dir1.pair[0];
        dir2.pair[1] = dir1.pair[1];
        dir2.rev = dir1.d.rev;
        dir2.off = sizeof(dir2.rev);
        dir2.etag = 0xffffffff;
        dir2.count = 0;
        dir2.tail[0] = lfs->lfs1->root[0];
        dir2.tail[1] = lfs->lfs1->root[1];
        dir2.erased = false;
        dir2.split = true;

        lfs_superblock_t superblock = {
            .version     = LFS_DISK_VERSION,
            .block_size  = lfs->cfg->block_size,
            .block_count = lfs->cfg->block_count,
            .name_max    = lfs->name_max,
            .file_max    = lfs->file_max,
            .attr_max    = lfs->attr_max,
        };

        lfs_superblock_tole32(&superblock);
        err = lfs_dir_commit(lfs, &dir2, LFS_MKATTRS(
                {LFS_MKTAG(LFS_TYPE_CREATE, 0, 0), NULL},
                {LFS_MKTAG(LFS_TYPE_SUPERBLOCK, 0, 8), "littlefs"},
                {LFS_MKTAG(LFS_TYPE_INLINESTRUCT, 0, sizeof(superblock)),
                    &superblock}));
        if (err) {
            goto cleanup;
        }

        // sanity check that fetch works
        err = lfs_dir_fetch(lfs, &dir2, (const lfs_block_t[2]){0, 1});
        if (err) {
            goto cleanup;
        }

        // force compaction to prevent accidentally mounting v1
        dir2.erased = false;
        err = lfs_dir_commit(lfs, &dir2, NULL, 0);
        if (err) {
            goto cleanup;
        }
    }

cleanup:
    lfs1_unmount(lfs);
    return err;
}

#endif


/// Public API wrappers ///

// 公共 API 薄壳层：每个入口 = 加锁 → TRACE 进参 → 调内部实现
// → TRACE 返回值 → 解锁。LFS_THREADSAFE 时锁粒度为"整个调用"，
// 多线程可安全共享同一 lfs_t；未定义时退化为空操作。
// 断言：打开的 file/dir 必须在 mlist 上（防未开先用/重复打开）。
// Here we can add tracing/thread safety easily

// Thread-safe wrappers if enabled
#ifdef LFS_THREADSAFE
#define LFS_LOCK(cfg)   cfg->lock(cfg)
#define LFS_UNLOCK(cfg) cfg->unlock(cfg)
#else
#define LFS_LOCK(cfg)   ((void)cfg, 0)
#define LFS_UNLOCK(cfg) ((void)cfg)
#endif

// Public API
#ifndef LFS_READONLY
/** @brief 格式化块设备（clobbers lfs 对象，返回后处于未挂载态） */
int lfs_format(lfs_t *lfs, const struct lfs_config *cfg) {
    int err = LFS_LOCK(cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_format(%p, %p {.context=%p, "
                ".read=%p, .prog=%p, .erase=%p, .sync=%p, "
                ".read_size=%"PRIu32", .prog_size=%"PRIu32", "
                ".block_size=%"PRIu32", .block_count=%"PRIu32", "
                ".block_cycles=%"PRId32", .cache_size=%"PRIu32", "
                ".lookahead_size=%"PRIu32", .read_buffer=%p, "
                ".prog_buffer=%p, .lookahead_buffer=%p, "
                ".name_max=%"PRIu32", .file_max=%"PRIu32", "
                ".attr_max=%"PRIu32"})",
            (void*)lfs, (void*)cfg, cfg->context,
            (void*)(uintptr_t)cfg->read, (void*)(uintptr_t)cfg->prog,
            (void*)(uintptr_t)cfg->erase, (void*)(uintptr_t)cfg->sync,
            cfg->read_size, cfg->prog_size, cfg->block_size, cfg->block_count,
            cfg->block_cycles, cfg->cache_size, cfg->lookahead_size,
            cfg->read_buffer, cfg->prog_buffer, cfg->lookahead_buffer,
            cfg->name_max, cfg->file_max, cfg->attr_max);

    err = lfs_format_(lfs, cfg);

    LFS_TRACE("lfs_format -> %d", err);
    LFS_UNLOCK(cfg);
    return err;
}
#endif

/** @brief 挂载；多个 lfs_t 可各自挂不同设备（对象由用户分配） */
int lfs_mount(lfs_t *lfs, const struct lfs_config *cfg) {
    int err = LFS_LOCK(cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_mount(%p, %p {.context=%p, "
                ".read=%p, .prog=%p, .erase=%p, .sync=%p, "
                ".read_size=%"PRIu32", .prog_size=%"PRIu32", "
                ".block_size=%"PRIu32", .block_count=%"PRIu32", "
                ".block_cycles=%"PRId32", .cache_size=%"PRIu32", "
                ".lookahead_size=%"PRIu32", .read_buffer=%p, "
                ".prog_buffer=%p, .lookahead_buffer=%p, "
                ".name_max=%"PRIu32", .file_max=%"PRIu32", "
                ".attr_max=%"PRIu32"})",
            (void*)lfs, (void*)cfg, cfg->context,
            (void*)(uintptr_t)cfg->read, (void*)(uintptr_t)cfg->prog,
            (void*)(uintptr_t)cfg->erase, (void*)(uintptr_t)cfg->sync,
            cfg->read_size, cfg->prog_size, cfg->block_size, cfg->block_count,
            cfg->block_cycles, cfg->cache_size, cfg->lookahead_size,
            cfg->read_buffer, cfg->prog_buffer, cfg->lookahead_buffer,
            cfg->name_max, cfg->file_max, cfg->attr_max);

    err = lfs_mount_(lfs, cfg);

    LFS_TRACE("lfs_mount -> %d", err);
    LFS_UNLOCK(cfg);
    return err;
}

/** @brief 卸载并释放资源（所有脏数据在此之前已由 sync/close 提交） */
int lfs_unmount(lfs_t *lfs) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_unmount(%p)", (void*)lfs);

    err = lfs_unmount_(lfs);

    LFS_TRACE("lfs_unmount -> %d", err);
    LFS_UNLOCK(lfs->cfg);
    return err;
}

#ifndef LFS_READONLY
/** @brief 删除文件/目录（目录必须为空）；原子，掉电不产生半删除 */
int lfs_remove(lfs_t *lfs, const char *path) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_remove(%p, \"%s\")", (void*)lfs, path);

    err = lfs_remove_(lfs, path);

    LFS_TRACE("lfs_remove -> %d", err);
    LFS_UNLOCK(lfs->cfg);
    return err;
}
#endif

#ifndef LFS_READONLY
/**
 * @brief 重命名/移动；目标存在须同类型、目录须空；
 *        跨目录由 gstate 保证原子（见 lfs_rename_）
 */
int lfs_rename(lfs_t *lfs, const char *oldpath, const char *newpath) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_rename(%p, \"%s\", \"%s\")", (void*)lfs, oldpath, newpath);

    err = lfs_rename_(lfs, oldpath, newpath);

    LFS_TRACE("lfs_rename -> %d", err);
    LFS_UNLOCK(lfs->cfg);
    return err;
}
#endif

/** @brief 查询文件/目录信息（type/size/name） */
int lfs_stat(lfs_t *lfs, const char *path, struct lfs_info *info) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_stat(%p, \"%s\", %p)", (void*)lfs, path, (void*)info);

    err = lfs_stat_(lfs, path, info);

    LFS_TRACE("lfs_stat -> %d", err);
    LFS_UNLOCK(lfs->cfg);
    return err;
}

/** @brief 读自定义属性；返回盘上长度（可与缓冲不等），无则 NOATTR */
lfs_ssize_t lfs_getattr(lfs_t *lfs, const char *path,
        uint8_t type, void *buffer, lfs_size_t size) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_getattr(%p, \"%s\", %"PRIu8", %p, %"PRIu32")",
            (void*)lfs, path, type, buffer, size);

    lfs_ssize_t res = lfs_getattr_(lfs, path, type, buffer, size);

    LFS_TRACE("lfs_getattr -> %"PRId32, res);
    LFS_UNLOCK(lfs->cfg);
    return res;
}

#ifndef LFS_READONLY
/** @brief 写自定义属性（不存在则隐式创建）；随一次 commit 原子生效 */
int lfs_setattr(lfs_t *lfs, const char *path,
        uint8_t type, const void *buffer, lfs_size_t size) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_setattr(%p, \"%s\", %"PRIu8", %p, %"PRIu32")",
            (void*)lfs, path, type, buffer, size);

    err = lfs_setattr_(lfs, path, type, buffer, size);

    LFS_TRACE("lfs_setattr -> %d", err);
    LFS_UNLOCK(lfs->cfg);
    return err;
}
#endif

#ifndef LFS_READONLY
/** @brief 删自定义属性（不存在则无操作） */
int lfs_removeattr(lfs_t *lfs, const char *path, uint8_t type) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_removeattr(%p, \"%s\", %"PRIu8")", (void*)lfs, path, type);

    err = lfs_removeattr_(lfs, path, type);

    LFS_TRACE("lfs_removeattr -> %d", err);
    LFS_UNLOCK(lfs->cfg);
    return err;
}
#endif

#ifndef LFS_NO_MALLOC
/** @brief 打开文件（默认配置，缓存 malloc；LFS_NO_MALLOC 时用 opencfg） */
int lfs_file_open(lfs_t *lfs, lfs_file_t *file, const char *path, int flags) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_file_open(%p, %p, \"%s\", %x)",
            (void*)lfs, (void*)file, path, (unsigned)flags);
    LFS_ASSERT(!lfs_mlist_isopen(lfs->mlist, (struct lfs_mlist*)file));

    err = lfs_file_open_(lfs, file, path, flags);

    LFS_TRACE("lfs_file_open -> %d", err);
    LFS_UNLOCK(lfs->cfg);
    return err;
}
#endif

/**
 * @brief 打开文件（带 per-file 配置：静态缓存/自定义属性列表）
 * @note  attrs 在 open 时读入、sync/close 时随文件内容原子写回
 */
int lfs_file_opencfg(lfs_t *lfs, lfs_file_t *file,
        const char *path, int flags,
        const struct lfs_file_config *cfg) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_file_opencfg(%p, %p, \"%s\", %x, %p {"
                 ".buffer=%p, .attrs=%p, .attr_count=%"PRIu32"})",
            (void*)lfs, (void*)file, path, (unsigned)flags,
            (void*)cfg, cfg->buffer, (void*)cfg->attrs, cfg->attr_count);
    LFS_ASSERT(!lfs_mlist_isopen(lfs->mlist, (struct lfs_mlist*)file));

    err = lfs_file_opencfg_(lfs, file, path, flags, cfg);

    LFS_TRACE("lfs_file_opencfg -> %d", err);
    LFS_UNLOCK(lfs->cfg);
    return err;
}

/** @brief 关闭文件：隐式 sync（这是"写必须 close/sync 才落盘"的落点） */
int lfs_file_close(lfs_t *lfs, lfs_file_t *file) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_file_close(%p, %p)", (void*)lfs, (void*)file);
    LFS_ASSERT(lfs_mlist_isopen(lfs->mlist, (struct lfs_mlist*)file));

    err = lfs_file_close_(lfs, file);

    LFS_TRACE("lfs_file_close -> %d", err);
    LFS_UNLOCK(lfs->cfg);
    return err;
}

#ifndef LFS_READONLY
/** @brief 手动提交文件（数据+元数据一起原子落盘） */
int lfs_file_sync(lfs_t *lfs, lfs_file_t *file) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_file_sync(%p, %p)", (void*)lfs, (void*)file);
    LFS_ASSERT(lfs_mlist_isopen(lfs->mlist, (struct lfs_mlist*)file));

    err = lfs_file_sync_(lfs, file);

    LFS_TRACE("lfs_file_sync -> %d", err);
    LFS_UNLOCK(lfs->cfg);
    return err;
}
#endif

/** @brief 读文件；返回实际读取字节数（可为 0=EOF） */
lfs_ssize_t lfs_file_read(lfs_t *lfs, lfs_file_t *file,
        void *buffer, lfs_size_t size) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_file_read(%p, %p, %p, %"PRIu32")",
            (void*)lfs, (void*)file, buffer, size);
    LFS_ASSERT(lfs_mlist_isopen(lfs->mlist, (struct lfs_mlist*)file));

    lfs_ssize_t res = lfs_file_read_(lfs, file, buffer, size);

    LFS_TRACE("lfs_file_read -> %"PRId32, res);
    LFS_UNLOCK(lfs->cfg);
    return res;
}

#ifndef LFS_READONLY
/** @brief 写文件；close/sync 前只在 RAM 缓存（掉电丢本次未提交写入） */
lfs_ssize_t lfs_file_write(lfs_t *lfs, lfs_file_t *file,
        const void *buffer, lfs_size_t size) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_file_write(%p, %p, %p, %"PRIu32")",
            (void*)lfs, (void*)file, buffer, size);
    LFS_ASSERT(lfs_mlist_isopen(lfs->mlist, (struct lfs_mlist*)file));

    lfs_ssize_t res = lfs_file_write_(lfs, file, buffer, size);

    LFS_TRACE("lfs_file_write -> %"PRId32, res);
    LFS_UNLOCK(lfs->cfg);
    return res;
}
#endif

/** @brief 定位（SET/CUR/END）；越 file_max 报 INVAL */
lfs_soff_t lfs_file_seek(lfs_t *lfs, lfs_file_t *file,
        lfs_soff_t off, int whence) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_file_seek(%p, %p, %"PRId32", %d)",
            (void*)lfs, (void*)file, off, whence);
    LFS_ASSERT(lfs_mlist_isopen(lfs->mlist, (struct lfs_mlist*)file));

    lfs_soff_t res = lfs_file_seek_(lfs, file, off, whence);

    LFS_TRACE("lfs_file_seek -> %"PRId32, res);
    LFS_UNLOCK(lfs->cfg);
    return res;
}

#ifndef LFS_READONLY
/** @brief 截断/扩展文件到指定大小（扩展补 0） */
int lfs_file_truncate(lfs_t *lfs, lfs_file_t *file, lfs_off_t size) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_file_truncate(%p, %p, %"PRIu32")",
            (void*)lfs, (void*)file, size);
    LFS_ASSERT(lfs_mlist_isopen(lfs->mlist, (struct lfs_mlist*)file));

    err = lfs_file_truncate_(lfs, file, size);

    LFS_TRACE("lfs_file_truncate -> %d", err);
    LFS_UNLOCK(lfs->cfg);
    return err;
}
#endif

/** @brief 返回当前读写位置 */
lfs_soff_t lfs_file_tell(lfs_t *lfs, lfs_file_t *file) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_file_tell(%p, %p)", (void*)lfs, (void*)file);
    LFS_ASSERT(lfs_mlist_isopen(lfs->mlist, (struct lfs_mlist*)file));

    lfs_soff_t res = lfs_file_tell_(lfs, file);

    LFS_TRACE("lfs_file_tell -> %"PRId32, res);
    LFS_UNLOCK(lfs->cfg);
    return res;
}

/** @brief 回到文件头 */
int lfs_file_rewind(lfs_t *lfs, lfs_file_t *file) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_file_rewind(%p, %p)", (void*)lfs, (void*)file);

    err = lfs_file_rewind_(lfs, file);

    LFS_TRACE("lfs_file_rewind -> %d", err);
    LFS_UNLOCK(lfs->cfg);
    return err;
}

/** @brief 文件大小（含未提交写入，WRITING 时取 max(pos,size)） */
lfs_soff_t lfs_file_size(lfs_t *lfs, lfs_file_t *file) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_file_size(%p, %p)", (void*)lfs, (void*)file);
    LFS_ASSERT(lfs_mlist_isopen(lfs->mlist, (struct lfs_mlist*)file));

    lfs_soff_t res = lfs_file_size_(lfs, file);

    LFS_TRACE("lfs_file_size -> %"PRIu32, res);
    LFS_UNLOCK(lfs->cfg);
    return res;
}

#ifndef LFS_READONLY
/** @brief 创建目录（父目录不存在报 NOENT，重名报 EXIST） */
int lfs_mkdir(lfs_t *lfs, const char *path) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_mkdir(%p, \"%s\")", (void*)lfs, path);

    err = lfs_mkdir_(lfs, path);

    LFS_TRACE("lfs_mkdir -> %d", err);
    LFS_UNLOCK(lfs->cfg);
    return err;
}
#endif

/** @brief 打开目录以便 dir_read 迭代 */
int lfs_dir_open(lfs_t *lfs, lfs_dir_t *dir, const char *path) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_dir_open(%p, %p, \"%s\")", (void*)lfs, (void*)dir, path);
    LFS_ASSERT(!lfs_mlist_isopen(lfs->mlist, (struct lfs_mlist*)dir));

    err = lfs_dir_open_(lfs, dir, path);

    LFS_TRACE("lfs_dir_open -> %d", err);
    LFS_UNLOCK(lfs->cfg);
    return err;
}

/** @brief 关闭目录 */
int lfs_dir_close(lfs_t *lfs, lfs_dir_t *dir) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_dir_close(%p, %p)", (void*)lfs, (void*)dir);

    err = lfs_dir_close_(lfs, dir);

    LFS_TRACE("lfs_dir_close -> %d", err);
    LFS_UNLOCK(lfs->cfg);
    return err;
}

/**
 * @brief 迭代目录项；返回 1=有条目、0=结束（注意与文件 read 语义不同）
 */
int lfs_dir_read(lfs_t *lfs, lfs_dir_t *dir, struct lfs_info *info) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_dir_read(%p, %p, %p)",
            (void*)lfs, (void*)dir, (void*)info);

    err = lfs_dir_read_(lfs, dir, info);

    LFS_TRACE("lfs_dir_read -> %d", err);
    LFS_UNLOCK(lfs->cfg);
    return err;
}

/** @brief 目录定位（off 必须来自 dir_tell） */
int lfs_dir_seek(lfs_t *lfs, lfs_dir_t *dir, lfs_off_t off) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_dir_seek(%p, %p, %"PRIu32")",
            (void*)lfs, (void*)dir, off);

    err = lfs_dir_seek_(lfs, dir, off);

    LFS_TRACE("lfs_dir_seek -> %d", err);
    LFS_UNLOCK(lfs->cfg);
    return err;
}

/** @brief 目录当前位置（仅可供 seek 回放，数值本身无业务含义） */
lfs_soff_t lfs_dir_tell(lfs_t *lfs, lfs_dir_t *dir) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_dir_tell(%p, %p)", (void*)lfs, (void*)dir);

    lfs_soff_t res = lfs_dir_tell_(lfs, dir);

    LFS_TRACE("lfs_dir_tell -> %"PRId32, res);
    LFS_UNLOCK(lfs->cfg);
    return res;
}

/** @brief 目录迭代回到开头 */
int lfs_dir_rewind(lfs_t *lfs, lfs_dir_t *dir) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_dir_rewind(%p, %p)", (void*)lfs, (void*)dir);

    err = lfs_dir_rewind_(lfs, dir);

    LFS_TRACE("lfs_dir_rewind -> %d", err);
    LFS_UNLOCK(lfs->cfg);
    return err;
}

/** @brief 查询盘上几何/版本/上限（fsinfo） */
int lfs_fs_stat(lfs_t *lfs, struct lfs_fsinfo *fsinfo) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_fs_stat(%p, %p)", (void*)lfs, (void*)fsinfo);

    err = lfs_fs_stat_(lfs, fsinfo);

    LFS_TRACE("lfs_fs_stat -> %d", err);
    LFS_UNLOCK(lfs->cfg);
    return err;
}

/** @brief 已用块数（共享 COW 块重复计数，best effort） */
lfs_ssize_t lfs_fs_size(lfs_t *lfs) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_fs_size(%p)", (void*)lfs);

    lfs_ssize_t res = lfs_fs_size_(lfs);

    LFS_TRACE("lfs_fs_size -> %"PRId32, res);
    LFS_UNLOCK(lfs->cfg);
    return res;
}

/** @brief 遍历全部在用块（坏块管理/镜像/统计的底座） */
int lfs_fs_traverse(lfs_t *lfs, int (*cb)(void *, lfs_block_t), void *data) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_fs_traverse(%p, %p, %p)",
            (void*)lfs, (void*)(uintptr_t)cb, data);

    err = lfs_fs_traverse_(lfs, cb, data, true);

    LFS_TRACE("lfs_fs_traverse -> %d", err);
    LFS_UNLOCK(lfs->cfg);
    return err;
}

#ifndef LFS_READONLY
/** @brief 显式完成遗留中间态（不等下一次写操作） */
int lfs_fs_mkconsistent(lfs_t *lfs) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_fs_mkconsistent(%p)", (void*)lfs);

    err = lfs_fs_mkconsistent_(lfs);

    LFS_TRACE("lfs_fs_mkconsistent -> %d", err);
    LFS_UNLOCK(lfs->cfg);
    return err;
}
#endif

#ifndef LFS_READONLY
/** @brief 保洁：mkconsistent + 压实超阈值元数据 + 预热分配器 */
int lfs_fs_gc(lfs_t *lfs) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_fs_gc(%p)", (void*)lfs);

    err = lfs_fs_gc_(lfs);

    LFS_TRACE("lfs_fs_gc -> %d", err);
    LFS_UNLOCK(lfs->cfg);
    return err;
}
#endif

#ifndef LFS_READONLY
/** @brief 扩容到新块数（重写超块；收缩仅限无越界占用时） */
int lfs_fs_grow(lfs_t *lfs, lfs_size_t block_count) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_fs_grow(%p, %"PRIu32")", (void*)lfs, block_count);

    err = lfs_fs_grow_(lfs, block_count);

    LFS_TRACE("lfs_fs_grow -> %d", err);
    LFS_UNLOCK(lfs->cfg);
    return err;
}
#endif

#ifdef LFS_MIGRATE
/** @brief v1→v2 在线迁移（需 LFS_MIGRATE；失败时 v1 数据不受损） */
int lfs_migrate(lfs_t *lfs, const struct lfs_config *cfg) {
    int err = LFS_LOCK(cfg);
    if (err) {
        return err;
    }
    LFS_TRACE("lfs_migrate(%p, %p {.context=%p, "
                ".read=%p, .prog=%p, .erase=%p, .sync=%p, "
                ".read_size=%"PRIu32", .prog_size=%"PRIu32", "
                ".block_size=%"PRIu32", .block_count=%"PRIu32", "
                ".block_cycles=%"PRId32", .cache_size=%"PRIu32", "
                ".lookahead_size=%"PRIu32", .read_buffer=%p, "
                ".prog_buffer=%p, .lookahead_buffer=%p, "
                ".name_max=%"PRIu32", .file_max=%"PRIu32", "
                ".attr_max=%"PRIu32"})",
            (void*)lfs, (void*)cfg, cfg->context,
            (void*)(uintptr_t)cfg->read, (void*)(uintptr_t)cfg->prog,
            (void*)(uintptr_t)cfg->erase, (void*)(uintptr_t)cfg->sync,
            cfg->read_size, cfg->prog_size, cfg->block_size, cfg->block_count,
            cfg->block_cycles, cfg->cache_size, cfg->lookahead_size,
            cfg->read_buffer, cfg->prog_buffer, cfg->lookahead_buffer,
            cfg->name_max, cfg->file_max, cfg->attr_max);

    err = lfs_migrate_(lfs, cfg);

    LFS_TRACE("lfs_migrate -> %d", err);
    LFS_UNLOCK(cfg);
    return err;
}
#endif


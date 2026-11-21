/*
 * ============================================================================
 * 【中文导读】lfs.h —— littlefs 公共 API 与配置（B级导读）
 * 移植接口在 lfs_config 结构体：实现 read/prog/erase/sync 四个回调即可把
 * littlefs 搬到任何块设备上（SPI Flash/SD/片内Flash）。LFS_INLINE 宏定义
 * 与配置常量的中文解释见官方注释；lfs_file_config 决定缓冲策略。
 * ============================================================================
 */
/*
 * The little filesystem
 *
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef LFS_H
#define LFS_H

#include "lfs_util.h"

#ifdef __cplusplus
extern "C"
{
#endif


/// Version info ///

// Software library version
// Major (top-nibble), incremented on backwards incompatible changes
// Minor (bottom-nibble), incremented on feature additions
#define LFS_VERSION 0x0002000b
#define LFS_VERSION_MAJOR (0xffff & (LFS_VERSION >> 16))
#define LFS_VERSION_MINOR (0xffff & (LFS_VERSION >>  0))

// Version of On-disk data structures
// Major (top-nibble), incremented on backwards incompatible changes
// Minor (bottom-nibble), incremented on feature additions
#define LFS_DISK_VERSION 0x00020001
#define LFS_DISK_VERSION_MAJOR (0xffff & (LFS_DISK_VERSION >> 16))
#define LFS_DISK_VERSION_MINOR (0xffff & (LFS_DISK_VERSION >>  0))


/// Definitions ///

// Type definitions
typedef uint32_t lfs_size_t;
typedef uint32_t lfs_off_t;

typedef int32_t  lfs_ssize_t;
typedef int32_t  lfs_soff_t;

typedef uint32_t lfs_block_t;

// Maximum name size in bytes, may be redefined to reduce the size of the
// info struct. Limited to <= 1022. Stored in superblock and must be
// respected by other littlefs drivers.
#ifndef LFS_NAME_MAX
#define LFS_NAME_MAX 255
#endif

// Maximum size of a file in bytes, may be redefined to limit to support other
// drivers. Limited on disk to <= 2147483647. Stored in superblock and must be
// respected by other littlefs drivers.
#ifndef LFS_FILE_MAX
#define LFS_FILE_MAX 2147483647
#endif

// Maximum size of custom attributes in bytes, may be redefined, but there is
// no real benefit to using a smaller LFS_ATTR_MAX. Limited to <= 1022. Stored
// in superblock and must be respected by other littlefs drivers.
#ifndef LFS_ATTR_MAX
#define LFS_ATTR_MAX 1022
#endif

// Possible error codes, these are negative to allow
// valid positive return values
/**
 * 错误码约定：全部为负值（数值借自 POSIX errno），这样带返回值的
 * 接口（read/write/seek…）用正数表成功、负数表错误，一个 int 双语义。
 * 移植者的 bd 回调也可以直接返回这些负错误码，littlefs 原样向上传播。
 */
enum lfs_error {
    LFS_ERR_OK          = 0,    // No error
    LFS_ERR_IO          = -5,   // Error during device operation
    LFS_ERR_CORRUPT     = -84,  // Corrupted
    LFS_ERR_NOENT       = -2,   // No directory entry
    LFS_ERR_EXIST       = -17,  // Entry already exists
    LFS_ERR_NOTDIR      = -20,  // Entry is not a dir
    LFS_ERR_ISDIR       = -21,  // Entry is a dir
    LFS_ERR_NOTEMPTY    = -39,  // Dir is not empty
    LFS_ERR_BADF        = -9,   // Bad file number
    LFS_ERR_FBIG        = -27,  // File too large
    LFS_ERR_INVAL       = -22,  // Invalid parameter
    LFS_ERR_NOSPC       = -28,  // No space left on device
    LFS_ERR_NOMEM       = -12,  // No more memory available
    LFS_ERR_NOATTR      = -61,  // No data/attr available
    LFS_ERR_NAMETOOLONG = -36,  // File name too long
};

// File types
/**
 * tag 类型常量：与盘上 32 位 tag 的 type3 字段一一对应（见 SPEC.md）。
 * 0x0xx=名字、0x2xx=结构（inline/ctz/dir）、0x3xx=用户属性、
 * 0x4xx=增删（SPLICE）、0x5xx=CRC、0x6xx=tail、0x7xx=全局状态。
 * 用户只会见到 LFS_TYPE_REG/LFS_TYPE_DIR 两种。
 */
enum lfs_type {
    // file types
    LFS_TYPE_REG            = 0x001,
    LFS_TYPE_DIR            = 0x002,

    // internally used types
    LFS_TYPE_SPLICE         = 0x400,
    LFS_TYPE_NAME           = 0x000,
    LFS_TYPE_STRUCT         = 0x200,
    LFS_TYPE_USERATTR       = 0x300,
    LFS_TYPE_FROM           = 0x100,
    LFS_TYPE_TAIL           = 0x600,
    LFS_TYPE_GLOBALS        = 0x700,
    LFS_TYPE_CRC            = 0x500,

    // internally used type specializations
    LFS_TYPE_CREATE         = 0x401,
    LFS_TYPE_DELETE         = 0x4ff,
    LFS_TYPE_SUPERBLOCK     = 0x0ff,
    LFS_TYPE_DIRSTRUCT      = 0x200,
    LFS_TYPE_CTZSTRUCT      = 0x202,
    LFS_TYPE_INLINESTRUCT   = 0x201,
    LFS_TYPE_SOFTTAIL       = 0x600,
    LFS_TYPE_HARDTAIL       = 0x601,
    LFS_TYPE_MOVESTATE      = 0x7ff,
    LFS_TYPE_CCRC           = 0x500,
    LFS_TYPE_FCRC           = 0x5ff,

    // internal chip sources
    LFS_FROM_NOOP           = 0x000,
    LFS_FROM_MOVE           = 0x101,
    LFS_FROM_USERATTRS      = 0x102,
};

// File open flags
/**
 * 打开标志：低 4 位是 POSIX 风格读写模式，0x0100+ 是创建语义；
 * 0x00010000+ 是内部状态位（DIRTY/WRITING/…），用户勿设。
 * LFS_READONLY 编译时写相关标志整体消失。
 */
enum lfs_open_flags {
    // open flags
    LFS_O_RDONLY = 1,         // Open a file as read only
#ifndef LFS_READONLY
    LFS_O_WRONLY = 2,         // Open a file as write only
    LFS_O_RDWR   = 3,         // Open a file as read and write
    LFS_O_CREAT  = 0x0100,    // Create a file if it does not exist
    LFS_O_EXCL   = 0x0200,    // Fail if a file already exists
    LFS_O_TRUNC  = 0x0400,    // Truncate the existing file to zero size
    LFS_O_APPEND = 0x0800,    // Move to end of file on every write
#endif

    // internally used flags
#ifndef LFS_READONLY
    LFS_F_DIRTY   = 0x00010000, // File does not match storage due to write
    LFS_F_DUSTY   = 0x00020000, // File does not match storage due to desync
    LFS_F_WRITING = 0x00040000, // File has been written since last flush
#endif
    LFS_F_READING = 0x00080000, // File has been read since last flush
#ifndef LFS_READONLY
    LFS_F_ERRED   = 0x00100000, // An error occurred during write
#endif
    LFS_F_INLINE  = 0x01000000, // Currently inlined in directory entry
};

// File seek flags
enum lfs_whence_flags {
    LFS_SEEK_SET = 0,   // Seek relative to an absolute position
    LFS_SEEK_CUR = 1,   // Seek relative to the current file position
    LFS_SEEK_END = 2,   // Seek relative to the end of the file
};


// Configuration provided during initialization of the littlefs
/**
 * 移植的核心：把 littlefs 绑到任何块设备上的全部接口与几何参数。
 *
 * 必须实现的四个回调（context 可携带私有状态）：
 *   read  —— 读（任意对齐，块内偏移；必须返回一致数据）
 *   prog  —— 编程（只能把 1 写成 0，且目标必须先擦除）
 *   erase —— 擦除（整块恢复全 1；可返回 LFS_ERR_CORRUPT 报坏块）
 *   sync  —— 落盘（若有写缓存必须在此冲刷，否则掉电一致性失效！）
 *
 * 几何约束（lfs_init 会逐条断言）：
 *   block_size % cache_size == 0，cache_size % (read/prog_size) == 0；
 *   block_size >= 128 保证 CTZ 指针不溢出块；
 *   RAM 预算 = 2*cache_size + lookahead_size + 每文件 cache_size。
 *
 * 可选缓冲（read_buffer/prog_buffer/lookahead_buffer/静态文件缓冲）
 * 全部提供后 littlefs 运行期零 malloc。
 */
struct lfs_config {
    // Opaque user provided context that can be used to pass
    // information to the block device operations
    void *context;

    // Read a region in a block. Negative error codes are propagated
    // to the user.
    int (*read)(const struct lfs_config *c, lfs_block_t block,
            lfs_off_t off, void *buffer, lfs_size_t size);

    // Program a region in a block. The block must have previously
    // been erased. Negative error codes are propagated to the user.
    // May return LFS_ERR_CORRUPT if the block should be considered bad.
    int (*prog)(const struct lfs_config *c, lfs_block_t block,
            lfs_off_t off, const void *buffer, lfs_size_t size);

    // Erase a block. A block must be erased before being programmed.
    // The state of an erased block is undefined. Negative error codes
    // are propagated to the user.
    // May return LFS_ERR_CORRUPT if the block should be considered bad.
    int (*erase)(const struct lfs_config *c, lfs_block_t block);

    // Sync the state of the underlying block device. Negative error codes
    // are propagated to the user.
    int (*sync)(const struct lfs_config *c);

#ifdef LFS_THREADSAFE
    // Lock the underlying block device. Negative error codes
    // are propagated to the user.
    int (*lock)(const struct lfs_config *c);

    // Unlock the underlying block device. Negative error codes
    // are propagated to the user.
    int (*unlock)(const struct lfs_config *c);
#endif

    // Minimum size of a block read in bytes. All read operations will be a
    // multiple of this value.
    lfs_size_t read_size;

    // Minimum size of a block program in bytes. All program operations will be
    // a multiple of this value.
    lfs_size_t prog_size;

    // Size of an erasable block in bytes. This does not impact ram consumption
    // and may be larger than the physical erase size. However, non-inlined
    // files take up at minimum one block. Must be a multiple of the read and
    // program sizes.
    lfs_size_t block_size;

    // Number of erasable blocks on the device. Defaults to block_count stored
    // on disk when zero.
    lfs_size_t block_count;

    // Number of erase cycles before littlefs evicts metadata logs and moves
    // the metadata to another block. Suggested values are in the
    // range 100-1000, with large values having better performance at the cost
    // of less consistent wear distribution.
    //
    // Set to -1 to disable block-level wear-leveling.
    int32_t block_cycles;

    // Size of block caches in bytes. Each cache buffers a portion of a block in
    // RAM. The littlefs needs a read cache, a program cache, and one additional
    // cache per file. Larger caches can improve performance by storing more
    // data and reducing the number of disk accesses. Must be a multiple of the
    // read and program sizes, and a factor of the block size.
    lfs_size_t cache_size;

    // Size of the lookahead buffer in bytes. A larger lookahead buffer
    // increases the number of blocks found during an allocation pass. The
    // lookahead buffer is stored as a compact bitmap, so each byte of RAM
    // can track 8 blocks.
    lfs_size_t lookahead_size;

    // Threshold for metadata compaction during lfs_fs_gc in bytes. Metadata
    // pairs that exceed this threshold will be compacted during lfs_fs_gc.
    // Defaults to ~88% block_size when zero, though the default may change
    // in the future.
    //
    // Note this only affects lfs_fs_gc. Normal compactions still only occur
    // when full.
    //
    // Set to -1 to disable metadata compaction during lfs_fs_gc.
    lfs_size_t compact_thresh;

    // Optional statically allocated read buffer. Must be cache_size.
    // By default lfs_malloc is used to allocate this buffer.
    void *read_buffer;

    // Optional statically allocated program buffer. Must be cache_size.
    // By default lfs_malloc is used to allocate this buffer.
    void *prog_buffer;

    // Optional statically allocated lookahead buffer. Must be lookahead_size.
    // By default lfs_malloc is used to allocate this buffer.
    void *lookahead_buffer;

    // Optional upper limit on length of file names in bytes. No downside for
    // larger names except the size of the info struct which is controlled by
    // the LFS_NAME_MAX define. Defaults to LFS_NAME_MAX or name_max stored on
    // disk when zero.
    lfs_size_t name_max;

    // Optional upper limit on files in bytes. No downside for larger files
    // but must be <= LFS_FILE_MAX. Defaults to LFS_FILE_MAX or file_max stored
    // on disk when zero.
    lfs_size_t file_max;

    // Optional upper limit on custom attributes in bytes. No downside for
    // larger attributes size but must be <= LFS_ATTR_MAX. Defaults to
    // LFS_ATTR_MAX or attr_max stored on disk when zero.
    lfs_size_t attr_max;

    // Optional upper limit on total space given to metadata pairs in bytes. On
    // devices with large blocks (e.g. 128kB) setting this to a low size (2-8kB)
    // can help bound the metadata compaction time. Must be <= block_size.
    // Defaults to block_size when zero.
    lfs_size_t metadata_max;

    // Optional upper limit on inlined files in bytes. Inlined files live in
    // metadata and decrease storage requirements, but may be limited to
    // improve metadata-related performance. Must be <= cache_size, <=
    // attr_max, and <= block_size/8. Defaults to the largest possible
    // inline_max when zero.
    //
    // Set to -1 to disable inlined files.
    lfs_size_t inline_max;

#ifdef LFS_MULTIVERSION
    // On-disk version to use when writing in the form of 16-bit major version
    // + 16-bit minor version. This limiting metadata to what is supported by
    // older minor versions. Note that some features will be lost. Defaults to 
    // to the most recent minor version when zero.
    uint32_t disk_version;
#endif
};

// File info structure
/**
 * stat/dir_read 的产出：类型（REG/DIR）+ 大小（仅 REG 有效）+ 名字。
 * 名字长度由 LFS_NAME_MAX 决定，缩减它可直接省 RAM（info 会变大）。
 */
struct lfs_info {
    // Type of the file, either LFS_TYPE_REG or LFS_TYPE_DIR
    uint8_t type;

    // Size of the file, only valid for REG files. Limited to 32-bits.
    lfs_size_t size;

    // Name of the file stored as a null-terminated string. Limited to
    // LFS_NAME_MAX+1, which can be changed by redefining LFS_NAME_MAX to
    // reduce RAM. LFS_NAME_MAX is stored in superblock and must be
    // respected by other littlefs drivers.
    char name[LFS_NAME_MAX+1];
};

// Filesystem info structure
/**
 * lfs_fs_stat 的产出：盘上格式版本 + 几何（块大小/数量）+ 三个上限。
 * 上限以"盘上 superblock 记录 ∩ 本机编译配置"为准。
 */
struct lfs_fsinfo {
    // On-disk version.
    uint32_t disk_version;

    // Size of a logical block in bytes.
    lfs_size_t block_size;

    // Number of logical blocks in filesystem.
    lfs_size_t block_count;

    // Upper limit on the length of file names in bytes.
    lfs_size_t name_max;

    // Upper limit on the size of files in bytes.
    lfs_size_t file_max;

    // Upper limit on the size of custom attributes in bytes.
    lfs_size_t attr_max;
};

// Custom attribute structure, used to describe custom attributes
// committed atomically during file writes.
/**
 * 自定义属性描述：8 位 type 标识 + 缓冲。随文件 sync/close 在同一次
 * 元数据提交里原子写盘；type 相同即覆盖，可当小型 xattr 用。
 */
struct lfs_attr {
    // 8-bit type of attribute, provided by user and used to
    // identify the attribute
    uint8_t type;

    // Pointer to buffer containing the attribute
    void *buffer;

    // Size of attribute in bytes, limited to LFS_ATTR_MAX
    lfs_size_t size;
};

// Optional configuration provided during lfs_file_opencfg
/**
 * per-file 配置：buffer 提供后该文件运行期零 malloc；attrs 列表
 * 声明后打开即读、sync 即写（保证属性与文件内容原子一致）。
 */
struct lfs_file_config {
    // Optional statically allocated file buffer. Must be cache_size.
    // By default lfs_malloc is used to allocate this buffer.
    void *buffer;

    // Optional list of custom attributes related to the file. If the file
    // is opened with read access, these attributes will be read from disk
    // during the open call. If the file is opened with write access, the
    // attributes will be written to disk every file sync or close. This
    // write occurs atomically with update to the file's contents.
    //
    // Custom attributes are uniquely identified by an 8-bit type and limited
    // to LFS_ATTR_MAX bytes. When read, if the stored attribute is smaller
    // than the buffer, it will be padded with zeros. If the stored attribute
    // is larger, then it will be silently truncated. If the attribute is not
    // found, it will be created implicitly.
    struct lfs_attr *attrs;

    // Number of custom attributes in the list
    lfs_size_t attr_count;
};


/// internal littlefs data structures ///
/**
 * 通用缓存描述：缓存哪个块、什么偏移起、多长、数据区。
 * littlefs 的读缓存/写缓存/每文件缓存都是这个形状。
 */
typedef struct lfs_cache {
    lfs_block_t block;
    lfs_off_t off;
    lfs_size_t size;
    uint8_t *buffer;
} lfs_cache_t;

/**
 * 元数据对的 RAM 镜像（全文件系统最核心的状态结构）：
 *   pair[2] —— 两块地址；约定 pair[0] 是最新提交所在块
 *   rev     —— 最新块的 revision count（磨损次数+新旧裁决）
 *   off     —— 日志尾偏移（下次 commit 的追加点）
 *   etag    —— 日志尾上一条 tag（XOR 链头，续写时接着异或）
 *   count   —— 本对内条目（文件）数
 *   erased  —— 日志尾是否仍处擦除态（决定能否直接追加）
 *   split   —— 是否有 hardtail（目录跨块分家过）
 *   tail[2] —— 下一对的指针（目录链/超块链用）
 */
typedef struct lfs_mdir {
    lfs_block_t pair[2];
    uint32_t rev;
    lfs_off_t off;
    uint32_t etag;
    uint16_t count;
    bool erased;
    bool split;
    lfs_block_t tail[2];
} lfs_mdir_t;

// littlefs directory type
/**
 * 打开的目录：mlist 节点（id/type/m）+ 迭代状态（pos/id/head）。
 * pos 0/1 是合成的 "."/".."；head 记住头块以便 rewind。
 */
typedef struct lfs_dir {
    struct lfs_dir *next;
    uint16_t id;
    uint8_t type;
    lfs_mdir_t m;

    lfs_off_t pos;
    lfs_block_t head[2];
} lfs_dir_t;

// littlefs file type
/**
 * 打开的文件：
 *   ctz.head/size —— CTZ 跳表尾块与文件大小（inline 文件 head=-2）
 *   flags         —— DIRTY(待提交)/WRITING(有未落盘写)/READING/
 *                    ERRED(写错误后禁提交)/INLINE(数据在元数据对里)
 *   block/off     —— 当前读写所在的块与偏移（缓存未命中时重定位）
 *   cache         —— 本文件的私有小缓存（cache_size 字节）
 * 写路径在 RAM 里推进 pos/cache，sync/close 时才一次性提交。
 */
typedef struct lfs_file {
    struct lfs_file *next;
    uint16_t id;
    uint8_t type;
    lfs_mdir_t m;

    struct lfs_ctz {
        lfs_block_t head;
        lfs_size_t size;
    } ctz;

    uint32_t flags;
    lfs_off_t pos;
    lfs_block_t block;
    lfs_off_t off;
    lfs_cache_t cache;

    const struct lfs_file_config *cfg;
} lfs_file_t;

/**
 * superblock 条目的 RAM 形态：格式化时写入，挂载时读出校验
 * （版本不兼容/几何不匹配都会拒绝挂载）。
 */
typedef struct lfs_superblock {
    uint32_t version;
    lfs_size_t block_size;
    lfs_size_t block_count;
    lfs_size_t name_max;
    lfs_size_t file_max;
    lfs_size_t attr_max;
} lfs_superblock_t;

/**
 * 全局状态（RAM 形态）：tag+pair 共 12 字节，delta 分布在盘上各
 * 元数据对的 MOVESTATE 里，XOR 聚合即得。三个实例分工：
 *   gstate —— RAM 中的当前值（可能尚未落盘）
 *   gdisk  —— 最近一次提交到盘上的值（两者之差=待提交 delta）
 *   gdelta —— 从其它对"偷"来的 delta（compact/drop 搬运时暂存）
 */
typedef struct lfs_gstate {
    uint32_t tag;
    lfs_block_t pair[2];
} lfs_gstate_t;

// The littlefs filesystem type
/**
 * 文件系统对象（用户分配，可多实例并存）——RAM 预算一览：
 *   rcache/pcache   2*cache_size，全局读写缓存
 *   lookahead       lookahead_size，空闲块位图窗口
 *   root[2]         根目录元数据对
 *   mlist           打开的 file/dir 链（节点在用户对象里，无额外 RAM）
 *   seed            各对 CRC 聚合出的伪随机数（分配起点用）
 *   gstate/gdisk/gdelta 3*12 字节全局状态
 * 除此之外没有任何随文件数增长的数据结构——RAM 严格有界。
 */
typedef struct lfs {
    lfs_cache_t rcache;
    lfs_cache_t pcache;

    lfs_block_t root[2];
    struct lfs_mlist {
        struct lfs_mlist *next;
        uint16_t id;
        uint8_t type;
        lfs_mdir_t m;
    } *mlist;
    uint32_t seed;

    lfs_gstate_t gstate;
    lfs_gstate_t gdisk;
    lfs_gstate_t gdelta;

    struct lfs_lookahead {
        lfs_block_t start;
        lfs_block_t size;
        lfs_block_t next;
        lfs_block_t ckpoint;
        uint8_t *buffer;
    } lookahead;

    const struct lfs_config *cfg;
    lfs_size_t block_count;
    lfs_size_t name_max;
    lfs_size_t file_max;
    lfs_size_t attr_max;
    lfs_size_t inline_max;

#ifdef LFS_MIGRATE
    struct lfs1 *lfs1;
#endif
} lfs_t;


/// Filesystem functions ///

#ifndef LFS_READONLY
// 格式化块设备。会 clobber lfs 对象且不保持挂载态；config 需清零。
// 错误：负错误码（含块设备回调透传）。
// Format a block device with the littlefs
//
// Requires a littlefs object and config struct. This clobbers the littlefs
// object, and does not leave the filesystem mounted. The config struct must
// be zeroed for defaults and backwards compatibility.
//
// Returns a negative error code on failure.
int lfs_format(lfs_t *lfs, const struct lfs_config *config);
#endif

// 挂载。lfs 与 config 必须在整个挂载期存活；可多实例并存。
// 错误：LFS_ERR_CORRUPT（找不到有效超块）等负错误码。
// 重入：LFS_THREADSAFE 时内部全程持锁。
// Mounts a littlefs
//
// Requires a littlefs object and config struct. Multiple filesystems
// may be mounted simultaneously with multiple littlefs objects. Both
// lfs and config must be allocated while mounted. The config struct must
// be zeroed for defaults and backwards compatibility.
//
// Returns a negative error code on failure.
int lfs_mount(lfs_t *lfs, const struct lfs_config *config);

// 卸载：仅释放资源（脏数据已在各 sync/close 提交）。
// Unmounts a littlefs
//
// Does nothing besides releasing any allocated resources.
// Returns a negative error code on failure.
int lfs_unmount(lfs_t *lfs);

/// General operations ///

#ifndef LFS_READONLY
// 删除；目录必须为空（LFS_ERR_NOTEMPTY）。原子操作，
// 掉电后由 deorphan 兜底，绝不产生半删除状态。
// Removes a file or directory
//
// If removing a directory, the directory must be empty.
// Returns a negative error code on failure.
int lfs_remove(lfs_t *lfs, const char *path);
#endif

#ifndef LFS_READONLY
// 重命名/移动。目标存在必须同类型（目录还须为空）；
// 跨目录移动由 gstate/move 机制保证原子（不丢不复制）。
// Rename or move a file or directory
//
// If the destination exists, it must match the source in type.
// If the destination is a directory, the directory must be empty.
//
// Returns a negative error code on failure.
int lfs_rename(lfs_t *lfs, const char *oldpath, const char *newpath);
#endif

// stat：填 type/size/name；不存在返回 LFS_ERR_NOENT。
// Find info about a file or directory
//
// Fills out the info structure, based on the specified file or directory.
// Returns a negative error code on failure.
int lfs_stat(lfs_t *lfs, const char *path, struct lfs_info *info);

// 读属性：返回盘上实际长度（可大于缓冲，超出部分被截断、
// 不足补 0）；属性不存在返回 LFS_ERR_NOATTR 且缓冲清零。
// Get a custom attribute
//
// Custom attributes are uniquely identified by an 8-bit type and limited
// to LFS_ATTR_MAX bytes. When read, if the stored attribute is smaller than
// the buffer, it will be padded with zeros. If the stored attribute is larger,
// then it will be silently truncated. If no attribute is found, the error
// LFS_ERR_NOATTR is returned and the buffer is filled with zeros.
//
// Returns the size of the attribute, or a negative error code on failure.
// Note, the returned size is the size of the attribute on disk, irrespective
// of the size of the buffer. This can be used to dynamically allocate a buffer
// or check for existence.
lfs_ssize_t lfs_getattr(lfs_t *lfs, const char *path,
        uint8_t type, void *buffer, lfs_size_t size);

#ifndef LFS_READONLY
// 写属性（不存在则隐式创建）；一次元数据提交内原子生效。
// Set custom attributes
//
// Custom attributes are uniquely identified by an 8-bit type and limited
// to LFS_ATTR_MAX bytes. If an attribute is not found, it will be
// implicitly created.
//
// Returns a negative error code on failure.
int lfs_setattr(lfs_t *lfs, const char *path,
        uint8_t type, const void *buffer, lfs_size_t size);
#endif

#ifndef LFS_READONLY
// 删属性（不存在则无操作，仍返回 0）。
// Removes a custom attribute
//
// If an attribute is not found, nothing happens.
//
// Returns a negative error code on failure.
int lfs_removeattr(lfs_t *lfs, const char *path, uint8_t type);
#endif


/// File operations ///

#ifndef LFS_NO_MALLOC
// 打开文件（缓冲走 malloc）。flags 组合见 enum lfs_open_flags。
// 错误：NOENT/EXIST/ISDIR/NAMETOOLONG/NOMEM 等。
// Open a file
//
// The mode that the file is opened in is determined by the flags, which
// are values from the enum lfs_open_flags that are bitwise-ored together.
//
// Returns a negative error code on failure.
int lfs_file_open(lfs_t *lfs, lfs_file_t *file,
        const char *path, int flags);

// if LFS_NO_MALLOC is defined, lfs_file_open() will fail with LFS_ERR_NOMEM
// thus use lfs_file_opencfg() with config.buffer set.
#endif

// 打开文件（带 per-file 配置：静态缓冲/自定义属性）。
// config 结构须在文件打开期间存活。
// Open a file with extra configuration
//
// The mode that the file is opened in is determined by the flags, which
// are values from the enum lfs_open_flags that are bitwise-ored together.
//
// The config struct provides additional config options per file as described
// above. The config struct must remain allocated while the file is open, and
// the config struct must be zeroed for defaults and backwards compatibility.
//
// Returns a negative error code on failure.
int lfs_file_opencfg(lfs_t *lfs, lfs_file_t *file,
        const char *path, int flags,
        const struct lfs_file_config *config);

// 关闭：隐式 sync 后释放资源。写后必须 close/sync 才落盘。
// Close a file
//
// Any pending writes are written out to storage as though
// sync had been called and releases any allocated resources.
//
// Returns a negative error code on failure.
int lfs_file_close(lfs_t *lfs, lfs_file_t *file);

// 手动提交：数据+元数据（含 attrs）一次原子落盘。
// Synchronize a file on storage
//
// Any pending writes are written out to storage.
// Returns a negative error code on failure.
int lfs_file_sync(lfs_t *lfs, lfs_file_t *file);

// 读：返回实际字节数（0=EOF）或负错误码。
// Read data from file
//
// Takes a buffer and size indicating where to store the read data.
// Returns the number of bytes read, or a negative error code on failure.
lfs_ssize_t lfs_file_read(lfs_t *lfs, lfs_file_t *file,
        void *buffer, lfs_size_t size);

#ifndef LFS_READONLY
// 写：返回写入字节数。close/sync 前仅存于 RAM（掉电丢未提交写入，
// 但已提交数据不受影响——这是"文件更新原子性"的边界）。
// Write data to file
//
// Takes a buffer and size indicating the data to write. The file will not
// actually be updated on the storage until either sync or close is called.
//
// Returns the number of bytes written, or a negative error code on failure.
lfs_ssize_t lfs_file_write(lfs_t *lfs, lfs_file_t *file,
        const void *buffer, lfs_size_t size);
#endif

// 定位：返回新位置或负错误码；whence 见 enum lfs_whence_flags。
// Change the position of the file
//
// The change in position is determined by the offset and whence flag.
// Returns the new position of the file, or a negative error code on failure.
lfs_soff_t lfs_file_seek(lfs_t *lfs, lfs_file_t *file,
        lfs_soff_t off, int whence);

#ifndef LFS_READONLY
// 截断：缩短丢尾（可退回 inline 省 RAM），扩展补 0。
// Truncates the size of the file to the specified size
//
// Returns a negative error code on failure.
int lfs_file_truncate(lfs_t *lfs, lfs_file_t *file, lfs_off_t size);
#endif

// 等价 seek(0, SEEK_CUR)。
// Return the position of the file
//
// Equivalent to lfs_file_seek(lfs, file, 0, LFS_SEEK_CUR)
// Returns the position of the file, or a negative error code on failure.
lfs_soff_t lfs_file_tell(lfs_t *lfs, lfs_file_t *file);

// 等价 seek(0, SEEK_SET)。
// Change the position of the file to the beginning of the file
//
// Equivalent to lfs_file_seek(lfs, file, 0, LFS_SEEK_SET)
// Returns a negative error code on failure.
int lfs_file_rewind(lfs_t *lfs, lfs_file_t *file);

// 文件大小；含未提交写入（WRITING 时取 max(pos, size)）。
// Return the size of the file
//
// Similar to lfs_file_seek(lfs, file, 0, LFS_SEEK_END)
// Returns the size of the file, or a negative error code on failure.
lfs_soff_t lfs_file_size(lfs_t *lfs, lfs_file_t *file);


/// Directory operations ///

#ifndef LFS_READONLY
// 建目录：父不存在 NOENT、重名 EXIST、超长 NAMETOOLONG。
// Create a directory
//
// Returns a negative error code on failure.
int lfs_mkdir(lfs_t *lfs, const char *path);
#endif

// 打开目录：后续用 lfs_dir_read 迭代（首两项是 "."/".."）。
// Open a directory
//
// Once open a directory can be used with read to iterate over files.
// Returns a negative error code on failure.
int lfs_dir_open(lfs_t *lfs, lfs_dir_t *dir, const char *path);

// 关闭目录并释放迭代状态。
// Close a directory
//
// Releases any allocated resources.
// Returns a negative error code on failure.
int lfs_dir_close(lfs_t *lfs, lfs_dir_t *dir);

// 迭代：1=info 有效；0=到尾；负=错误（注意与文件 read 的返回语义不同）。
// Read an entry in the directory
//
// Fills out the info structure, based on the specified file or directory.
// Returns a positive value on success, 0 at the end of directory,
// or a negative error code on failure.
int lfs_dir_read(lfs_t *lfs, lfs_dir_t *dir, struct lfs_info *info);

// 目录定位：off 必须来自 lfs_dir_tell，否则 INVAL。
// Change the position of the directory
//
// The new off must be a value previous returned from tell and specifies
// an absolute offset in the directory seek.
//
// Returns a negative error code on failure.
int lfs_dir_seek(lfs_t *lfs, lfs_dir_t *dir, lfs_off_t off);

// 返回迭代位置（仅用于 seek 回放，数值无业务含义）。
// Return the position of the directory
//
// The returned offset is only meant to be consumed by seek and may not make
// sense, but does indicate the current position in the directory iteration.
//
// Returns the position of the directory, or a negative error code on failure.
lfs_soff_t lfs_dir_tell(lfs_t *lfs, lfs_dir_t *dir);

// 迭代回到开头。
// Change the position of the directory to the beginning of the directory
//
// Returns a negative error code on failure.
int lfs_dir_rewind(lfs_t *lfs, lfs_dir_t *dir);


/// Filesystem-level filesystem operations

// 盘上信息：版本/块大小/块数/三个上限。
// Find on-disk info about the filesystem
//
// Fills out the fsinfo structure based on the filesystem found on-disk.
// Returns a negative error code on failure.
int lfs_fs_stat(lfs_t *lfs, struct lfs_fsinfo *fsinfo);

// 已用块数。共享 COW 块会重复计数 => 结果只会偏大（best effort）。
// Finds the current size of the filesystem
//
// Note: Result is best effort. If files share COW structures, the returned
// size may be larger than the filesystem actually is.
//
// Returns the number of allocated blocks, or a negative error code on failure.
lfs_ssize_t lfs_fs_size(lfs_t *lfs);

// 遍历全部在用块（回调可中断遍历：返回非 0 即停）。
// Traverse through all blocks in use by the filesystem
//
// The provided callback will be called with each block address that is
// currently in use by the filesystem. This can be used to determine which
// blocks are in use or how much of the storage is available.
//
// Returns a negative error code on failure.
int lfs_fs_traverse(lfs_t *lfs, int (*cb)(void*, lfs_block_t), void *data);

#ifndef LFS_READONLY
// 显式完成所有遗留中间态（move/孤儿/超块升级），
// 不必等到下一次写操作才隐式触发。
// Attempt to make the filesystem consistent and ready for writing
//
// Calling this function is not required, consistency will be implicitly
// enforced on the first operation that writes to the filesystem, but this
// function allows the work to be performed earlier and without other
// filesystem changes.
//
// Returns a negative error code on failure.
int lfs_fs_mkconsistent(lfs_t *lfs);
#endif

#ifndef LFS_READONLY
// 保洁三件事：mkconsistent → 压实超阈值元数据 → 预热分配器。
// 适合在空闲时段调用，把昂贵整理移出关键写路径。
// Attempt any janitorial work
//
// This currently:
// 1. Calls mkconsistent if not already consistent
// 2. Compacts metadata > compact_thresh
// 3. Populates the block allocator
//
// Though additional janitorial work may be added in the future.
//
// Calling this function is not required, but may allow the offloading of
// expensive janitorial work to a less time-critical code path.
//
// Returns a negative error code on failure. Accomplishing nothing is not
// an error.
int lfs_fs_gc(lfs_t *lfs);
#endif

#ifndef LFS_READONLY
// 扩容（重写超块 block_count）。收缩默认不支持
//（LFS_SHRINKNONRELOCATING 下越界块全空闲才允许）。
// Grows the filesystem to a new size, updating the superblock with the new
// block count.
//
// If LFS_SHRINKNONRELOCATING is defined, this function will also accept
// block_counts smaller than the current configuration, after checking
// that none of the blocks that are being removed are in use.
// Note that littlefs's pseudorandom block allocation means that
// this is very unlikely to work in the general case.
//
// Returns a negative error code on failure.
int lfs_fs_grow(lfs_t *lfs, lfs_size_t block_count);
#endif

#ifndef LFS_READONLY
#ifdef LFS_MIGRATE
// v1→v2 在线迁移（需 LFS_MIGRATE）。掉电安全：v1 数据只读不动，
// 失败重跑即可；成功后 v1 结构不可再挂载。
// Attempts to migrate a previous version of littlefs
//
// Behaves similarly to the lfs_format function. Attempts to mount
// the previous version of littlefs and update the filesystem so it can be
// mounted with the current version of littlefs.
//
// Requires a littlefs object and config struct. This clobbers the littlefs
// object, and does not leave the filesystem mounted. The config struct must
// be zeroed for defaults and backwards compatibility.
//
// Returns a negative error code on failure.
int lfs_migrate(lfs_t *lfs, const struct lfs_config *cfg);
#endif
#endif


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif

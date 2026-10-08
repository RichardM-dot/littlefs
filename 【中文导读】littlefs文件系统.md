# littlefs（轻量级掉电安全文件系统）学习导读

> 目录：`02-文件系统与存储/littlefs-轻量级掉电安全文件系统`
> 来源：https://github.com/littlefs-project/littlefs （压缩包：littlefs-master.zip）

## 这是什么

littlefs 是 ARM 官方开源的、专为 MCU 设计的**掉电安全（power-loss resilient）文件系统**：写入中途断电不会损坏已有数据，且天然抗 Flash 坏块。它已经在 ARM Mbed、Zephyr（`CONFIG_FILE_SYSTEM_LITTLEFS`）、NuttX 等几乎所有主流平台上被集成，是"往 SPI Flash / 片内 Flash 存文件"的事实标准。代码量极小（就 `lfs.c` 一个主文件，几千行），**非常适合通读源码**。

## 目录导读

| 文件/目录 | 内容 |
| --- | --- |
| `lfs.c` / `lfs.h` | 文件系统全部核心实现与 API（必读！） |
| `lfs_util.c` / `lfs_util.h` | CRC、调试工具函数 |
| `DESIGN.md` | **设计文档，先读它再读代码**，图文讲清元数据平衡树 |
| `SPEC.md` | 磁盘格式规范（二进制布局逐字段说明） |
| `bd/` | 块设备适配层（SPI Flash、SD 卡等） |
| `runners/` | linux/emubd 等运行器，可在 PC 上模拟 Flash |
| `scripts/` | 结构体定义检查、性能测试脚本 |

## 学习路径（建议）

1. 先通读 `DESIGN.md`（有中文社区翻译，搜"littlefs DESIGN 中文"）。
2. 在 Linux 上跑通最小例程（挂 `bd/littlefsbd` 或者官方 README 的 RAM 块设备），创建/读写一个文件。
3. 读 `lfs.c` 三大主线：`lfs_mount`（扫描挂载）→ `lfs_file_write`（元数据提交/双块轮换）→ `lfs_alloc`（磨损均衡）。
4. 做一个断电实验：写一半时 kill 进程，重新 mount 验证数据完好——亲眼看到掉电安全。

## 学习要点

1. **copy-on-write + 双元数据块**：每次提交写"另一个块"，提交成功才切换指针——这是掉电安全的全部秘密。
2. **与 FAT/JFFS2 对比**：FAT 断电易炸、JFFS2 内存开销大，littlefs 用极小 RAM 换取确定性安全。
3. **磨损均衡是"被动式"**：没有主动搬块，靠元数据轮转自然分散写入。
4. **移植只需实现两个回调**：`read`/`prog`/`erase`/`sync` 四个函数（在 `lfs_config` 里注册）。

## 建议购买/准备

- **零成本**：用 runners 里的模拟块设备在 Linux 上学习。
- **推荐硬件**：W25Q64 / W25Q128 SPI Flash 模块（¥3-8）+ 任意 STM32/ESP32 板；或直接买带 Flash 的开发板（树莓派 Pico 2MB Flash 就能玩）。

## 学习目标（自测）

- [ ] 能画出 littlefs 双元数据块切换的示意图
- [ ] 能说清一次 `lfs_file_write` 的完整提交过程
- [ ] 在 PC 模拟环境完成断电恢复实验
- [ ] 知道如何把它移植到一块 SPI Flash 上

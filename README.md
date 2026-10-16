# FwLoader

[![Release](https://img.shields.io/github/v/release/Hoopenfly/FwLoader?label=release)](https://github.com/Hoopenfly/FwLoader/releases)

嵌入式固件升级库（目前仅在 STM32L071CZT6 实机验证过）。

**库版本：v1.0.0**。同一版本号维护在 `library/inc/fw_msg.h` 的 `FW_LIB_VERSION`，发布时以本仓库的 Git tag 为准。

- 支持三种编译期选定的升级方式：覆写、交换、就地执行。组织方式参考 MCUboot，三种方式各占一个源文件，主流程只有一条，不做语句级分叉。
- 升级过程中需要持久化的数据（升级标志、交换进度、单页暂存）由库内的扇区级磨损均衡 KV（`fw_kv`）承载。这些数据在升级期间会被反复改写，且随时可能掉电，因此 `fw_kv` 采用追加式写入与先搬后擦的 GC，底层读写擦由调用方注入。
- `fw_kv` 本身是通用 KV，除升级元数据外也可承载应用参数，但库的定位是固件升级。
- 库内不访问芯片寄存器，也不使用平台头文件；板级地址与器件能力集中在 `library/inc/fw_cfg.h`，平台动作集中在 `library/src/fw_port.c`，器件驱动由使用方提供的平台后端实现。

---

## 目录结构

```
FwLoader/                              仓库根
    README.md
    LICENSE                            MIT
    .gitignore
    library/                           库本体
        inc/                           对外头文件
            fw_kv.h                    KV 存储内核
            fw_cfg.h                   全部配置：库内核参数、器件能力、板级布局
            fw_boot.h                  引导主流程对外接口
            fw_strategy.h              升级策略接口，三种模式同语义
            fw_iap.h                   串口升级协议
            fw_port.h                  平台适配层接口
            fw_msg.h                   对外日志
            fw_assert.h                库内断言
        src/                           源文件
            fw_kv.c                    KV 存储内核
            fw_boot.c                  引导主流程、KV 元数据读写、镜像校验、跳转
            fw_overwrite.c             模式 0：覆写
            fw_swap.c                  模式 1：交换
            fw_xip.c                   模式 2：就地执行
            fw_iap.c                   串口升级协议实现
            fw_port.c                  平台适配层实现
            fw_msg.c                   对外日志实现
            fw_cfg.c                   分区表实例化
    产线烧录工具/                       PC 端串口烧录工具，不属于库本身，见第十二节
        fw_download.ps1                烧录脚本
        烧录.bat                       批处理入口
        app.bin                        固件占位文件
        工具说明.md                    工具设计与协议说明
        使用说明.md                    操作说明
```

全部配置集中在 `library/inc/fw_cfg.h`（库内核参数 / 器件能力 / 板级布局三节）；平台实现（Flash与EEPROM读写擦、串口收发与取帧、调试输出、中断开关、跳转、复位原因、断言动作、临界区）放在 `library/src/fw_port.c`；库内其它文件不使用平台头文件与寄存器，只调用 `fw_port` 的抽象接口。平台后端与使用方工具不属于本库，由工程提供。

**头文件搜索路径**：源文件中的 `#include "fw_kv.h"` 等写法保持不变，头文件统一位于 `library/inc/`。构建时需要在工程中把该目录加入头文件搜索路径。Keil 中的位置为 `Options for Target → C/C++ → Include Paths`，加入 `library/inc/`。

**分层原则**：`fw_kv` 内核不引用芯片地址；`boot` 通过 `fw_kv` 与注入的 Flash 接口工作；平台相关代码集中于 `library/src/fw_port.c` 与工程提供的平台后端。

---

## 一、升级元数据存储：`fw_kv`

`fw_kv` 为升级流程提供元数据存储：升级标志、交换进度、交换暂存页。它同时是一个通用 KV，也可用于存放应用参数。

### 1.1 设计要点

- **数据组织**：分区 → 多个扇区 → 每个扇区由扇区头与连续追加的 KV 记录组成。记录允许跨页，但必须落在同一个扇区内。
- **追加式写入**：修改一个键不覆盖旧记录，而是追加一条新记录，旧记录成为垃圾，由 GC 在扇区轮转时回收。依据是存储器件特性：同一存储位在一个擦除周期内只能编程一次。该特性同时是磨损均衡的基础。
- **掉电安全**：GC 先把存活记录搬到另一个扇区并逐条回读校验，确认无误后才擦除原扇区。安全路径下任何一步掉电都不会丢失数据；无空扇区时的压缩回退路径需先把目标扇区存活记录搬到 RAM 再擦除回写，该窗口掉电会丢掉只存在于该扇区的键。扇区轮转时优先预留至少一个空扇区作为搬迁目标；分区内没有空扇区时，退化为把两个扇区的存活数据压缩进一个扇区。
- **擦除态自动适配**：`FW_ERASED_VALUE` 取 `0xFF`（多数 Flash，含 NOR/SPI 与多数片内 Flash，编程为清位）或 `0x00`（少数器件，如 STM32L0/L1 系列的片内 Flash/EEPROM，编程为置位）。扇区与节点状态值的方向据此自动取反，应用代码无需处理。

### 1.2 分区表

所有分区在 `library/inc/fw_cfg.h` 的 `FW_KV_PART_TABLE` 宏中登记，新增分区即新增一行：

```c
#define FW_KV_PART_TABLE                                                        \
{                                                                               \
  {"EEPROM",           EEPROM_BASE_ADDR, EEPROM_TOTAL_SIZE, EEPROM_SECTOR_SIZE, \
   FW_EEPROM_PAGE_SIZE, FW_KV_OPS(fw_port_eeprom_read, fw_port_eeprom_write, fw_port_eeprom_erase)}, \
  {FW_BOOT_STAGE_NAME, STAGE_ADDR,       STAGE_SIZE,        STAGE_SECTOR_SIZE,  \
   FW_FLASH_PAGE_SIZE,  FW_KV_OPS(fw_port_flash_read, fw_port_flash_write, fw_port_flash_erase)},    \
}
```

每行字段依次为：分区名、起始地址、分区大小（字节）、扇区大小（字节）、页大小（字节）、读写擦函数。

`FW_KV_OPS(读, 写, 擦)` 用复合字面量把三个函数名组合为 `fw_flash_ops_t`，因此同一张表可以挂接不同器件，例如片内 EEPROM 与主 Flash。

分区约束，由 `fw_kv_init` 检查：分区大小为扇区大小的整数倍；扇区大小为 2 的幂；扇区大小能被页大小整除；扇区数不少于 2 且不超过 `FW_KV_MAX_SECTORS`。

### 1.3 键值分配

键在 `library/inc/fw_cfg.h` 中以一个枚举集中编号。键的作用域是分区，不同分区可以使用相同编号。

| 键 | 值 | 所在分区 | 用途 |
|---|---|---|---|
| `FW_KV_KEY_BOOT_FLAG` | 1 | STAGE | 升级信令标志 |
| `FW_KV_KEY_BOOT_PROGRESS` | 2 | STAGE | 交换进度 / XIP 运行槽位 |
| `FW_KV_KEY_BOOT_SIZE` | 3 | STAGE | 固件实际大小，交换模式据此只交换有效页 |
| `FW_KV_KEY_BOOT_SCRATCH` | 4 | STAGE | 交换模式的单页暂存 |
| `FW_KV_KEY_CALIB` | 5 | EEPROM | 标定 / 传感器参数 |
| `FW_KV_KEY_BAUD` | 6 | EEPROM | 通信波特率 |
| 应用自定义键 | 由使用方编号 | 应用分区 | 与同分区内其它键不重复即可 |

### 1.4 API

```c
int  fw_kv_init(fw_kv_t *kv, const fw_kv_part_t *part);          //按分区描述初始化
int  fw_kv_init_by_name(fw_kv_t *kv, const char *name);          //按分区名初始化
const fw_kv_part_t *fw_kv_part_find(const char *name);           //按名查分区
int  fw_kv_get(fw_kv_t *kv, uint32_t key, uint8_t *value, uint32_t *len);
int  fw_kv_set(fw_kv_t *kv, uint32_t key, const uint8_t *value, uint32_t len);
int  fw_kv_del(fw_kv_t *kv, uint32_t key);
int  fw_kv_get_stats(fw_kv_t *kv, fw_kv_stats_t *stats);         //磨损与占用统计
void fw_kv_set_log(fw_kv_log_fn_t fn);                           //注册日志回调，可选
//日志回调签名：事件 + 实例 + 分区名 + a/b/c，见 library/inc/fw_kv.h
```

返回码：`FW_KV_OK=0`，`FW_KV_ERR_PARAM=-1`、`_FLASH=-2`、`_NOT_FOUND=-3`、`_NOSPC=-4`、`_BUF_SMALL=-5`、`_CRC=-6`。

调用约定：

- `fw_kv_get` 的 `*len` 为输入输出参数：传入缓冲区大小，返回实际长度。缓冲区不足时返回 `FW_KV_ERR_BUF_SMALL`，并把所需长度回填到 `*len`。
- 键 `0` 非法。
- 实例由调用方分配内存，可为全局或静态变量。
- 并发访问由平台层保护：每个接口内部是一串多步操作（读写实例状态，必要时触发扇区轮转与 GC），必须整体原子。保护方式由 `library/src/fw_port.c` 的 `fw_port_kv_crit_lock()` / `fw_port_kv_crit_unlock()` 提供（见[第六章](#六平台适配层fw_port)），内核只负责调用；无 OS 环境实现为关中断，有 OS 环境实现为互斥锁。使用方**不需要**在 `main()` 里注册任何回调。

用法示例：

```c
static fw_kv_t kv;

if(fw_kv_init_by_name(&kv, FW_BOOT_STAGE_NAME) != FW_KV_OK) { /* 处理失败 */ }

uint32_t flag = 0;
uint32_t len = sizeof(flag);
fw_kv_get(&kv, FW_KV_KEY_BOOT_FLAG, (uint8_t *)&flag, &len);
fw_kv_set(&kv, FW_KV_KEY_BOOT_FLAG, (const uint8_t *)&flag, sizeof(flag));
```

### 1.5 常驻 key 映射

每个 `fw_kv_t` 实例内驻留一张 `key → 最新记录位置` 的表（`key_map[]`、`key_count`、`key_map_ok`）：

- `fw_kv_init` 扫描全分区建表；
- `fw_kv_set` / `fw_kv_del` 追加记录后增量更新；
- GC 搬迁改变记录位置，结束后整表重建；
- `fw_kv_get` 先查表定位，命中后仍会读一次节点头校验，key 或魔数不符即判为不可信；校验不通过、表未建好或键数超过 `FW_KV_MAX_KEYS` 时自动回退全分区扫描。

映射只用于加速，正确性不依赖它：命中映射时按记录位置直接读取，无需遍历分区解析记录。

### 1.6 库内核参数（`library/inc/fw_cfg.h`）

| 宏 | 默认 | 说明 |
|---|---|---|
| `FW_ERASED_VALUE` | `0x00` | 擦除态，取值 `0x00` 或 `0xFF`，按器件设置 |
| `FW_ERASED_WORD` / `FW_ERASED_HALFWORD` | 由 `FW_ERASED_VALUE` 展开 | 擦除态的 32 位 / 16 位形式 |
| `FW_KV_GC_THRESHOLD` | `2` | 活动扇区剩余 ≤ 该页数（含等于）时提前触发 GC |
| `FW_KV_SECTOR_PAGES` | `32` | 扇区页数倍率，扇区大小 = 页大小 × 该值 |
| `FW_KV_GC_BUF_SIZE` | `2 × 页大小` | GC 搬迁缓冲 |
| `FW_KV_MAX_SECTORS` | `256` | 分区内最大扇区数 |
| `FW_KV_SECTOR_SIZE` | `页大小 × FW_KV_SECTOR_PAGES` | 扇区大小 |
| `FW_KV_MAX_SECTOR_SIZE` | `= FW_KV_SECTOR_SIZE` | 单个扇区的最大字节数 |

页大小 `FW_FLASH_PAGE_SIZE`（默认 `128`）与写粒度 `FW_FLASH_WRITE_BYTE` / `FW_EEPROM_PAGE_SIZE` / `FW_EEPROM_WRITE_BYTE` 属于"器件能力"，同样定义在 `library/inc/fw_cfg.h`，取值须与平台器件一致。

常驻映射容量 `FW_KV_MAX_KEYS`（默认 `8`，超出后回退全扫描）定义在 `library/inc/fw_kv.h`，不在本文件。

并发保护不由内核宏决定：临界区由平台层的 `fw_port_kv_crit_lock()` / `fw_port_kv_crit_unlock()` 提供，见[第六章](#六平台适配层fw_port)。

---

## 二、引导层：`boot`

### 2.1 Flash 布局

分区顺序固定，具体地址与大小由 `library/inc/fw_cfg.h` 定义；[第十一章](#十一示例配置)给出的数值仅用于说明结构，不代表本工程实际配置。

| 区域 | 位置 | 用途 |
|---|---|---|
| BootLoader | Flash 起始处 | 引导程序，不参与升级 |
| SLOT0 | BootLoader 之后 | 主应用镜像 |
| SLOT1 | SLOT0 之后 | 升级镜像与备份槽 |
| STAGE | SLOT1 之后 | `fw_kv` 分区：升级标志、进度、交换暂存 |
| 数据 EEPROM | 独立地址空间，可选 | `fw_kv` 分区：应用参数 |

### 2.2 三种升级方式

由 `library/inc/fw_boot.h` 的 `FW_BOOT_UPGRADE_MODE` 在编译期选定，与 MCUboot 的 overwrite-only、swap、direct-XIP 对应：

| 模式 | 文件 | 做法 | 回滚 |
|---|---|---|---|
| `0` 覆写 | `library/src/fw_overwrite.c` | 新镜像先收到 SLOT1，启动时整份覆写到 SLOT0，倒序写入 | 不支持 |
| `1` 交换 | `library/src/fw_swap.c` | SLOT0 与 SLOT1 逐页交换，用 STAGE 的 KV 做单页暂存与进度记录；**交换页数按镜像实际大小计算**，不必处理整个槽位 | 支持 |
| `2` 就地执行 | `library/src/fw_xip.c` | 两个槽位各自就地执行，只切换运行槽位与 confirm 位 | 支持 |

**覆写采用倒序写入**：从最后一页向第一页写，使向量表所在页最后落盘。中途断电时向量表仍为擦除态，`fw_boot_image_is_valid()` 判定无效，不会跳转到写了一半的镜像。配合 `CONFIRMED` 仅在覆写成功后写入，标志仍为 `VALID`，重启后重新执行升级。覆写始终覆写整个槽位，页数恒为槽位大小除以页大小（本工程 512 页），耗时只与槽位大小和器件编程速度有关，与实际镜像大小无关。

进度输出按已完成页数驱动、每 10% 打印一档，与写入顺序无关：模式 0 与模式 1 传的都是已完成页数（1..总页数），最后一档为 100%。模式 2 只切换运行槽位、不搬移镜像，因此不输出升级动作行。

每个策略文件由文件级 `#if` 包住，并要求 `FW_BOOT_BUILD_LOADER == 0`，一次构建只会编到其中一个；例外是模式 2 的两个槽位查询函数在 App 构建也需要编译，见 `library/src/fw_xip.c` 顶部条件。

### 2.3 启动流程

`fw_boot_handler()` 按固定步骤执行，内部没有模式分支：

```c
fw_port_rx_irq_disable();            //升级期间关闭串口接收中断，避免噪声累积引发中断风暴
if(fw_boot_stage_init() != 0) { fw_msg_error(FW_MSG_ERR_KV_INIT, 0); return; }

fw_strategy_state_read();            //阶段0：读升级标志与两个槽位的有效性
fw_msg_kv_parts();                   //阶段0：按分区表逐个打印统计，默认关闭
fw_strategy_recover();               //阶段0b：恢复上次被中断的升级，仅交换模式有实体
fw_strategy_rollback_flag();         //阶段1：处理强制回滚标志
fw_strategy_upgrade();               //阶段2：处理升级请求
fw_strategy_unconfirmed();           //阶段3：升级已执行但 App 未确认，自动回滚
fw_strategy_select_and_jump();       //阶段4：选择有效镜像跳转

fw_msg_error(FW_MSG_ERR_NO_IMAGE, 0);   //阶段5：两个槽位都没有有效镜像
fw_boot_flag_write(FW_BOOT_FLAG_NONE);
#if (FW_BOOT_IAP_ENABLE != 0)
  fw_port_rx_irq_enable();           //重新打开接收中断后返回，串口升级交给主循环
#endif
//两种情况都返回；无 IAP 时的周期提示由 main() 的 while(1) 负责，见 13.1
```

阶段 5 的两种行为：

- `FW_BOOT_IAP_ENABLE = 1`：停在串口升级模式等待上位机发送固件，收到完整固件后提交并输出提示，设备自身不复位，复位由现场完成（见 2.9），详见 [2.9](#29-无有效镜像时的串口升级fw_boot_iap_enable)。
- `FW_BOOT_IAP_ENABLE = 0`：同样直接返回，由 `main()` 的循环周期调用 `fw_msg_waiting()`，此时只能通过调试器重新烧录。

BootLoader 侧显式调用 `fw_boot_stage_init()`，目的是尽早发现初始化失败并停在错误状态。App 侧不需要调用，见 [2.8](#28-暂存区-kv-的惰性初始化)。

### 2.4 策略接口

三种模式各自实现同一组语义的函数，见 `library/inc/fw_strategy.h`，由 `library/src/fw_boot.c` 的主流程统一调用：

| 接口 | 语义 |
|---|---|
| `fw_strategy_state_read` | 读升级标志与两个槽位的有效性，XIP 另读运行槽位 |
| `fw_strategy_recover` | 恢复上次被中断的升级；修复完成且 SLOT0 可用时直接跳转，不返回 |
| `fw_strategy_rollback_flag` | 处理 `FW_BOOT_FLAG_ROLLBACK` |
| `fw_strategy_upgrade` | 处理 `FW_BOOT_FLAG_VALID`；成功后可直接跳转，不返回 |
| `fw_strategy_unconfirmed` | 处理升级已执行但 App 未确认的情况 |
| `fw_strategy_select_and_jump` | 选择有效镜像跳转；均无效则返回，由主流程报错 |
| `fw_strategy_perform` | 执行升级动作 |
| `fw_strategy_rollback` | 执行回滚动作，仅模式 1 与模式 2 使用 |

`fw_strategy_select_and_jump` 在模式 0 与模式 1 下实现相同，因此共用一份，位于 `library/src/fw_boot.c`，由 `MODE == 0 || MODE == 1` 条件包住。

### 2.5 升级标志与确认机制

标志值存放于 STAGE 分区的 `FW_KV_KEY_BOOT_FLAG`：

| 标志 | 值 | 含义 |
|---|---|---|
| `FW_BOOT_FLAG_NONE` | 擦除态 | 无操作 |
| `FW_BOOT_FLAG_VALID` | `0x5A5A5A5A` | 升级待执行 |
| `FW_BOOT_FLAG_ROLLBACK` | `0xA5A5A5A5` | 强制回滚 |
| `FW_BOOT_FLAG_CONFIRMED` | `0x01010101` | 升级已确认，App 正常启动 |

流程，模式 1 与模式 2：

1. 上位机把固件发到目标槽位（模式 1 为 SLOT1，模式 2 为当前运行槽位的对侧槽位），整包校验通过后写入 `FW_BOOT_FLAG_VALID`；
2. 复位后引导程序执行升级，交换或切换槽位，然后跳转新固件；
3. App 正常启动后调用 `fw_boot_set_confirmed()` 写入 `FW_BOOT_FLAG_CONFIRMED`；
4. App 启动失败时，例如看门狗复位，下次引导检测到标志为 `VALID` 且升级已完成但未确认，自动回滚到旧固件。

### 2.6 镜像有效性判定

`fw_boot_image_is_valid(addr)` 检查 Cortex-M 向量表，三个条件全部满足才判定有效：

- 地址在 Flash 范围内；
- 向量表第 0 项，即初始 MSP，落在 RAM 范围内；
- 向量表第 1 项，即复位向量，落在 Flash 范围内。

向量表地址需按 `VTOR_ALIGN_MASK` 对齐，但该对齐不在本判定内校验，而在跳转时校验：不满足则输出 `Err 0x05`、把标志清为 `NONE` 并停机。

### 2.7 对外接口（`library/inc/fw_boot.h`）

```c
int         fw_boot_stage_init(void);        //初始化STAGE分区的KV，可显式调用；不调用也会自动初始化
void        fw_boot_set_confirmed(void);     //App确认本轮升级有效
int         fw_boot_image_is_valid(uint32_t addr);

//以下仅引导构建参与编译
void fw_boot_handler(void);
void fw_boot_jump_to_app(uint32_t app_addr);

//标志区读写
uint32_t    fw_boot_flag_read(void);
int         fw_boot_flag_write(uint32_t flag);
uint32_t    fw_boot_progress_read(void);
int         fw_boot_progress_write(uint32_t value);
uint32_t    fw_boot_image_size_read(void);
int         fw_boot_image_size_write(uint32_t size);
uint32_t    fw_boot_kv_read_u32(uint32_t key, uint32_t default_val);
int         fw_boot_kv_write_u32(uint32_t key, uint32_t value);

//XIP槽位查询，仅模式2
uint32_t fw_xip_get_run_slot(void);
uint32_t fw_xip_is_pending_confirm(void);
```

### 2.8 暂存区 KV 的惰性初始化

`fw_boot_stage_init()` 用于初始化 STAGE 分区的 KV 实例 `fw_boot_kv`，调用方不需要显式调用它。`fw_boot_flag_read/write`、`fw_boot_progress_read/write`、`fw_boot_image_size_read/write` 内部都会先经过 `fw_boot_kv_ensure()`，首次访问时自动完成初始化。

| 调用方 | 做法 |
|---|---|
| BootLoader | `fw_boot_handler()` 显式调用一次，以便尽早发现初始化失败并停在错误状态 |
| App | 不需要调用；`fw_boot_set_confirmed()` 或 IAP 提交时的首次 KV 访问会自动初始化 |

状态保存在文件级变量 `boot_kv_ready` 中。`fw_boot_stage_init()` 会先将其清零再重新初始化，因此需要强制重新初始化时仍可显式调用。

设计目的：新增模块读写升级标志时若未先初始化，仍会走正常初始化路径，不会读写一个未初始化的实例。

### 2.9 无有效镜像时的串口升级（`FW_BOOT_IAP_ENABLE`）

两个槽位都没有有效镜像时，引导程序没有可跳转的目标。`FW_BOOT_IAP_ENABLE = 1` 时它不进入提示循环，而是成为升级接收端：

```
阶段5
  ├─ 重新打开接收中断，该中断在阶段0之前被关闭，然后返回
  ├─ 由主循环反复调用 fw_boot_iap_poll()：
  │     首次调用输出 Serial update: waiting for firmware
  │     之后循环：等待空闲中断 → 取整帧 → 按帧头分发，与 App 使用同一套 IAP 协议
  │           收到完整固件且整包 CRC 通过 → IAP 写入 FW_BOOT_FLAG_VALID
  │           输出 Serial update: firmware received, reset to upgrade
  │           无法识别的帧 → 忽略，不作应答
  └─ 复位由现场断电或复位键完成，设备自身不复位
        → 复位后执行正常流程：阶段2 覆写 SLOT1 到 SLOT0，倒序写入，然后跳转新固件
```

提交后不自动复位，也不接受复位命令。原因：设备一旦复位，上位机持有的串口句柄不再收到数据，后续覆写进度与启动信息全部丢失。因此 BootLoader 收完固件后只输出提示，不执行复位动作，复位由现场完成。

停在 IAP 模式不改变设备状态，重新执行一次烧录能够完成升级。

无法识别的帧不作应答，与 App 侧的处理一致。收发共用一条 RS485 半双工总线时，设备会收到自己发出的数据；若对未知帧也作应答，就会形成自发自收的闭环，把串口与 CPU 占满。上位机对无应答的帧按停等协议重发，因此忽略比应答更安全。

要点：

- 固件写入哪个槽位由升级模式决定：模式 0/1 固定写入 SLOT1，模式 2（XIP）写入当前运行槽位的对侧槽位，与 App 侧升级使用同一套协议；
- 判断帧边界需要空闲中断：一帧最长 324 字节，收齐需要 20ms 以上，边收边取会把半帧当作整帧解析，导致后续字节错位。引导构建用平台后端的空闲中断事件接口按帧取数据，见 `library/src/fw_port.c` 的 `fw_port_read_frame()`；App 构建改由中断通知接收任务；
- 等待期间不做周期输出，避免与 IAP 应答帧抢占串口，干扰上位机解析；
- 该宏只影响引导构建，App 构建始终包含 IAP；
- 轮询函数为 `library/src/fw_boot.c` 的 `fw_boot_iap_poll()`，由主循环调用。它只调用 `library/src/fw_port.c` 提供的取帧与发送抽象动作，不直接访问串口寄存器。

---

## 三、串口升级：`iap`

`library/src/fw_iap.c` 与 `library/inc/fw_iap.h` 整体由 `#if (FW_BOOT_BUILD_LOADER == 1) || (FW_BOOT_IAP_ENABLE != 0)` 包住：App 构建始终编译；引导构建由 `FW_BOOT_IAP_ENABLE` 决定，用于两个槽位都无效时接收固件，见 [2.9](#29-无有效镜像时的串口升级fw_boot_iap_enable)。

支持两套上位机协议，由接入层按帧头分发：

| 协议 | 帧头 | 特点 |
|---|---|---|
| 文本 `$UP` | `$UP` / `$UD` | 24 字节文本帧头加数据，数据部分每帧不超过 300 字节（整帧最长 324 字节），带整包 CRC16 |
| HEX 二进制 | `0x55 0xAA` | `SOF(2) VER_TYPE(1) OFFSET(4) LEN(2) DATA CRC16(2)`，每帧数据不超过 256 字节 |

接口：

```c
void fw_iap_parse_text_frame(uint8_t *frame, uint32_t len);   //文本：整帧输入
void fw_iap_parse_hex_frame(uint8_t *buf, uint32_t len);      //HEX：字节流就地解析
iap_state_t fw_iap_get_status(uint32_t *total_size, uint32_t *received_size, uint8_t *percent);
```

接收链路：接入层在串口空闲中断置位后取走整帧，按帧头识别协议，再调用上面两个解析函数之一。

固件写入哪个槽位由升级模式决定：模式 0/1 写入 SLOT1，模式 2（XIP）写入当前运行槽位的对侧槽位；整包校验通过后调用 `fw_boot_flag_write(FW_BOOT_FLAG_VALID)` 提交，实际升级由复位后的引导程序完成。

会话规则如下。两套协议共用同一份会话状态结构，其中 OFFSET 衔接校验与重传判定只由 HEX 协议实现；文本 `$UP` 协议不校验 OFFSET 衔接，靠应答里的已收长度与英文提示让上位机自行重传：

- 数据帧的 OFFSET 必须与已写入长度严格衔接，不衔接时返回 NAK 并携带期望位置，以此拒绝丢帧、乱序与粘帧；
- 数据帧的帧尾恰好落在写入前沿时判为重传，仅重发 ACK；
- 结束帧在会话已结束后重复到达时返回幂等 ACK；
- 写入失败、整包 CRC 不符或镜像头无效时作废本次会话，要求上位机从偏移 0 整包重传；文本协议只在镜像超长与镜像头无效时作废会话，写入失败时保留会话状态，由上位机重发该帧；
- 版本号不符的帧返回状态码为 `BAD_FRAME` 的 NAK；
- 长度字段超过单帧上限的帧由解析层丢弃并重新查找 SOF，不返回 NAK；上位机不应发送的 ACK/NAK 帧直接忽略。

---

## 四、日志：`fw_msg`

- **文案集中**在 `library/src/fw_msg.c`，其它模块只调用函数，不自行拼接字符串；
- 前缀 `[APP] ` 与 `[BL]  `（引导前缀补两个空格，凑齐 6 字符宽）由 `FW_BOOT_BUILD_LOADER` 决定，两种构建共用同一份实现；
- 输出为全 ASCII，行格式为 `<前缀><键=值 ...>`，便于上位机抓取；
- 每一项有独立开关，`FW_MSG_ENABLE=0` 时全部关闭，函数体与字符串都不参与编译，调用方无需条件编译；
- **输出经平台层**：`library/src/fw_msg.c` 调用 `fw_port_log_printf()`，不直接调用平台调试输出函数。单行上限为 `FW_PORT_LOG_LINE_MAX`，默认 64 字节，含行尾 `\r\n`；超长由平台层截断，并在截断处补 `\r\n`，保证上位机按行切分；
- **复位原因只取一次**：`fw_msg_reset_reason()` 缓存 `fw_port_reset_reason_get()` 的结果，平台层读取后清标志的副作用仅发生一次，重复输出不会变为 `UNKNOWN`；
- **擦写期间只做非阻塞输出**：KV 的 GC、轮转与错误事件可能在 Flash 擦写期间回调进入，平台层单次输出须设等待上限，见 [六、平台适配层](#六平台适配层fw_port)；
- **项目专有字段不入库**：库内只提供升级流程通用文案。App 就绪一类日志中的波特率、标定点数等字段由调用方以格式串传入，例如 `fw_msg_app_ready("baud=%u cal=%u", baud, cal)`，传入 `NULL` 时只输出 `Ready`；需要其它内容时可直接调用 `fw_port_log_printf()`。

`fw_kv` 通过 `fw_kv_set_log()` 注册回调输出日志，库本身不依赖 `fw_msg`。适配器位于 `library/src/fw_msg.c` 的 `fw_msg_kv_log_attach()`，由 `main()` 调用一次；分区名由回调直接给出，日志模块无需访问 KV 实例字段。

---

## 五、断言：`fw_assert`

`library/inc/fw_assert.h` 提供运行时断言，用于检查库内部不变量。

| 宏 | 说明 |
|---|---|
| `FW_ASSERT(cond)` | 条件为假时调用 `fw_assert_fail()` |
| `FW_ASSERT_EVAL(expr, cond)` | 条件含副作用时使用，第一个参数在开关关闭时仍被求值 |
| `FW_STATIC_ASSERT(cond, msg)` | 编译期断言，要求 C11 |

使用范围：

- 仅用于库内部推导出的条件，例如扇区索引边界、记录是否越过分区边界、映射表下标；
- 外部输入错误，例如串口帧、Flash 返回码、分区表内容，返回错误码，不使用断言；
- `FW_ASSERT_ENABLE=0` 时条件与字符串都不参与编译，调用方无需条件编译。

失败动作由平台层的 `fw_assert_fail()` 实现。默认实现输出一行 `[ASSERT] 文件:行号 条件`，等待串口发送完成后主动复位。该行为不依赖看门狗喂狗位置与调试器连接状态。需要改为停机等待调试器时，把 `NVIC_SystemReset()` 替换为死循环。

---

## 六、平台适配层：`fw_port`

`library/inc/fw_port.h` 定义库与平台之间的全部接口，`library/inc/fw_cfg.h` 声明平台能力。移植时改这两个文件：在 `fw_cfg.h` 填本平台取值，按 `fw_port.h` 重写 `library/src/fw_port.c`。

| 类别 | 接口 |
|---|---|
| 串口动作 | `fw_port_rx_irq_disable()`、`fw_port_rx_irq_enable()`、`fw_port_read_frame()`、`fw_port_send_buf()`、`fw_port_wait_tx_complete()` |
| 日志出口 | `fw_port_log_printf()`、`fw_port_log_vprintf()` |
| 复位原因 | `fw_port_reset_reason_get()` |
| 断言动作 | `fw_assert_fail()`，声明在 `library/inc/fw_assert.h` |
| 存储原语 | `fw_port_flash_read()`、`fw_port_flash_write()`、`fw_port_flash_erase()`、`fw_port_eeprom_read()`、`fw_port_eeprom_write()`、`fw_port_eeprom_erase()` |
| 中断开关 | `fw_port_irq_disable()`、`fw_port_irq_enable()` |
| 跳转 | `fw_port_jump_to_app()` |

调用约定：

1. `fw_port_log_printf()` 与 `fw_port_log_vprintf()` 不受库内开关影响，始终可用，对外日志依赖它输出升级流程；平台后端自身的调试开关（如本平台的 `DBG_ENABLE`）不得在本层之上把输出编译掉；
2. 单次调用输出完整一行，实现须保证整行原子，不被其它任务或中断的输出插入；至少支持 `%s`、`%u`、`%d`、`%X`、`%c`、`%%` 及宽度，例如 `%08X`；
3. 单行长度不超过 `FW_PORT_LOG_LINE_MAX` 字节，含 `\r\n`，超长由实现截断；
4. 输出不得无限阻塞：升级流程会在 Flash 擦写前后输出，单次等待须设上限，建议 10ms 以内；
5. `fw_port_reset_reason_get()` 读取后清除复位标志，一次上电仅调用一次，多次使用需要在更上层缓存，见 `fw_msg_reset_reason()`；
6. `fw_port_kv_crit_lock()` / `fw_port_kv_crit_unlock()` 供 `fw_kv` 做临界区，成对使用；有 OS 环境用互斥锁而不是关中断，因为临界区可能包含 Flash 擦写，`fw_port_kv_crit_init()` 负责创建互斥锁且可重复调用；
7. 库内不做喂狗，喂狗由平台负责：平台后端提供的读/写/擦函数须在内部调用喂狗，覆盖单次擦写耗时；工程主循环的喂狗直接调用平台自己的看门狗接口。注意擦写期间 CPU 无法从 Flash 取指，此时不能调用位于 Flash 中的 HAL 喂狗函数，须直接刷新看门狗寄存器。

平台相关代码只允许出现在 `library/src/fw_port.c` 中。库内其它文件（IAP、三种升级策略、引导主流程、日志）一律只调用上表中的抽象动作：不引用平台头文件、不访问寄存器、不使用平台宏；页大小与写粒度取自 `fw_cfg.h`。

---

## 七、编译开关总表

| 开关 | 位置 | 取值 |
|---|---|---|
| `FW_BOOT_UPGRADE_MODE` | `library/inc/fw_boot.h` | `0` 覆写 / `1` 交换 / `2` 就地执行 |
| `FW_BOOT_IAP_ENABLE` | `library/inc/fw_boot.h` | `1` 无有效镜像时支持串口升级 / `0` 关闭 |
| `FW_BOOT_BUILD_LOADER` | 工程选项；`library/inc/fw_boot.h` 内 `#ifndef` 兜底为 `1` | `0` 引导构建 / `1` 应用构建 |
| `configUSE_FREERTOS` | 工程选项；仅 `library/src/fw_port.c` 内兜底 | `1` 时平台层临界区用 FreeRTOS 互斥锁，`0` 时用关中断 |
| `FW_ERASED_VALUE` | `library/inc/fw_cfg.h` | `0x00` / `0xFF` |
| `FW_KV_*` | `library/inc/fw_cfg.h` | GC 阈值、扇区页数、缓冲、扇区大小 |
| `FW_MSG_ENABLE` / `FW_MSG_EN_*` | `library/inc/fw_msg.h` | 日志总开关与分项开关 |
| `FW_LIB_VERSION` | `library/inc/fw_msg.h` | 库自身版本，由库维护者随发布更新，不使用 `#ifndef`；不参与打印 |
| `FW_ASSERT_ENABLE` | `library/inc/fw_assert.h` | `1` 保留断言检查 / `0` 全部编译掉 |
| `FW_FLASH_PAGE_SIZE` / `FW_FLASH_WRITE_BYTE` / `FW_EEPROM_PAGE_SIZE` / `FW_EEPROM_WRITE_BYTE` / `FW_PORT_RX_BUF_SIZE` | `library/inc/fw_cfg.h`（平台填写） | 平台器件能力，库据此切分擦写并校验串口缓冲 |
| `FW_PORT_LOG_LINE_MAX` | `library/inc/fw_port.h` | 一行日志最大字节数，默认 64，含 `\r\n` |
| `VTOR_ALIGN_MASK` | `library/inc/fw_boot.h` | 向量表对齐掩码 |

---

## 八、移植到别的平台要改什么

1. **平台层**：在 `library/inc/fw_cfg.h` 填本平台的页大小、写粒度与接收缓冲；按 `library/inc/fw_port.h` 重写 `library/src/fw_port.c`，包含串口动作、存储原语、中断开关、跳转、`fw_port_log_printf()`、`fw_port_reset_reason_get()`、`fw_assert_fail()` 与三个临界区函数；
2. **平台后端**：实现 Flash 与 EEPROM 的读写擦原语（签名对齐 `fw_port.h`，返回 0 表示成功），并在 `fw_port.c` 中转发给器件驱动；**每个原语内部都要喂狗**，库不承担喂狗职责；
3. **分区表**：在 `library/inc/fw_cfg.h` 的 `FW_KV_PART_TABLE` 中修改地址、大小、扇区与页，指向新的后端；
4. **擦除态**：把 `FW_ERASED_VALUE` 设为 `0x00` 或 `0xFF`；
5. **页大小**：把 `FW_FLASH_PAGE_SIZE` 设为新器件的页大小，写粒度由 `FW_FLASH_WRITE_BYTE` 给出（`FLASH_PAGE_SIZE` 由平台 HAL 提供，库内不定义）；
6. **并发保护**：在 `library/src/fw_port.c` 中实现 `fw_port_kv_crit_init()` / `fw_port_kv_crit_lock()` / `fw_port_kv_crit_unlock()`——无 OS 环境实现为关中断，有 OS 环境实现为互斥锁；库自带的实现按 `configUSE_FREERTOS` 在两者间切换，`main()` 不需要做任何注册。

`library/src/fw_kv.c`、`library/inc/fw_kv.h` 与引导逻辑无需修改。

**新增一个键**：在 `library/inc/fw_cfg.h` 的 `fw_kv_key_t` 枚举中加一项，然后用 `fw_kv_get/set` 读写。
**新增一个分区**：在 `FW_KV_PART_TABLE` 中加一行，然后用 `fw_kv_init_by_name()` 初始化。

---

## 九、两种构建的关系

同一份库可由两种构建复用：引导构建与应用构建。构建差异由 `FW_BOOT_BUILD_LOADER` 决定，源码本身没有分叉。

| 宏 | 引导构建 | 应用构建 | 作用 |
|---|---|---|---|
| `FW_BOOT_BUILD_LOADER` | `0` | `1` | 区分引导与应用，决定哪些函数参与编译以及日志前缀 |

`FW_BOOT_BUILD_LOADER` 由工程选项传入，源码中只有 `#ifndef` 兜底默认值。Keil 中的位置为 `Options for Target → C/C++ → Preprocessor Symbols → Define`：

```
FW_BOOT_BUILD_LOADER=0     // 引导构建
FW_BOOT_BUILD_LOADER=1     // 应用构建
```

两个工程的 `Include Paths` 都需要包含库的 `library/inc/` 目录。

**默认值与风险**：`FW_BOOT_BUILD_LOADER` 在 `library/inc/fw_boot.h` 中默认 `1`，有 `#ifndef` 保护。未传入时不会产生编译错误，但构建配置与预期不符，新建工程时需要显式传入。

构建差异，全部由 `FW_BOOT_BUILD_LOADER` 决定：

- `fw_boot_handler()` 与 `fw_boot_jump_to_app()` 仅在 `FW_BOOT_BUILD_LOADER == 0` 时编译；
- 三个升级策略文件 `library/src/fw_overwrite.c`、`library/src/fw_swap.c`、`library/src/fw_xip.c` 仅在 `FW_BOOT_BUILD_LOADER == 0` 时编译；
- `library/src/fw_iap.c` 在 `FW_BOOT_BUILD_LOADER == 1` 时始终编译。

---

## 十、已知限制与注意事项

1. **BootLoader 自身不能被升级**。它一旦损坏，例如被误擦除，只能通过调试器重新烧录。要支持掉电安全地更新引导程序，需要不可变引导选择器加双引导副本的结构，当前分区布局未预留该空间，需要重新分区。本库不提供该能力。
2. **模式 0 覆写不支持回滚**：该请求被丢弃，标志被改写为 `FW_BOOT_FLAG_CONFIRMED`，随后按正常流程选择镜像。
3. **交换模式的擦写量**：每交换一页会向 STAGE 分区追加一条暂存记录，长度等于节点头加页号加页数据加 CRC。以 128 字节页、4096 字节扇区为例：一条暂存记录 152 字节，另有每页一条 24 字节的进度记录，故每页合计约 176 字节。页数按镜像实际大小计算（见 2.2），一个占满 64KB 槽位的镜像对应该槽位的 512 页、约 88KB；按扇区大小折算会触发十几次到二十几次扇区轮转与 GC。分区容量与升级耗时须按此核算。
4. **常驻映射不是正确性的前提**：映射失效时自动回退全分区扫描，仅性能下降。键数超过 `FW_KV_MAX_KEYS` 时持续使用慢路径。
5. **单次升级的镜像大小上限**为一个槽位，由 IAP 在起始帧与逐帧边界处检查。

---

## 十一、示例配置

本节为示例，用于说明板级配置的结构，数值仅用于演示，与任何实际产品配置无关。假定器件为 256KB 主 Flash 加 8KB 数据 EEPROM，主 Flash 页 128 字节，EEPROM 页 4 字节。

### 11.1 地址与分区表

```c
/* 主 Flash */
#define FLASH_BASE_ADDR     0x08000000
#define FLASH_TOTAL_SIZE    (256 * 1024)
#define FW_FLASH_PAGE_SIZE  128

/* 数据 EEPROM，基址由器件决定 */
#define EEPROM_NAME         "EEPROM"
#define EEPROM_BASE_ADDR    0x08080000
#define EEPROM_TOTAL_SIZE   (8 * 1024)
#define FW_EEPROM_PAGE_SIZE 4
#define EEPROM_SECTOR_SIZE  256

/* 引导区与两个槽位 */
#define BOOT_SIZE           (16 * 1024)
#define SLOT_SIZE           (96 * 1024)
#define BOOT_ADDR           FLASH_BASE_ADDR
#define SLOT0_ADDR          (BOOT_ADDR + BOOT_SIZE)
#define SLOT1_ADDR          (SLOT0_ADDR + SLOT_SIZE)

/* 暂存区，紧随 SLOT1 */
#define STAGE_SIZE          (48 * 1024)
#define STAGE_ADDR          (SLOT1_ADDR + SLOT_SIZE)
#define STAGE_SECTOR_SIZE   4096

/* 两行分区表：数据 EEPROM 与主 Flash 暂存区 */
#define FW_KV_PART_TABLE                                                        \
{                                                                               \
  {EEPROM_NAME,        EEPROM_BASE_ADDR, EEPROM_TOTAL_SIZE, EEPROM_SECTOR_SIZE, \
   FW_EEPROM_PAGE_SIZE, FW_KV_OPS(fw_port_eeprom_read, fw_port_eeprom_write, fw_port_eeprom_erase)}, \
  {FW_BOOT_STAGE_NAME, STAGE_ADDR,       STAGE_SIZE,        STAGE_SECTOR_SIZE,  \
   FW_FLASH_PAGE_SIZE,  FW_KV_OPS(fw_port_flash_read, fw_port_flash_write, fw_port_flash_erase)},    \
}
```

两行分别对应两种器件：数据 EEPROM 使用 4 字节页与 256 字节扇区，主 Flash 暂存区使用 128 字节页与 4096 字节扇区。分区表由 `library/src/fw_cfg.c` 实例化。

### 11.2 键值编号

```c
typedef enum
{
  FW_KV_KEY_BOOT_FLAG     = 0x01,   /* STAGE：升级信令标志 */
  FW_KV_KEY_BOOT_PROGRESS = 0x02,   /* STAGE：交换进度或 XIP 运行槽位 */
  FW_KV_KEY_BOOT_SIZE     = 0x03,   /* STAGE：固件实际大小 */
  FW_KV_KEY_BOOT_SCRATCH  = 0x04,   /* STAGE：交换模式单页暂存 */
  FW_KV_KEY_APP_PARAM     = 0x10    /* 示例：应用自定义键，编号由使用方决定 */
}fw_kv_key_t;
```

前四项由引导逻辑使用，编号固定。应用自定义键的编号由使用方决定，只需保证与同分区内的其它键不重复；示例中的 `0x10` 仅为取值示范。

### 11.3 三种升级模式的布局差异

| 模式 | 需要的区域 | 布局要求 |
|---|---|---|
| `0` 覆写 | BootLoader、SLOT0、SLOT1、STAGE | 四段连续，SLOT1 只作为接收缓冲 |
| `1` 交换 | BootLoader、SLOT0、SLOT1、STAGE | 四段连续，SLOT0 与 SLOT1 大小相同 |
| `2` 就地执行 | BootLoader、SLOT0、SLOT1、STAGE | 两个槽位需能各自就地执行：双 Bank 器件，或把两份固件编译到不同的 IROM 地址 |

模式 0 与模式 1 的差别在于是否需要回滚能力；模式 2 不搬运镜像，只切换运行槽位与 confirm 位。

以上参数均属板级配置。更换芯片时仅需修改 `library/inc/fw_cfg.h` 与本节的地址宏，库内核与引导逻辑无需改动。

---

## 十二、产线烧录工具

`产线烧录工具/` 是随库提供的 PC 端串口烧录工具，由 PowerShell 脚本实现，批处理文件作为双击入口。它不属于库本身，不参与库的构建。

作用：把待烧录的应用固件通过串口发送给设备，由设备端的 IAP 接收并写入升级槽；设备复位后由引导程序完成实际升级。

适用场景：

- 批量烧录：操作人员仅需一根 USB 转串口线与固件文件，不需要编译环境，也不需要调试器；
- 现场升级：设备已在运行应用时，使用同一套工具与同一套协议完成升级。

工具与库之间的接口是 IAP 协议，即 `library/src/fw_iap.c` 实现的文本 `$UP` 协议。帧格式、CRC16 初值与多项式必须与设备端一致，细节见 `产线烧录工具/工具说明.md`。

工具目录中的 `app.bin` 是占位文件，用于说明固件的放置位置与命名；实际烧录时替换为编译产出的固件。

---

## 十三、在工程里的接入骨架

前面各节讲库的设计与接口，本节给出两种构建下 `main()` 的接入顺序。示例只保留与本库相关的调用，平台初始化部分用注释代替。

### 13.1 引导构建

```c
int main(void)
{
    /* ① 平台初始化：时钟、外设、串口、延时；临界区在 fw_port.c 里，这里不用管 */
    ...

    fw_msg_kv_log_attach();   /* ② 可选：把 fw_kv 的事件(GC/轮转/错误)接到 fw_msg 的输出上 */

    fw_msg_banner(fw_version);   /* ③ 版本横幅，版本串由调用方传入 */
    fw_boot_handler();        /* ④ 升级主流程；跳转成功时不返回 */
    fw_msg_waiting();         /* ⑤ 走到这里表示没有可跳转的镜像 */

    while(1)
    {
#if (FW_BOOT_IAP_ENABLE != 0)
        fw_boot_iap_poll();   /* ⑥ 串口升级轮询，非阻塞 */
#endif
        平台喂狗函数();          /* ⑦ 喂狗：直接调用平台自己的接口 */
        /* 其它空闲处理 */
    }
}
```

每一步的作用：

| 步骤 | 调用 | 作用 |
|---|---|---|
| ① | 平台初始化 | 时钟、GPIO、串口、延时。必须排在 ③ 之前，否则日志出口尚未可用 |
| ② | `fw_msg_kv_log_attach()` | 可选。把 `fw_kv` 的 GC、扇区轮转、错误事件接到日志输出。不调用只是少了这几类日志，存储功能不受影响 |
| ③ | `fw_msg_banner(fw_version)` | 输出一行版本横幅（前缀、调用方传入的版本串、编译日期），用于确认设备当前运行的是哪份固件。传 `NULL` 则不打版本串 |
| ④ | `fw_boot_handler()` | 引导主流程，按阶段依次做完：读升级标志与两个槽位的有效性 → 恢复上次被中断的升级 → 处理强制回滚 → 按升级模式执行升级 → 选择有效镜像并**跳转**。跳转成功时**不返回**；两个槽位都没有有效镜像时返回 |
| ⑤ | `fw_msg_waiting()` | 只在 ④ 返回后才执行，输出"需要重新烧录 / 等待串口升级"提示。不要挪到 ④ 之前，否则正常启动的设备也会打印它 |
| ⑥ | `fw_boot_iap_poll()` | 仅引导构建且开启 IAP 时参与编译。**非阻塞**：取一帧 → 按帧头分发 → 写入目标槽位 → 整包校验通过后写 `VALID` 标志。由主循环反复调用，不能在里面阻塞等待整包 |
| ⑦ | 平台喂狗 | 直接调用平台的看门狗刷新接口。库内不喂狗，喂狗由平台后端的擦写原语与使用方负责 |

### 13.2 应用构建

```c
int main(void)
{
    /* ① 平台初始化：时钟、外设、看门狗、串口、延时
     * 临界区由 fw_port.c 提供：有 OS 时用互斥锁，首次加锁时自动创建 */
    ...

    fw_msg_kv_log_attach();   /* ② 可选：KV 事件日志 */
    fw_msg_banner(fw_version);   /* ③ 版本横幅，版本串由调用方传入 */

    xTaskCreate(Start_Task, ...);   /* ④ 应用主逻辑交给任务 */
    vTaskStartScheduler();
    while(1) { }
}

/* 启动任务：应用自身初始化完成后确认升级 */
void Start_Task(void *parameter)
{
    /* ⑤ 创建其它任务与定时器，完成应用初始化 */
    ...

    /* ⑥ 确认本次升级有效；不调用会在下次复位时被引导程序回滚（仅模式 1/2；模式 0 不支持回滚） */
    fw_boot_set_confirmed();

    /* ⑦ 就绪日志：项目专有字段由调用方以格式串传入 */
    fw_msg_app_ready("baud=%u cal=%u", (unsigned)baud, (unsigned)cal_count);

    vTaskDelete(NULL);
}
```

每一步的作用：

| 步骤 | 调用 | 作用 |
|---|---|---|
| ① | 平台初始化 | 时钟、外设、看门狗、串口、延时。临界区实现放在 `library/src/fw_port.c`，`main()` 不需要注册任何回调 |
| ② | `fw_msg_kv_log_attach()` | 可选，同 13.1 |
| ③ | `fw_msg_banner(fw_version)` | 输出应用侧版本横幅，版本串由调用方传入 |
| ④ | 创建任务 + 启动调度器 | 应用主逻辑与串口接收都交给任务；本库自身不创建任务 |
| ⑤ | 应用初始化 | 完成应用自己的外设与参数初始化 |
| ⑥ | `fw_boot_set_confirmed()` | **确认升级有效**：写入 `CONFIRMED` 标志。引导程序下次复位时若发现升级未被确认，会自动回滚到上一份固件（仅模式 1/2；模式 0 覆写成功后由引导自己写 CONFIRMED，不存在未确认状态）。必须在应用真正跑起来之后调用，调用过早会把坏固件也确认掉 |
| ⑦ | `fw_msg_app_ready(...)` | 输出就绪日志，附带项目专有字段（波特率、标定计数等），字段由调用方以格式串传入 |
| ⑧ | 串口升级接收 | 应用侧要支持串口升级时，按[第三章](#三串口升级iap)接入：串口空闲中断置位后唤醒接收任务，取走整帧再交给 `fw_iap_parse_text_frame()` 或 `fw_iap_parse_hex_frame()`。引导构建用 `fw_boot_iap_poll()` 承担同一职责 |

### 13.3 顺序要求与常见错误

| 要求 | 原因 |
|---|---|
| 临界区写在 `fw_port.c` 里，`main()` 不注册 | 内核直接调用 `fw_port_kv_crit_lock()`，由 `configUSE_FREERTOS` 选择关中断还是互斥锁 |
| 引导构建不要用互斥锁做临界区 | 无 OS 环境没有调度器 |
| 应用构建不要用关中断做临界区 | 临界区可能包含 Flash 擦写，关中断会影响系统实时性 |
| `fw_boot_set_confirmed()` 应在应用真正启动后调用 | 它是"升级有效"的凭证，调用过早会让坏固件也被确认 |
| `fw_boot_handler()` 返回即表示没有可跳转的镜像 | 跳转成功时不返回，其后的提示与轮询代码不会被执行 |

IAP 的收帧由接入层负责，整帧取到后调用 `fw_iap_parse_text_frame()` 或 `fw_iap_parse_hex_frame()`，见[第三章](#三串口升级iap)。

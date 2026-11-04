#ifndef _FW_CFG_H_
#define _FW_CFG_H_

/* ========================================================================
* 全部配置集中在本文件，分三节：
*   一、库内核参数：与具体工程无关的通用参数(擦除态、GC、扇区组织、页大小)
*   二、器件能力  ：本平台 Flash / EEPROM / 串口的实际能力
*   三、板级布局  ：地址划分、fw_kv 分区表、键值编号(工程绑定)
*
* 换芯片或换工程只改本文件；库内其它文件不引用平台头文件、不使用平台宏。
* ======================================================================== */

#include <stdint.h>

/* ==================== 一、库内核参数 ==================== */
/* 整片器件的擦除态(全片一致)：0xFF为多数Flash(含NOR/SPI与多数片内Flash，编程为清位)；
 * 0x00为少数器件(如STM32L0/L1系列的片内Flash/EEPROM，编程为置位)。
 * 它决定扇区与节点状态值的写法，fw_kv据此适配。 */
#ifndef FW_ERASED_VALUE
  #define FW_ERASED_VALUE       0x00
#endif
#define FW_ERASED_WORD            ((uint32_t)FW_ERASED_VALUE * 0x01010101u)
#define FW_ERASED_HALFWORD        ((uint16_t)FW_ERASED_VALUE * 0x0101u)

/* GC阈值(单位:页)：活动扇区剩余<=该页数时提前触发GC。
 * 0=分区满才GC，2=剩2页就GC(默认)。越大GC越提前、单次停顿越短，但GC次数略增。 */
#ifndef FW_KV_GC_THRESHOLD
  #define FW_KV_GC_THRESHOLD  2
#endif

//扇区页数倍率，本项目设为32（得到4096字节扇区，磨损小）
#ifndef FW_KV_SECTOR_PAGES
  #define FW_KV_SECTOR_PAGES  32
#endif

//GC暂存缓冲：搬迁时一次经过缓冲区搬一条记录，能放下一条最大记录即可
#ifndef FW_KV_GC_BUF_SIZE
  #define FW_KV_GC_BUF_SIZE  (2 * FW_FLASH_PAGE_SIZE)
#endif

//分区内最大扇区数
#ifndef FW_KV_MAX_SECTORS
  #define FW_KV_MAX_SECTORS  256U
#endif

//扇区大小 = Flash页大小 × 扇区页数倍率
#define FW_KV_SECTOR_SIZE  (FW_FLASH_PAGE_SIZE * FW_KV_SECTOR_PAGES)

//每个扇区的最大字节数
#define FW_KV_MAX_SECTOR_SIZE  FW_KV_SECTOR_SIZE

/* ==================== 二、器件能力 ==================== */
/* 库据此切分擦写、校验串口缓冲；取值必须与平台器件一致。 */
/* 构建日期：默认取编译器日期；需要完全可复现时在工程选项里固定成"yyyy-mm-dd"，
 * 这样同一份源码跨天编译也能得到逐字节一致的镜像，差分率更稳定。 */
#ifndef FW_BUILD_DATE
  #define FW_BUILD_DATE  __DATE__
#endif

/* 以下器件能力宏都可由工程选项覆盖：换器件时在工程里定义同名宏即可，不必改本文件。 */
#ifndef FW_FLASH_PAGE_SIZE
  #define FW_FLASH_PAGE_SIZE   128u   //主Flash单次擦除的页大小(字节)
#endif
#ifndef FW_FLASH_WRITE_BYTE
  #define FW_FLASH_WRITE_BYTE  4u     //主Flash一次可编程的最小字节数
#endif
#ifndef FW_EEPROM_PAGE_SIZE
  #define FW_EEPROM_PAGE_SIZE  4u     //数据EEPROM按字(4字节)擦除/编程
#endif
#ifndef FW_EEPROM_WRITE_BYTE
  #define FW_EEPROM_WRITE_BYTE 4u     //数据EEPROM写粒度
#endif

/* 平台串口接收缓冲的字节数：库用它做编译期校验，保证放得下最长的IAP帧 */
#ifndef FW_PORT_RX_BUF_SIZE
  #define FW_PORT_RX_BUF_SIZE  340u
#endif

/* 平台后端沿用的名字(它们不包含库头文件，只用这些宏描述自身器件)。
 * 库内一律使用上面的 FW_* 名字；FLASH_PAGE_SIZE 由平台HAL提供，此处不再定义。 */
#define FLASH_WRITE_BYTE       FW_FLASH_WRITE_BYTE
#define EEPROM_PAGE_SIZE       FW_EEPROM_PAGE_SIZE
#define EEPROM_WRITE_BYTE      FW_EEPROM_WRITE_BYTE

/* ==================== 三、板级布局 ==================== */

/* 固件水印：本工程的项目编号。打包时写进镜像元数据，设备侧比对，不符即拒绝，
 * 避免不同项目的固件互相刷入。编号与项目名的对应关系由工程自行维护。 */
#ifndef FW_IMG_PROJECT_ID
  #define FW_IMG_PROJECT_ID    0x00000001u
#endif

/* ==================== Flash ==================== */
/* 以下板级布局宏都可由工程选项覆盖：板级配置在工程里定义同名宏即可，不必改本文件。
 * 各分区的起始地址由基础宏推导，覆盖基础宏后自动跟随。 */
#ifndef FLASH_BASE_ADDR
  #define FLASH_BASE_ADDR   0x08000000
#endif
#ifndef FLASH_TOTAL_SIZE
  #define FLASH_TOTAL_SIZE  (192 * 1024)  //192KB
#endif
#ifndef FLASH_WRITE_GRA
  #define FLASH_WRITE_GRA   32  //32位
#endif


#ifndef FW_BOOT_SIZE
  #define FW_BOOT_SIZE      0x00008000  //32KB
#endif
#ifndef SLOT_SIZE
  #define SLOT_SIZE         0x00010000  //64KB
#endif
#ifndef STAGE_SIZE
  #define STAGE_SIZE        (32 * 1024)  //32KB
#endif

#define FW_BOOT_ADDR         FLASH_BASE_ADDR
#define SLOT0_ADDR        (FW_BOOT_ADDR + FW_BOOT_SIZE)
#define SLOT1_ADDR        (SLOT0_ADDR + SLOT_SIZE)
#define STAGE_ADDR        (SLOT1_ADDR + SLOT_SIZE)

/* ==================== EEPROM ==================== */
#ifndef EEPROM_BASE_ADDR
  #define EEPROM_BASE_ADDR  0x08080000  //片内数据EEPROM基地址
#endif
#ifndef EEPROM_TOTAL_SIZE
  #define EEPROM_TOTAL_SIZE (6 * 1024)  //6KB
#endif
#ifndef EEPROM_WRITE_GRA
  #define EEPROM_WRITE_GRA  32  //设置写入粒度为32位
#endif

/* ==================== RAM ==================== */
#ifndef RAM_BASE_ADDR
  #define RAM_BASE_ADDR     0x20000000
#endif
#ifndef RAM_TOTAL_SIZE
  #define RAM_TOTAL_SIZE    (20 * 1024)  //20KB
#endif
#ifndef RAM_END_ADDR
  #define RAM_END_ADDR      (RAM_BASE_ADDR + RAM_TOTAL_SIZE)
#endif

/* ==================== 各分区扇区大小 ==================== */
//暂存区扇区大小
#define STAGE_SECTOR_SIZE FW_KV_SECTOR_SIZE

//自定义eeprom扇区大小
#define EEPROM_SECTOR_SIZE 256U


/* ==================== fw_kv 分区表(FAL风格，单宏登记多分区) ==================== */
/* FAL风格分区表：所有分区在同一宏内登记，新增分区即新增一行。每行字段依次为
 *     分区名, 起始地址, 大小(字节), 扇区大小(字节), 页大小(字节), 读写擦函数
 * 读写擦用 FW_KV_OPS(读,写,擦) 写本分区用到的三个函数名，因此一张表内可混挂
 * 不同器件(片内EEPROM + 主Flash)。约束(由fw_kv_init检查)：大小为扇区整数倍、
 * 扇区为2的幂且能被页整除。擦除态整片一致，由FW_ERASED_VALUE决定。
 */
#define FW_KV_PART_TABLE  \
{                                                                                     \
  /* 片内数据EEPROM：标定数据 + 系统参数(键值见下面的"键值分配") */                   \
  {"EEPROM",     EEPROM_BASE_ADDR, EEPROM_TOTAL_SIZE, EEPROM_SECTOR_SIZE,          \
   FW_EEPROM_PAGE_SIZE, FW_KV_OPS(fw_port_eeprom_read, fw_port_eeprom_write, fw_port_eeprom_erase)},             \
  /* 主Flash暂存区：OTA升级标志/进度(fw_boot.c使用) */                                   \
  {FW_BOOT_STAGE_NAME, STAGE_ADDR,       STAGE_SIZE,       STAGE_SECTOR_SIZE,            \
   FW_FLASH_PAGE_SIZE,  FW_KV_OPS(fw_port_flash_read, fw_port_flash_write, fw_port_flash_erase)},                \
}

/* ==================== fw_kv 键值(库内核部分) ==================== */
/* 这里只放升级流程自身用到的键，由 fw_boot.c 读写。
 * 键是在分区内取值的，不同分区可以重复；0不用(fw_kv视为非法键)；
 * fw_kv接口收uint32_t，本枚举只是给调用方看的"键名表"。
 * 应用层业务键(标定数据、通信波特率等)由使用方在自己的头文件里按分区编号，
 * 库不感知；镜像大小与版本取自镜像元数据的 image_len / image_ver，不另存KV。
 */
typedef enum
{
  FW_KV_KEY_BOOT_FLAG     = 0x01,   //OTA升级标志        (分区STAGE，fw_boot.c)
  FW_KV_KEY_BOOT_PROGRESS = 0x02,   //升级/交换进度      (分区STAGE，fw_boot.c)
  FW_KV_KEY_BOOT_SCRATCH  = 0x03,   //擦写暂存的单页数据 (分区STAGE，fw_boot.c)
}fw_kv_key_t;

#endif

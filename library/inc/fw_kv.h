#ifndef _FW_KV_H_
#define _FW_KV_H_

#include <stdint.h>
#include "fw_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 擦除态适配(整片一致) ==================== */
/* 擦除态由 FW_ERASED_VALUE 决定(定义在fw_cfg.h)：
 *     0xFF : 多数Flash(含NOR/SPI与多数片内Flash) —— 编程为清位
 *     0x00 : 少数器件(如STM32L0/L1系列片内Flash/EEPROM) —— 编程为置位
 * 扇区/节点的状态值按擦除态自动取反方向，应用代码无需关心。
 */
#if (FW_ERASED_VALUE == 0x00)
  #define FW_KV_ERASED32  FW_ERASED_WORD
  #define FW_KV_SECTOR_EMPTY  FW_ERASED_WORD
  #define FW_KV_SECTOR_ACTIVE  (FW_KV_ERASED32 | 0x00000001U)
  #define FW_KV_SECTOR_FULL  (FW_KV_ERASED32 | 0x00000003U)
  #define FW_KV_NODE_VALID  FW_KV_ERASED32
  #define FW_KV_NODE_DELETED  (FW_KV_ERASED32 | 0x00000001U)
#elif (FW_ERASED_VALUE == 0xFF)
  #define FW_KV_ERASED32  FW_ERASED_WORD
  #define FW_KV_SECTOR_EMPTY  FW_ERASED_WORD
  #define FW_KV_SECTOR_ACTIVE  (FW_KV_ERASED32 & ~0x00000001U)
  #define FW_KV_SECTOR_FULL  (FW_KV_ERASED32 & ~0x00000003U)
  #define FW_KV_NODE_VALID  FW_KV_ERASED32
  #define FW_KV_NODE_DELETED  (FW_KV_ERASED32 & ~0x00000001U)
#else
  #error "FW_ERASED_VALUE must be 0x00 or 0xFF"
#endif

//扇区头、节点头魔数(两种擦除态下都不能等于擦除态本身)
#define FW_KV_SECTOR_MAGIC  0x574C4B56U  //"WLKV"
#define FW_KV_NODE_MAGIC  0x4B564E44U  //"KVND"

//返回码
#define FW_KV_OK  0
#define FW_KV_ERR_PARAM  -1
#define FW_KV_ERR_FLASH  -2
#define FW_KV_ERR_NOT_FOUND  -3
#define FW_KV_ERR_NOSPC  -4
#define FW_KV_ERR_BUF_SMALL  -5
#define FW_KV_ERR_CRC  -6

//Flash后端接口：fw_kv只通过这三个函数访问Flash
typedef struct
{
  int (*read )(uint32_t addr, uint8_t *buf, uint32_t size);
  int (*write)(uint32_t addr, const uint8_t *buf, uint32_t size);
  int (*erase)(uint32_t addr, uint32_t size);
}fw_flash_ops_t;

/* 分区表每行末尾直接写本分区用到的三个函数名(C99复合字面量，静态存储期)：
 *     { "名字", 基址, 大小, 扇区, 页, FW_KV_OPS(读函数, 写函数, 擦函数) }
 * 于是每行自带底层读写擦，同一张表里能挂不同器件(如片内EEPROM + 主Flash)；
 * 已经写好fw_flash_ops_t实例的，也可以直接写 &xxx_ops。
 * 三个函数形式：f(addr, buf, size)读；f(addr, const buf, size)写(任意地址/长度，
 * 内部读-改-写)；f(addr, size)擦(按扇区调用)。返回0=成功，非0=失败；
 * 工程里返回 xxx_err_t(OK=0) 的函数可直接写进来，宏里会转一下类型。
 */
#define FW_KV_OPS(read_fn, write_fn, erase_fn)                                                      \
  (&(const fw_flash_ops_t){ .read  = (int (*)(uint32_t, uint8_t *, uint32_t))(read_fn),             \
                            .write = (int (*)(uint32_t, const uint8_t *, uint32_t))(write_fn),      \
                            .erase = (int (*)(uint32_t, uint32_t))(erase_fn) })

/* KV分区描述：一个fw_kv实例对应一个分区。地址、扇区/页大小和底层接口都收在
 * 这里(FAL风格)，调用方只需定义一个表项即可挂接任意底层存储。
 */
typedef struct fw_kv_part
{
  const char          *name;  //分区名
  uint32_t             base;        //分区起始地址
  uint32_t             size;        //分区大小（字节）
  uint32_t             sector_size;  //扇区大小（字节），须为2的幂且是页大小的整数倍
  uint32_t             page_size;   //页大小（字节）
  const fw_flash_ops_t *ops;        //底层读/写/擦
}fw_kv_part_t;

/* ==================== 常驻key映射 ==================== */
/* 把"key→最新记录位置"常驻在实例里，get时直接定位，不必每次遍历整个分区。
 * 键数量超过上限时自动回退到全扫描(功能不受影响，只是变慢)。 */
#ifndef FW_KV_MAX_KEYS
  #define FW_KV_MAX_KEYS  8
#endif

//key→记录位置的一条映射
typedef struct
{
  uint32_t key;  //键值
  uint32_t seq;  //所在扇区序号
  uint32_t sector;  //所在扇区索引
  uint32_t offset;  //记录在扇区内的偏移
}fw_kv_key_loc_t;

//实例结构，调用方只负责分配内存，字段由fw_kv_init管理
typedef struct fw_kv
{
  fw_flash_ops_t ops;  //Flash后端
  const char *name;  //分区名，取自分区描述；日志回调直接使用，调用方无需反查
  uint32_t base;  //分区起始地址
  uint32_t size;  //分区大小
  uint32_t sector_size;  //扇区大小
  uint32_t page_size;  //页大小（如主Flash=128、EEPROM=4）
  uint32_t sector_count;  //扇区数量
  int      active_sector;  //当前活动扇区，-1表示尚未分配
  uint32_t used;  //活动扇区已用字节数（含扇区头）
  uint32_t next_seq;  //下一个扇区序号
  fw_kv_key_loc_t key_map[FW_KV_MAX_KEYS];  //常驻key映射(每个实例一份)
  uint32_t key_count;  //映射中的key数量
  uint8_t  key_map_ok;  //1=映射已建立且可用
}fw_kv_t;

//分区统计信息，用于评估磨损均衡与空间占用
typedef struct fw_kv_stats
{
  uint32_t sector_size;  //扇区大小（字节）
  uint32_t sector_count;  //总扇区数
  uint32_t active_sector;  //当前活动扇区，UINT32_MAX表示无
  uint32_t full_sectors;  //非活动有效扇区数（已写满，不追加新记录）
  uint32_t empty_sectors;  //空/无效扇区数
  uint32_t dirty_sectors;  //含可回收垃圾的扇区数
  uint32_t used_bytes;  //活动扇区已用字节（含扇区头）
  uint32_t garbage_bytes;  //总可回收垃圾字节
  uint32_t live_bytes;  //总有效数据字节
}fw_kv_stats_t;

/* ==================== 分区表(FAL风格，单宏登记多分区) ==================== */
//全部fw_kv分区的表：内容由fw_cfg.h的FW_KV_PART_TABLE决定，由fw_cfg.c实例化
extern const fw_kv_part_t fw_kv_part_table[];
extern const uint32_t     fw_kv_part_count;

/* ==================== 日志回调(库不依赖任何日志模块) ==================== */
/* 本库只上报事件，输出方式由应用决定；未注册回调时不产生任何输出。
 * 事件与参数含义：
 *   FW_KV_EVT_GC     : a=源扇区，b=目标扇区，c=目标扇区已用字节
 *   FW_KV_EVT_ROTATE : a=扇区号，b未用，c=扇区序号
 *   FW_KV_EVT_ERROR  : a=返回码，b/c未用
 * 分区名随事件给出(part_name)，应用侧无需访问KV实例字段。
 * 回调可能在Flash擦写期间调用，实现中只应做非阻塞输出。
 */
typedef enum
{
  FW_KV_EVT_GC     = 0,   //垃圾回收
  FW_KV_EVT_ROTATE = 1,   //扇区轮转
  FW_KV_EVT_ERROR  = 2    //读写失败
}fw_kv_evt_t;

//日志回调类型：事件 + 实例 + 分区名，其余按事件解释
typedef void (*fw_kv_log_fn_t)(fw_kv_evt_t evt, const fw_kv_t *kv, const char *part_name,
                               int a, int b, uint32_t c);

//注册日志回调(未注册时不产生输出)
void fw_kv_set_log(fw_kv_log_fn_t fn);

/* ==================== 临界区 ==================== */
/* 每个对外接口内部是一串多步操作：读写实例状态，必要时触发扇区轮转与GC。
 * 这串操作必须整体原子，否则并发访问会破坏实例状态与Flash上的一致性。
 * 保护方式由平台适配层的fw_port_kv_crit_lock()/unlock()提供(见fw_port.h)，
 * 内核不关心它是关中断还是互斥锁，使用方也不必在main里注册任何回调。 */

//按分区描述初始化实例(扫描全分区、建常驻映射)；返回FW_KV_OK或错误码
int fw_kv_init(fw_kv_t *kv, const fw_kv_part_t *part);
//按名字在分区表里找一项，找不到返回NULL
const fw_kv_part_t *fw_kv_part_find(const char *name);
//按分区名初始化(内部先查找再init)
int fw_kv_init_by_name(fw_kv_t *kv, const char *name);
//读一个键。*len入参是缓冲区大小、出参是实际长度；缓冲区不够时回BUF_SMALL并回填所需长度
int fw_kv_get(fw_kv_t *kv, uint32_t key, uint8_t *value, uint32_t *len);
//写一个键(追加一条新记录，旧记录留作垃圾等GC回收)
int fw_kv_set(fw_kv_t *kv, uint32_t key, const uint8_t *value, uint32_t len);
//删除一个键(追加一条已删除标记)
int fw_kv_del(fw_kv_t *kv, uint32_t key);
//取分区统计(磨损均衡与空间占用)
int fw_kv_get_stats(fw_kv_t *kv, fw_kv_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* _FW_KV_H_ */

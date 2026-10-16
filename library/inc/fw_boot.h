#ifndef _BOOT_H_
#define _BOOT_H_

#include <stdint.h>
#include "fw_cfg.h"

//当前编译FwLoader的工程：0=Bootloader，1=App(正常由工程选项传入)
#ifndef FW_BOOT_BUILD_LOADER
  #define FW_BOOT_BUILD_LOADER				1
#endif

//升级模式：0=覆写 1=交换 2=XIP
//更换模式前须先擦除两个Slot；旧模式残留的有效镜像会导致地址错乱
//XIP模式适合支持双Bank的芯片；不支持双Bank时，要把两份固件编译到不同的IROM地址
#define FW_BOOT_UPGRADE_MODE   0
/* BootLoader内置的串口固件升级(IAP)开关：
 *   1 = 启用：两个槽位都没有有效镜像时，BootLoader停在串口升级模式，
 *       等待上位机发送固件(文本$UP或HEX 0x55AA协议，与App是同一套)
 *   0 = 关闭：无有效镜像时只停在提示循环，需要SWD重新烧录
 * 说明：App构建本来就带IAP，这个宏只影响BootLoader构建。
 */
#ifndef FW_BOOT_IAP_ENABLE
  #define FW_BOOT_IAP_ENABLE					1
#endif
#define VTOR_ALIGN_MASK								0x7F  //VTOR 128字节对齐掩码
//暂存区KV分区名：升级标志、进度、镜像大小、交换暂存都存在这里
//用到的键(FW_KV_KEY_BOOT_*)统一在fw_cfg.h的键值表里定义
#define FW_BOOT_STAGE_NAME						"STAGE"

//升级标志(存于STAGE分区的FW_KV_KEY_BOOT_FLAG)
#define FW_BOOT_FLAG_NONE							FW_ERASED_WORD   //无操作(擦除态)
#define FW_BOOT_FLAG_VALID						0x5A5A5A5A          //升级待执行
#define FW_BOOT_FLAG_ROLLBACK					0xA5A5A5A5          //强制回滚
#define FW_BOOT_FLAG_CONFIRMED				0x01010101          //升级已确认(App正常启动)

//模式专用配置
#if (FW_BOOT_UPGRADE_MODE == 1)
  //交换进度值（存储在PROGRESS位置）
  #define FW_SWAP_PROGRESS_NONE  FW_ERASED_WORD  //无交换进行中（擦除态）
  #define FW_SWAP_PROGRESS_COMPLETE		0xCCCCCCCC  //交换已完成，等待App确认（全字节0xCC重复，区分进度值1~512）
#elif (FW_BOOT_UPGRADE_MODE == 2)
  //XIP运行槽位值（存储在PROGRESS位置）
  #define FW_XIP_RUN_SLOT0						0x01010101
  #define FW_XIP_RUN_SLOT1						0x02020202
  #define FW_XIP_SLOT_MASK						0x7F7F7F7F  //槽位掩码（屏蔽confirm位）
  #define FW_XIP_CONFIRM_BIT					0x80808080  //等待确认标志位
#endif

//API函数声明
int fw_boot_stage_init(void);
void fw_boot_set_confirmed(void);
int fw_boot_image_is_valid(uint32_t addr);

//以下升级主流程接口仅Boot构建需要，App构建不编译
#if (FW_BOOT_BUILD_LOADER == 0)
void fw_boot_handler(void);

#if (FW_BOOT_IAP_ENABLE != 0)
//没有可跳转的镜像时，fw_boot_handler会返回，之后由它接管串口升级
void fw_boot_iap_poll(void);
#endif
void fw_boot_jump_to_app(uint32_t app_addr);
#endif

//标志区操作
//KV元数据读写(策略文件也要用)
uint32_t fw_boot_kv_read_u32(uint32_t key, uint32_t default_val);
int fw_boot_kv_write_u32(uint32_t key, uint32_t value);
uint32_t fw_boot_flag_read(void);
int fw_boot_flag_write(uint32_t flag);
uint32_t fw_boot_progress_read(void);
int fw_boot_progress_write(uint32_t value);
uint32_t fw_boot_image_size_read(void);
int fw_boot_image_size_write(uint32_t size);

//XIP槽位查询
#if (FW_BOOT_UPGRADE_MODE == 2)
uint32_t fw_xip_get_run_slot(void);
uint32_t fw_xip_is_pending_confirm(void);
#endif

#endif /* _BOOT_H_ */

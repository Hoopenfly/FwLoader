#ifndef _BOOT_STRATEGY_H_
#define _BOOT_STRATEGY_H_

#include <stdint.h>
#include "fw_boot.h"
#include "fw_kv.h"      //fw_kv_t(策略文件要用fw_boot_kv)

/* ==================== 升级策略接口 ====================
 * 三种升级方式各独立成文件(对应MCUboot的overwrite-only/swap/direct-XIP)：
 *     FW_BOOT_UPGRADE_MODE == 0  ->  fw_overwrite.c   覆写
 *     FW_BOOT_UPGRADE_MODE == 1  ->  fw_swap.c        交换
 *     FW_BOOT_UPGRADE_MODE == 2  ->  fw_xip.c         就地执行
 * 每个策略文件用文件级#if包住，一次构建只会编到其中一个；fw_boot.c的主流程
 * 只调下面这组接口，按模式分叉的语句级#if集中在各策略文件内。
 */

//启动过程中各阶段共享的状态(定义在fw_boot.c)
extern fw_kv_t  fw_boot_kv;        //暂存区KV实例
extern uint32_t boot_cur_flag;  //当前升级标志
extern int      boot_slot0_ok;  //SLOT0是否有有效镜像
extern int      boot_slot1_ok;  //SLOT1是否有有效镜像

//读升级标志与两个槽位的有效性(XIP另读运行槽位)
void fw_strategy_state_read(void);
//恢复上次被中断的升级；已修好且SLOT0可用时直接跳转(不返回)
void fw_strategy_recover(void);
//处理强制回滚标志FW_BOOT_FLAG_ROLLBACK
void fw_strategy_rollback_flag(void);
//处理升级请求FW_BOOT_FLAG_VALID；成功后可直接跳转(不返回)
void fw_strategy_upgrade(void);
//处理"升级已执行但App未确认"的情况(自动回滚)
void fw_strategy_unconfirmed(void);
//选一个有效镜像跳转；两个都无效则返回，由主流程报错
void fw_strategy_select_and_jump(void);
//真正执行升级动作(覆写SLOT1->SLOT0，或XIP切槽位)
int fw_strategy_perform(void);

//真正执行回滚动作(仅交换/就地执行模式需要)
#if (FW_BOOT_UPGRADE_MODE == 1) || (FW_BOOT_UPGRADE_MODE == 2)
int fw_strategy_rollback(void);
#endif

#endif /* _BOOT_STRATEGY_H_ */

/**
* @file    fw_xip.c
* @brief   升级策略-模式2：就地执行(XIP)
* @note    见 fw_strategy.h 的接口说明；本文件只在对应模式下编译。
*/

#include "fw_boot.h"
#include "fw_strategy.h"
#include <string.h>
#include "fw_msg.h"
#include "fw_kv.h"

//这两个查询不带FW_BOOT_BUILD_LOADER条件：App构建(模式2)下fw_boot_set_confirmed()也要用
#if (FW_BOOT_UPGRADE_MODE == 2)

/**
* @brief   获取XIP模式当前运行槽位
* @return  FW_XIP_RUN_SLOT0或FW_XIP_RUN_SLOT1
*/
uint32_t fw_xip_get_run_slot(void)
{
  uint32_t slot = fw_boot_progress_read();
  slot &= FW_XIP_SLOT_MASK;//屏蔽确认位，仅提取槽位号
  if(slot != FW_XIP_RUN_SLOT0 && slot != FW_XIP_RUN_SLOT1)
  {
    return FW_XIP_RUN_SLOT0;
  }
  return slot;
}

/**
* @brief   检测XIP模式是否处于"待App确认"状态（CONFIRM_BIT已置位）
* @return  0=正常状态，非0=升级已切换槽位但App尚未确认
*/
uint32_t fw_xip_is_pending_confirm(void)
{
  return(fw_boot_progress_read() & FW_XIP_CONFIRM_BIT) ? 1 : 0;
}

#endif /* FW_BOOT_UPGRADE_MODE == 2 */

#if (FW_BOOT_BUILD_LOADER == 0) && (FW_BOOT_UPGRADE_MODE == 2)

//XIP运行状态(由fw_strategy_state_read填写)
static uint32_t boot_run_slot;   //当前运行槽位
static int      boot_cur_ok;     //当前运行槽位是否有有效镜像
static int      boot_other_ok;   //对侧槽位是否有有效镜像

/**
* @brief   写入XIP模式当前运行槽位
* @param   run_slot  FW_XIP_RUN_SLOT0或FW_XIP_RUN_SLOT1
* @return  0表示成功
*/
static int fw_xip_set_run_slot(uint32_t run_slot)
{
  return fw_boot_progress_write(run_slot);
}

/**
* @brief   获取XIP模式下待升级的目标槽位地址
* @return  目标槽位起始地址（当前运行槽位的对侧）
*/
static uint32_t fw_xip_get_upgrade_target_address(void)
{
  uint32_t run_slot = fw_xip_get_run_slot();
  return(run_slot == FW_XIP_RUN_SLOT0) ? SLOT1_ADDR : SLOT0_ADDR;
}

/**
* @brief   读本次启动的状态(含XIP运行槽位)
*/
void fw_strategy_state_read(void)
{
  boot_cur_flag = fw_boot_flag_read();
  boot_slot0_ok = fw_boot_image_is_valid(SLOT0_ADDR);
  boot_slot1_ok = fw_boot_image_is_valid(SLOT1_ADDR);
  fw_msg_boot_state(boot_cur_flag, boot_slot0_ok, boot_slot1_ok);
  boot_run_slot = fw_xip_get_run_slot();
  boot_cur_ok = (boot_run_slot == FW_XIP_RUN_SLOT0) ? boot_slot0_ok : boot_slot1_ok;
  boot_other_ok = (boot_run_slot == FW_XIP_RUN_SLOT0) ? boot_slot1_ok : boot_slot0_ok;
}

/**
* @brief   恢复被中断的升级
* @note    XIP模式只切槽位、不做数据搬运，没有需要恢复的中间状态
*/
void fw_strategy_recover(void)
{
}

/**
* @brief   处理强制回滚标志
* @note    XIP模式回滚：切回另一个运行槽位(不设confirm位)
*/
void fw_strategy_rollback_flag(void)
{
  if(boot_cur_flag != FW_BOOT_FLAG_ROLLBACK)
  {
    return;
  }
  fw_msg_rollback_start();
  if(fw_strategy_rollback() == 0)
  {
    fw_boot_flag_write(FW_BOOT_FLAG_CONFIRMED);   //回滚完成，置为已确认
    boot_run_slot = fw_xip_get_run_slot();     //读回滚之后的运行槽位
    boot_cur_ok = 1;                        //刚切过去的槽位必定有效
    boot_slot0_ok = fw_boot_image_is_valid(SLOT0_ADDR);
    boot_slot1_ok = fw_boot_image_is_valid(SLOT1_ADDR);
    fw_msg_rollback_done();
  }
  else
  {
    fw_msg_error(FW_MSG_ERR_ROLLBACK, 0);
    fw_boot_flag_write(FW_BOOT_FLAG_NONE);
  }
  boot_cur_flag = fw_boot_flag_read();
}

/**
* @brief   处理升级标志
* @note    仅当未设置confirm位时才执行新槽位切换；已设置时说明上次启动已执行过，
*          留给strategy_unconfirmed()处理
*/
void fw_strategy_upgrade(void)
{
  if(boot_cur_flag != FW_BOOT_FLAG_VALID)
  {
    return;
  }
  if(!fw_xip_is_pending_confirm())
  {
    uint32_t target_addr = fw_xip_get_upgrade_target_address();
    if(fw_boot_image_is_valid(target_addr))
    {
      if(fw_strategy_perform() == 0)
      {
        //升级成功(confirm位已设置)，直接跳转到新固件
        boot_run_slot = fw_xip_get_run_slot();
        boot_cur_ok = 1;
        fw_boot_jump_to_app(target_addr);
      }
      else
      {
        fw_msg_error(FW_MSG_ERR_UPDATE, 0);
        fw_boot_flag_write(FW_BOOT_FLAG_NONE);
      }
    }
    else
    {
      fw_msg_error(FW_MSG_ERR_SLOT1_BAD, 0);
      fw_boot_flag_write(FW_BOOT_FLAG_NONE);
    }
  }
  boot_cur_flag = fw_boot_flag_read();
}

/**
* @brief   处理"升级已执行但App未确认"
* @note    confirm位仍置着而App没有清掉，说明新固件启动失败，切回旧槽位
*/
void fw_strategy_unconfirmed(void)
{
  if(boot_cur_flag != FW_BOOT_FLAG_VALID || !fw_xip_is_pending_confirm())
  {
    return;
  }
  fw_msg_rollback_start();
  if(boot_other_ok)     //对侧槽位还有效，才回滚得回去
  {
    if(fw_strategy_rollback() == 0)
    {
      fw_msg_rollback_done();
      fw_boot_flag_write(FW_BOOT_FLAG_NONE);
      boot_run_slot = fw_xip_get_run_slot();
      boot_cur_ok = 1;
      boot_slot0_ok = fw_boot_image_is_valid(SLOT0_ADDR);
      boot_slot1_ok = fw_boot_image_is_valid(SLOT1_ADDR);
      boot_cur_flag = fw_boot_flag_read();
    }
    else
    {
      fw_msg_error(FW_MSG_ERR_ROLLBACK, 0);
    }
  }
  else
  {
    fw_msg_error(FW_MSG_ERR_ROLLBACK, 0);
    fw_boot_flag_write(FW_BOOT_FLAG_NONE);
    boot_cur_flag = fw_boot_flag_read();
  }
}

/**
* @brief   选有效镜像跳转(XIP模式：跳当前运行槽位，坏了就对侧)
*/
void fw_strategy_select_and_jump(void)
{
  uint32_t run_addr = (boot_run_slot == FW_XIP_RUN_SLOT0) ? SLOT0_ADDR : SLOT1_ADDR;
  if(boot_cur_ok)
  {
    fw_boot_jump_to_app(run_addr);
  }
  //当前运行槽位坏了：切到对侧槽位再跳
  if(boot_other_ok)
  {
    uint32_t fallback_addr = (boot_run_slot == FW_XIP_RUN_SLOT0) ? SLOT1_ADDR : SLOT0_ADDR;
    uint32_t new_slot = (boot_run_slot == FW_XIP_RUN_SLOT0) ? FW_XIP_RUN_SLOT1 : FW_XIP_RUN_SLOT0;
    fw_xip_set_run_slot(new_slot);
    fw_boot_jump_to_app(fallback_addr);
  }
}

/**
* @brief   执行升级(就地执行模式)
* @return  0表示升级成功
*/
int fw_strategy_perform(void)
{
  uint32_t current_slot = fw_xip_get_run_slot();
  uint32_t new_slot = (current_slot == FW_XIP_RUN_SLOT0) ? FW_XIP_RUN_SLOT1 : FW_XIP_RUN_SLOT0;
  //写入新槽位号并置位pending标志（等待App确认）
  if(fw_xip_set_run_slot(new_slot | FW_XIP_CONFIRM_BIT) != 0)
  {
    return -1;
  }
  return 0;
}

/**
* @brief   执行回滚(就地执行模式)
* @return  0表示回滚成功
*/
int fw_strategy_rollback(void)
{
  //XIP模式：切换运行槽位实现回滚（不设置confirm位）
  uint32_t current_slot = fw_xip_get_run_slot();
  uint32_t target_slot;
  uint32_t target_addr;
  if(current_slot == FW_XIP_RUN_SLOT0)
  {
    target_slot = FW_XIP_RUN_SLOT1;
    target_addr = SLOT1_ADDR;
  }
  else
  {
    target_slot = FW_XIP_RUN_SLOT0;
    target_addr = SLOT0_ADDR;
  }
  //验证目标槽位镜像是否有效
  if(!fw_boot_image_is_valid(target_addr))
  {
    return -1;
  }
  //写入目标槽位号（不含confirm位，清除待确认状态）
  if(fw_xip_set_run_slot(target_slot) != 0)
  {
    return -1;
  }
  return 0;
}

#endif /* FW_BOOT_BUILD_LOADER == 0 && FW_BOOT_UPGRADE_MODE == 2 */

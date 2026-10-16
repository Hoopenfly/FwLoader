/**
* @file    fw_overwrite.c
* @brief   升级策略-模式0：覆写
* @note    见 fw_strategy.h 的接口说明；本文件只在对应模式下编译。
*/

#include "fw_boot.h"
#include "fw_strategy.h"
#include <string.h>
#include "fw_msg.h"
#include "fw_kv.h"
#include "fw_port.h"   //平台抽象接口

#if (FW_BOOT_BUILD_LOADER == 0) && (FW_BOOT_UPGRADE_MODE == 0)

//页缓冲区：覆写时先读出整页，再写回目标页
static __attribute__((aligned(FW_FLASH_WRITE_BYTE))) uint8_t boot_page_buf[FW_FLASH_PAGE_SIZE];

/**
* @brief   读本次启动的状态
*/
void fw_strategy_state_read(void)
{
  boot_cur_flag = fw_boot_flag_read();
  boot_slot0_ok = fw_boot_image_is_valid(SLOT0_ADDR);
  boot_slot1_ok = fw_boot_image_is_valid(SLOT1_ADDR);
  fw_msg_boot_state(boot_cur_flag, boot_slot0_ok, boot_slot1_ok);
}

/**
* @brief   恢复被中断的升级
* @note    覆写模式不做交换，没有需要恢复的中间状态
*/
void fw_strategy_recover(void)
{
}

/**
* @brief   处理强制回滚标志
* @note    覆写模式不支持回滚，清除标志即可
*/
void fw_strategy_rollback_flag(void)
{
  if(boot_cur_flag != FW_BOOT_FLAG_ROLLBACK)
  {
    return;
  }
  fw_boot_flag_write(FW_BOOT_FLAG_CONFIRMED);//覆写模式不支持回滚，改成"已确认"即可
  boot_cur_flag = fw_boot_flag_read();
}

/**
* @brief   处理升级标志
* @note    覆写模式每次VALID都执行覆写(无状态追踪)
*/
void fw_strategy_upgrade(void)
{
  if(boot_cur_flag != FW_BOOT_FLAG_VALID)
  {
    return;
  }
  if(boot_slot1_ok)
  {
    if(fw_strategy_perform() == 0)
    {
      fw_boot_flag_write(FW_BOOT_FLAG_CONFIRMED);//覆写成功，置为已确认
      boot_slot0_ok = fw_boot_image_is_valid(SLOT0_ADDR);//重新校验覆写结果
      if(boot_slot0_ok)
      {
        fw_boot_jump_to_app(SLOT0_ADDR);//校验通过才跳转
      }
    }
    else
    {
      fw_msg_error(FW_MSG_ERR_UPDATE, 0);
      fw_boot_flag_write(FW_BOOT_FLAG_NONE);//覆写失败，清除标志避免反复重试
    }
  }
  else
  {
    fw_msg_error(FW_MSG_ERR_SLOT1_BAD, 0);
    fw_boot_flag_write(FW_BOOT_FLAG_NONE);//SLOT1没有有效镜像，清标志
  }
  boot_cur_flag = fw_boot_flag_read();
}

/**
* @brief   处理"升级已执行但App未确认"
* @note    覆写模式没有待确认状态
*/
void fw_strategy_unconfirmed(void)
{
}

/**
* @brief   执行升级(覆写模式)
* @return  0表示升级成功
*/
int fw_strategy_perform(void)
{
  uint32_t total_size = SLOT_SIZE;
  uint32_t num_pages;
  int ret;
  //擦除目标SLOT0区域
  ret = fw_port_flash_erase(SLOT0_ADDR, SLOT_SIZE);
  if(ret != 0)
  {
    return ret;
  }
  //计算需要写入的页数
  num_pages = (total_size + FW_FLASH_PAGE_SIZE - 1) / FW_FLASH_PAGE_SIZE;
  fw_msg_update_start(num_pages);
  /* 逐页复制SLOT1到SLOT0，从最后一页往第一页写：向量表在第0页，让它最后落盘，
   * 中途掉电或写失败时它仍是擦除态、判为无效，不会误跳到残缺镜像上。
   * 循环用"已完成页数"驱动，进度打印才不会跟着倒着走。 */
  for(uint32_t done = 0; done < num_pages; done++)
  {
    uint32_t page = num_pages - 1u - done;      //实际页号：从最后一页往前推
    uint32_t src_addr = SLOT1_ADDR + page * FW_FLASH_PAGE_SIZE;
    uint32_t dst_addr = SLOT0_ADDR + page * FW_FLASH_PAGE_SIZE;
    uint32_t copy_size = FW_FLASH_PAGE_SIZE;
    fw_msg_update_progress(done + 1u, num_pages);
    //最后一页可能不足一页
    if((page + 1) == num_pages)
    {
      uint32_t last_page_size = total_size % FW_FLASH_PAGE_SIZE;
      if(last_page_size != 0)
      {
        copy_size = last_page_size;
      }
    }
    //读出源页再写进目标页(目标页已在前面整块擦过)
    if(fw_port_flash_read(src_addr, boot_page_buf, copy_size) != 0)
    {
      return -1;
    }
    if(fw_port_flash_write(dst_addr, boot_page_buf, copy_size) != 0)
    {
      return -1;
    }
  }
  fw_msg_update_done();
  return 0;
}

#endif /* FW_BOOT_BUILD_LOADER == 0 && FW_BOOT_UPGRADE_MODE == 0 */

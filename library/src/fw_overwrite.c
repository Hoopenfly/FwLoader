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
#include "fw_img.h"    //fw_img_locate：按元数据取镜像实际长度

#if (FW_BOOT_BUILD_LOADER == 0) && (FW_BOOT_UPGRADE_MODE == 0)

//页缓冲区：覆写时先读出整页，再写回目标页
static __attribute__((aligned(FW_FLASH_WRITE_BYTE))) uint8_t boot_page_buf[FW_FLASH_PAGE_SIZE];

/**
* @brief   读本次启动的状态
*/
void fw_strategy_state_read(void)
{
  boot_cur_flag = fw_boot_flag_read();
  boot_slot0_ok = (fw_img_meta_verify(SLOT0_ADDR, SLOT_SIZE, NULL) == FW_IMG_OK) ? 1 : 0;
  boot_slot1_ok = (fw_img_meta_verify(SLOT1_ADDR, SLOT_SIZE, NULL) == FW_IMG_OK) ? 1 : 0;
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
      boot_slot0_ok = (fw_img_crc32_verify(SLOT0_ADDR, SLOT_SIZE, NULL) == FW_IMG_OK) ? 1 : 0;//重新校验覆写结果
      if(boot_slot0_ok)
      {
        fw_boot_flag_write(FW_BOOT_FLAG_CONFIRMED);//校验通过才置为已确认
        fw_boot_jump_to_app(SLOT0_ADDR);
      }
      else
      {
        /*搬页写坏：VERIFY失败时保持VALID，下次上电用SLOT1里完好的镜像重做一次覆写；
          不能在此写CONFIRMED，否则fw_boot.c的"flag==VALID才重做"分支被挡住，失去唯一恢复路径*/
        fw_msg_error(FW_MSG_ERR_UPDATE, 0);
      }
    }
    else
    {
      /*搬页中途失败：SLOT0可能已被擦除或只写了一部分，此处同样保持VALID，
        让下次上电重新执行一次覆写；若改写成NONE，阶段4的"flag==VALID才允许用SLOT1恢复"
        会被挡住，而SLOT0又过不了CRC校验，设备只能靠串口重新升级 */
      fw_msg_error(FW_MSG_ERR_UPDATE, 0);
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
  //按镜像元数据里的image_len搬运：只搬有效页，尾部擦除态不必搬
  uint32_t total_size = SLOT_SIZE;
  uint32_t num_pages;
  fw_img_meta_t meta;
  //搬的是SLOT1→SLOT0，长度取SLOT1里那个镜像的元数据
  if((fw_img_locate(SLOT1_ADDR, SLOT_SIZE, &meta) == FW_IMG_OK) &&
     (meta.image_len > 0u) && (meta.image_len <= SLOT_SIZE))
  {
    total_size = meta.image_len;
  }
  //计算需要写入的页数
  num_pages = (total_size + FW_FLASH_PAGE_SIZE - 1) / FW_FLASH_PAGE_SIZE;
  fw_msg_update_start(num_pages);
  /* 逐页复制SLOT1到SLOT0，从第0页顺序写入。搬运过程本身不保证中途中断后的完整性，
   * 完整性由跳转前的fw_img_crc32_verify()把关：它回读代码算CRC32并与元数据比对，
   * 半成品镜像不会被判为有效，也就不会跳转过去。 */
  for(uint32_t page = 0; page < num_pages; page++)
  {
    uint32_t src_addr = SLOT1_ADDR + page * FW_FLASH_PAGE_SIZE;
    uint32_t dst_addr = SLOT0_ADDR + page * FW_FLASH_PAGE_SIZE;
    uint32_t copy_size = FW_FLASH_PAGE_SIZE;
    fw_msg_update_progress(page + 1u, num_pages);
    //最后一页可能不足一页：按镜像实际长度算出该页有效字节数
    if((page + 1) == num_pages)
    {
      uint32_t last_page_size = total_size % FW_FLASH_PAGE_SIZE;
      if(last_page_size != 0)
      {
        copy_size = last_page_size;
      }
    }
    //先读源页再擦目标页：源页不可读时目标页保持原样，不会把唯一可用的镜像弄丢
    if(fw_port_flash_read(src_addr, boot_page_buf, copy_size) != 0)
    {
      return -1;
    }
    if(fw_port_flash_erase(dst_addr, FW_FLASH_PAGE_SIZE) != 0)
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

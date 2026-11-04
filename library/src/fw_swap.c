/**
* @file    fw_swap.c
* @brief   升级策略-模式1：交换
* @note    见 fw_strategy.h 的接口说明；本文件只在对应模式下编译。
*/

#include "fw_boot.h"
#include "fw_strategy.h"
#include <string.h>
#include "fw_msg.h"
#include "fw_kv.h"
#include "fw_port.h"   //平台抽象接口
#include "fw_assert.h"//FW_STATIC_ASSERT：暂存记录尺寸的编译期检查
#include "fw_img.h"    //fw_img_locate：按元数据取镜像实际长度

#if (FW_BOOT_BUILD_LOADER == 0) && (FW_BOOT_UPGRADE_MODE == 1)

//交换时的单页暂存缓冲：4字节页号 + 整页数据
static uint8_t boot_scratch_buf[4 + FW_FLASH_PAGE_SIZE];

/* 暂存记录最终写进KV，其长度必须能装进一个KV扇区，否则交换升级在写第一条
 * scratch时就失败，而且只表现为策略函数返回-1。配置错误在这里直接拦下：
 * 记录长度 = 节点头16 + (页号4 + 页数据) + CRC4 */
FW_STATIC_ASSERT((4 + FW_FLASH_PAGE_SIZE + 24) <= FW_KV_SECTOR_SIZE,
    "boot scratch record must fit into one KV sector");

//页缓冲区：读一页/写一页的搬运缓冲
static __attribute__((aligned(FW_FLASH_WRITE_BYTE))) uint8_t boot_page_buf[FW_FLASH_PAGE_SIZE];

/**
* @brief   读取交换进度
* @return  FW_SWAP_PROGRESS_NONE表示未开始，1-512表示已完成页数
*/
static uint32_t fw_swap_progress_read(void)
{
  return fw_boot_progress_read();
}

/**
* @brief   写入交换进度
* @param   progress    FW_SWAP_PROGRESS_NONE或已完成页数（1-512）
* @return  0表示成功
*/
static int fw_swap_progress_write(uint32_t progress)
{
  return fw_boot_progress_write(progress);
}

/**
* @brief   计算交换模式需要交换的页数（依据镜像元数据里的image_len）
* @return  需要交换的页数
* @note    元数据定位失败或长度非法时，回退到整个SLOT的页数
*/
static uint32_t fw_swap_num_pages(void)
{
  uint32_t total_size = SLOT_SIZE;
  fw_img_meta_t meta;
  if((fw_img_locate(SLOT0_ADDR, SLOT_SIZE, &meta) == FW_IMG_OK) &&
     (meta.image_len > 0u) && (meta.image_len <= SLOT_SIZE))
  {
    total_size = meta.image_len;
  }
  return(total_size + FW_FLASH_PAGE_SIZE - 1) / FW_FLASH_PAGE_SIZE;
}

/**
* @brief   复制一页Flash数据从源地址到目标地址（自动擦除目标页）
* @param   src_addr    源地址
* @param   dst_addr    目标地址
* @return  0表示成功
*/
static int fw_swap_copy_src_to_dst(uint32_t src_addr, uint32_t dst_addr)
{
  int ret;
  //擦除目标页
  ret = fw_port_flash_erase(dst_addr, FW_FLASH_PAGE_SIZE);
  if(ret != 0)
  {
    return ret;
  }
  //读取源页
  ret = fw_port_flash_read(src_addr, boot_page_buf, FW_FLASH_PAGE_SIZE);
  if(ret != 0)
  {
    return ret;
  }
  //写入目标页
  return fw_port_flash_write(dst_addr, boot_page_buf, FW_FLASH_PAGE_SIZE);
}

/**
* @brief   把SLOT0的指定页备份到暂存KV
* @param   page    页索引
* @return  0/-1
*/
static int fw_swap_scratch_kv_save(uint32_t page)
{
  uint32_t slot0_pg = SLOT0_ADDR + page * FW_FLASH_PAGE_SIZE;
  memcpy(boot_scratch_buf, &page, sizeof(page));
  if(fw_port_flash_read(slot0_pg, boot_scratch_buf + sizeof(page), FW_FLASH_PAGE_SIZE) != 0)
  {
    return -1;
  }
  return fw_kv_set(&fw_boot_kv, FW_KV_KEY_BOOT_SCRATCH, boot_scratch_buf, sizeof(boot_scratch_buf)) == FW_KV_OK ? 0 : -1;
}

/**
* @brief   从暂存KV读取指定页的数据到boot_page_buf
* @param   expect_page  期望的页索引
* @return  0/-1
*/
static int fw_swap_scratch_kv_load(uint32_t expect_page)
{
  uint32_t len = sizeof(boot_scratch_buf);
  uint32_t page = 0;
  if(fw_kv_get(&fw_boot_kv, FW_KV_KEY_BOOT_SCRATCH, boot_scratch_buf, &len) != FW_KV_OK || len != sizeof(boot_scratch_buf))
  {
    return -1;
  }
  memcpy(&page, boot_scratch_buf, sizeof(page));
  if(page != expect_page)
  {
    return -1;
  }
  memcpy(boot_page_buf, boot_scratch_buf + sizeof(page), FW_FLASH_PAGE_SIZE);
  return 0;
}

/**
* @brief   拷贝SLOT0页到暂存KV
*/
static int fw_swap_copy_slot0_to_scratch(uint32_t page)
{
  return fw_swap_scratch_kv_save(page);
}

/**
* @brief   拷贝暂存KV到SLOT1页
*/
static int fw_swap_copy_scratch_to_slot1(uint32_t page)
{
  uint32_t slot1_pg = SLOT1_ADDR + page * FW_FLASH_PAGE_SIZE;
  if(fw_swap_scratch_kv_load(page) != 0)
  {
    return -1;
  }
  if(fw_port_flash_erase(slot1_pg, FW_FLASH_PAGE_SIZE) != 0)
  {
    return -1;
  }
  return fw_port_flash_write(slot1_pg, boot_page_buf, FW_FLASH_PAGE_SIZE);
}

/**
* @brief   比较缓冲区与Flash地址处的整页数据是否一致
* @param   buf   缓冲区（一页字节）
* @param   addr  Flash地址
* @return  1=相同，0=不同或读取失败
*/
static int fw_swap_buf_page_equal(const uint8_t *buf, uint32_t addr)
{
  uint32_t w1, w2;
  for(uint32_t off = 0; off < FW_FLASH_PAGE_SIZE; off += sizeof(uint32_t))
  {
    memcpy(&w1, buf + off, sizeof(w1));
    if(fw_port_flash_read(addr + off, (uint8_t *)&w2, sizeof(w2)) != 0)
    {
      return 0;
    }
    if(w1 != w2)
    {
      return 0;
    }
  }
  return 1;
}

/**
* @brief   比较两个Flash地址处的整页数据是否完全一致
*/
static int fw_swap_page_equal(uint32_t a1, uint32_t a2)
{
  for(uint32_t off = 0; off < FW_FLASH_PAGE_SIZE; off += sizeof(uint32_t))
  {
    uint32_t w1, w2;
    if(fw_port_flash_read(a1 + off, (uint8_t *)&w1, sizeof(w1)) != 0)
    {
      return 0;
    }
    if(fw_port_flash_read(a2 + off, (uint8_t *)&w2, sizeof(w2)) != 0)
    {
      return 0;
    }
    if(w1 != w2)
    {
      return 0;
    }
  }
  return 1;
}

/**
* @brief   判断Flash地址处整页是否处于擦除态
* @param   addr  Flash地址
* @return  1=整页擦除态，0=存在已编程数据或读取失败
* @note    擦除态取值随器件而异，这里按FW_ERASED_VALUE构造比较基准，
*          不假定一定是全0xFF。
*/
static int fw_swap_page_is_erased(uint32_t addr)
{
  uint32_t word;
  uint32_t erased = ((uint32_t)FW_ERASED_VALUE << 24) | ((uint32_t)FW_ERASED_VALUE << 16) |
           ((uint32_t)FW_ERASED_VALUE << 8) | (uint32_t)FW_ERASED_VALUE;

  for(uint32_t off = 0; off < FW_FLASH_PAGE_SIZE; off += sizeof(uint32_t))
  {
    if(fw_port_flash_read(addr + off, (uint8_t *)&word, sizeof(word)) != 0)
    {
      return 0;
    }
    if(word != erased)
    {
      return 0;
    }
  }
  return 1;
}

/**
* @brief   拷贝暂存KV到SLOT0页
* @param   page  页号
* @return   0成功，-1失败
* @note    用于交换停在"SLOT0已擦除、新数据未写入"时的回退：把旧固件写回SLOT0，
*          避免后续把空的SLOT0当成旧固件来源。
*/
static int fw_swap_copy_scratch_to_slot0(uint32_t page)
{
  uint32_t slot0_pg = SLOT0_ADDR + page * FW_FLASH_PAGE_SIZE;
  if(fw_swap_scratch_kv_load(page) != 0)
  {
    return -1;
  }
  if(fw_port_flash_erase(slot0_pg, FW_FLASH_PAGE_SIZE) != 0)
  {
    return -1;
  }
  return fw_port_flash_write(slot0_pg, boot_page_buf, FW_FLASH_PAGE_SIZE);
}

/**
* @brief   使用Scratch区恢复中断的交换操作
* @return  0表示恢复成功
* @note    progress是已完成页数，从page=progress继续。每页靠"暂存内容 + 整页比较"
*          判断中断停在哪一步，再决定整页重做还是只补最后一步。
*/
static int fw_swap_recover(void)
{
  uint32_t progress = fw_swap_progress_read();
  if(progress == FW_SWAP_PROGRESS_NONE || progress == FW_SWAP_PROGRESS_COMPLETE)
  {
    return 0;
  }
  uint32_t num_pages = fw_swap_num_pages();
  for(uint32_t page = progress; page < num_pages; page++)
  {
    fw_msg_update_progress(page + 1u, num_pages);
    uint32_t slot0_pg = SLOT0_ADDR + page * FW_FLASH_PAGE_SIZE;
    uint32_t slot1_pg = SLOT1_ADDR + page * FW_FLASH_PAGE_SIZE;
    int do_full = 0;//是否需要执行完整交换
    int do_finish = 0;//是否只需完成Scratch -> SLOT1
    //读取暂存：若暂存属于当前page，boot_page_buf即为暂存的页数据
    int scratch_ok = (fw_swap_scratch_kv_load(page));
    if(!scratch_ok)
    {
      //暂存为空或属于其他页：当前页尚未开始
      do_full = 1;
    }
    else if(fw_swap_buf_page_equal(boot_page_buf, slot0_pg))
    {
      //当前页第1步后：SLOT0尚未被覆盖，重做完整交换
      do_full = 1;
    }
    else if(fw_swap_buf_page_equal(boot_page_buf, slot1_pg))
    {
      //当前页第3步后：已完成交换，无需重做
    }
    else if(fw_swap_page_is_erased(slot0_pg))
    {
      /*当前页第2步擦除后、新数据写入前掉电：SLOT0是空的，旧固件只存在于暂存里。
        必须先写回SLOT0，否则下面的完整交换会把空的SLOT0当成旧固件来源，
        把唯一的旧固件备份覆盖掉。*/
      if(fw_swap_copy_scratch_to_slot0(page) != 0)
      {
        return -1;
      }
      do_full = 1;
    }
    else if(fw_swap_page_equal(slot0_pg, slot1_pg))
    {
      //当前页第2步后：SLOT0已写入新数据，SLOT1尚未写入
      do_finish = 1;
    }
    else
    {
      //异常情况：重做完整交换
      do_full = 1;
    }
    if(do_full)
    {
      if(fw_swap_copy_slot0_to_scratch(page) != 0)
      {
        return -1;
      }
      if(fw_swap_copy_src_to_dst(slot1_pg, slot0_pg) != 0)
      {
        return -1;
      }
      if(fw_swap_copy_scratch_to_slot1(page) != 0)
      {
        return -1;
      }
    }
    else if(do_finish)
    {
      if(fw_swap_copy_scratch_to_slot1(page) != 0)
      {
        return -1;
      }
    }
    //每页交换完成后记录进度
    if(fw_swap_progress_write((uint32_t)(page + 1)) != 0)
    {
      return -1;
    }
  }
  //恢复完成，设置COMPLETE标志（等待App确认）
  if(fw_swap_progress_write(FW_SWAP_PROGRESS_COMPLETE) != 0)
  {
    return -1;
  }
  return 0;
}

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
* @brief   恢复被中断的交换
* @note    进度采用已完成页数编码(取值1-512)；恢复成功后SLOT0已是新固件，直接跳转
*/
void fw_strategy_recover(void)
{
  uint32_t progress = fw_swap_progress_read();
  if(progress == FW_SWAP_PROGRESS_NONE || progress == FW_SWAP_PROGRESS_COMPLETE)
  {
    return;
  }
  fw_msg_recover_start();
  if(fw_swap_recover() == 0)
  {
    fw_msg_recover_done();
    boot_slot0_ok = fw_boot_image_is_valid(SLOT0_ADDR);
    if(boot_slot0_ok)
    {
      //恢复完成，SLOT0已为新固件，直接跳转(跳过阶段2/3)
      fw_boot_jump_to_app(SLOT0_ADDR);
    }
  }
  else
  {
    //恢复失败，放弃本次升级
    fw_msg_error(FW_MSG_ERR_RECOVER, 0);
    fw_boot_flag_write(FW_BOOT_FLAG_NONE);
  }
}

/**
* @brief   处理强制回滚标志
* @note    交换模式回滚：重新执行交换恢复旧固件
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
    //回滚完成，置为已确认；写失败会导致下次启动重复回滚，记一条错误便于定位
    if(fw_boot_flag_write(FW_BOOT_FLAG_CONFIRMED) != 0)
    {
      fw_msg_error(FW_MSG_ERR_UPDATE, FW_BOOT_FLAG_CONFIRMED);
    }
    boot_slot0_ok = fw_boot_image_is_valid(SLOT0_ADDR);   //又交换了一轮，两个槽位都要重新校验
    boot_slot1_ok = fw_boot_image_is_valid(SLOT1_ADDR);
    fw_msg_rollback_done();
  }
  else
  {
    fw_msg_error(FW_MSG_ERR_ROLLBACK, 0);
    if(fw_boot_flag_write(FW_BOOT_FLAG_NONE) != 0)
    {
      fw_msg_error(FW_MSG_ERR_UPDATE, FW_BOOT_FLAG_NONE);
    }
  }
  boot_cur_flag = fw_boot_flag_read();
}

/**
* @brief   处理升级标志
* @note    仅当进度为NONE时才执行新交换；进度为COMPLETE时说明上次启动已执行过，
*          留给strategy_unconfirmed()处理
*/
void fw_strategy_upgrade(void)
{
  if(boot_cur_flag != FW_BOOT_FLAG_VALID)
  {
    return;
  }
  if(fw_swap_progress_read() == FW_SWAP_PROGRESS_NONE)
  {
    if(boot_slot1_ok)
    {
      if(fw_strategy_perform() == 0)
      {
        //升级成功(进度已设为COMPLETE)，直接跳转到新固件
        boot_slot0_ok = fw_boot_image_is_valid(SLOT0_ADDR);
        if(boot_slot0_ok)
        {
          fw_boot_jump_to_app(SLOT0_ADDR);
        }
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
* @note    交换已完成而App没有确认，说明新固件启动失败，回滚到旧固件
*/
void fw_strategy_unconfirmed(void)
{
  if(boot_cur_flag != FW_BOOT_FLAG_VALID || fw_swap_progress_read() != FW_SWAP_PROGRESS_COMPLETE)
  {
    return;
  }
  fw_msg_rollback_start();
  if(boot_slot1_ok)   //对侧槽位还有效，才回滚得回去
  {
    if(fw_strategy_rollback() == 0)
    {
      fw_msg_rollback_done();
      fw_boot_flag_write(FW_BOOT_FLAG_NONE);
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
* @brief   执行升级(交换模式)
* @return  0表示升级成功
*/
int fw_strategy_perform(void)
{
  uint32_t num_pages = fw_swap_num_pages();
  fw_msg_update_start(num_pages);
  for(uint32_t page = 0; page < num_pages; page++)
  {
    uint32_t slot0_pg = SLOT0_ADDR + page * FW_FLASH_PAGE_SIZE;
    uint32_t slot1_pg = SLOT1_ADDR + page * FW_FLASH_PAGE_SIZE;
    //备份SLOT0到Scratch区
    if(fw_swap_copy_slot0_to_scratch(page) != 0)
    {
      return -1;
    }
    //复制SLOT1到SLOT0
    if(fw_swap_copy_src_to_dst(slot1_pg, slot0_pg) != 0)
    {
      return -1;
    }
    //复制Scratch到SLOT1
    if(fw_swap_copy_scratch_to_slot1(page) != 0)
    {
      return -1;
    }
    //交换完成后才记录进度：progress = page+1 表示 page 0..page 已全部完成
    if(fw_swap_progress_write((uint32_t)(page + 1)) != 0)
    {
      return -1;
    }
    fw_msg_update_progress(page + 1u, num_pages);  //与覆写模式一致，按已完成页数输出进度
  }
  //所有页面交换完成，设置COMPLETE标志（等待App确认）
  if(fw_swap_progress_write(FW_SWAP_PROGRESS_COMPLETE) != 0)
  {
    return -1;
  }
  fw_msg_update_done();
  return 0;
}

/**
* @brief   执行回滚(交换模式)
* @return  0表示回滚成功
*/
int fw_strategy_rollback(void)
{
  //交换模式：再次执行升级即可回滚
  {
    int ret = fw_strategy_perform();
    if(ret == 0)
    {
      /*回滚完成后重置进度为NONE（清除COMPLETE状态）。这一步写失败时必须上报：
        残留的COMPLETE标志会让下次启动误判交换未完成并重复执行恢复流程*/
      if(fw_swap_progress_write(FW_SWAP_PROGRESS_NONE) != 0)
      {
        return -1;
      }
    }
    return ret;
  }
}

#endif /* FW_BOOT_BUILD_LOADER == 0 && FW_BOOT_UPGRADE_MODE == 1 */

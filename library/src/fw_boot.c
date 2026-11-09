/**
* @file    fw_boot.c
* @brief   Bootloader核心实现，支持三种升级模式
* @note    通过fw_boot.h中定义的FW_BOOT_UPGRADE_MODE选择模式
*          模式0 = 覆写模式，不支持回滚
*          模式1 = 交换模式，支持回滚
*          模式2 = 就地执行模式，支持回滚
*/

#include "fw_boot.h"
#include "fw_strategy.h"   //升级策略接口(三种模式各一个文件)
#include <string.h>
#include "fw_msg.h"
#include "fw_kv.h"
#include "fw_img.h"      //fw_img_crc32_verify：跳转前回读代码算CRC32
#include "fw_port.h"      //平台适配层：存储原语/取一帧/发一段/跳转(两种构建都用)
#if (FW_BOOT_BUILD_LOADER == 0)
  #include "fw_iap.h"          //串口升级协议解析(与App同一套)
#endif
//暂存区KV实例(策略文件也直接用fw_kv接口访问它)
fw_kv_t fw_boot_kv;

//一次启动内各阶段共享的状态(策略文件经fw_strategy.h访问)
uint32_t boot_cur_flag;			//当前升级标志
int      boot_slot0_ok;			//SLOT0是否有有效镜像
int      boot_slot1_ok;			//SLOT1是否有有效镜像

/* ==================== 暂存区KV的惰性初始化 ==================== */
//1=暂存区KV已初始化；首次访问时自动初始化，调用方不需要显式初始化
static int boot_kv_ready = 0;

/**
* @brief   确保暂存区KV已初始化(库内部用)
* @return  1=可用，0=不可用
*/
static int fw_boot_kv_ensure(void)
{
  if(boot_kv_ready)
  {
    return 1;
  }
  if(fw_kv_init_by_name(&fw_boot_kv, FW_BOOT_STAGE_NAME) == FW_KV_OK)
  {
    boot_kv_ready = 1;
  }
  return boot_kv_ready;
}

/**
* @brief   初始化boot的暂存区KV
* @return  0/-1
*/
int fw_boot_stage_init(void)
{
  boot_kv_ready = 0;//强制重新初始化
  return fw_boot_kv_ensure() ? 0 : -1;
}

/**
* @brief   读取一个32位元数据，未找到时返回默认值
* @param   key         键
* @param   default_val 默认值
* @return  读到的值
*/
uint32_t fw_boot_kv_read_u32(uint32_t key, uint32_t default_val)
{
  uint32_t val = default_val;
  uint32_t len = sizeof(val);   //入参是缓冲区大小，返回实际长度
  if(!fw_boot_kv_ensure())
  {
    return default_val;
  }
  if(fw_kv_get(&fw_boot_kv, key, (uint8_t *)&val, &len) != FW_KV_OK)
  {
    return default_val;
  }
  return val;
}

/**
* @brief   写入一个32位元数据
* @param   key   键
* @param   value 值
* @return  0/-1
*/
int fw_boot_kv_write_u32(uint32_t key, uint32_t value)
{
  if(!fw_boot_kv_ensure())
  {
    return -1;
  }
  return fw_kv_set(&fw_boot_kv, key, (const uint8_t *)&value, sizeof(value)) == FW_KV_OK ? 0 : -1;
}

/**
* @brief   读取Boot标志
* @return  标志值，读取失败返回FW_BOOT_FLAG_NONE
*/
uint32_t fw_boot_flag_read(void)
{
  return fw_boot_kv_read_u32(FW_KV_KEY_BOOT_FLAG, FW_BOOT_FLAG_NONE);
}

/**
* @brief   写入Boot标志
* @param   flag    要写入的标志值
* @return  0表示成功
*/
int fw_boot_flag_write(uint32_t flag)
{
  return fw_boot_kv_write_u32(FW_KV_KEY_BOOT_FLAG, flag);
}

/**
* @brief   读取进度标志（交换进度/XIP运行槽位）
* @return  进度值
*/
uint32_t fw_boot_progress_read(void)
{
  return fw_boot_kv_read_u32(FW_KV_KEY_BOOT_PROGRESS, FW_ERASED_WORD);
}

/**
* @brief   写入进度标志（交换进度/XIP运行槽位）
* @param   value   要写入的值
* @return  0表示成功
*/
int fw_boot_progress_write(uint32_t value)
{
  return fw_boot_kv_write_u32(FW_KV_KEY_BOOT_PROGRESS, value);
}

/**
* @brief   确认当前运行的镜像有效
* @note    由App正常启动后调用。若App没起来(看门狗复位)，
*          下次Boot检测到VALID未被确认会自动回滚(仅模式1/2)
*/
void fw_boot_set_confirmed(void)
{
  uint32_t flag = fw_boot_flag_read();
#if (FW_BOOT_UPGRADE_MODE == 0)
  //覆写模式：不支持回滚，直接清除VALID标志
  if(flag == FW_BOOT_FLAG_VALID)
  {
    fw_boot_flag_write(FW_BOOT_FLAG_CONFIRMED);
  }
#elif (FW_BOOT_UPGRADE_MODE == 1)
  //交换模式：确认SLOT0有效，清除交换进度标记
  if(flag == FW_BOOT_FLAG_VALID)
  {
    fw_boot_flag_write(FW_BOOT_FLAG_CONFIRMED);
    fw_boot_progress_write(FW_SWAP_PROGRESS_NONE);
  }
#elif (FW_BOOT_UPGRADE_MODE == 2)
  //XIP模式：确认当前槽位有效，清除confirm标志
  if(flag == FW_BOOT_FLAG_VALID)
  {
    fw_boot_flag_write(FW_BOOT_FLAG_CONFIRMED);
    //仅保留槽位号，清除confirm确认位
    fw_boot_progress_write(fw_xip_get_run_slot());
  }
#endif
}

#if (FW_BOOT_BUILD_LOADER == 0)

/**
* @brief   跳转到应用程序执行
* @param   app_addr    应用程序起始地址(向量表地址)
* @note    本函数不返回
*/
void fw_boot_jump_to_app(uint32_t app_addr)
{
  fw_msg_jump(app_addr);
  fw_port_wait_tx_complete();
  fw_port_irq_disable();
  //向量表地址须按VTOR_ALIGN_MASK对齐，不满足则报错并复位重新选择镜像
  if(app_addr & VTOR_ALIGN_MASK)
  {
    fw_msg_error(FW_MSG_ERR_VTOR_ALIGN, app_addr);
    /* 地址不对齐无法安全跳转：清掉升级标志后复位，不能关中断死等，
     * 否则整机只能等看门狗，也失去了重新接收升级包的机会 */
    fw_boot_flag_write(FW_BOOT_FLAG_NONE);
    fw_port_system_reset();
  }
  //关闭时基、重定位向量表、设置栈指针并跳转，不返回
  fw_port_jump_to_app(app_addr);
}

/* ---------------- 覆写/交换共用的启动选择：优先SLOT0 ---------------- */
#if (FW_BOOT_UPGRADE_MODE == 0) || (FW_BOOT_UPGRADE_MODE == 1)
/**
* @brief   选有效镜像跳转(覆写/交换模式：优先SLOT0)
*/
void fw_strategy_select_and_jump(void)
{
  if(boot_slot0_ok)
  {
    /* 跳转前做一次完整校验：把代码区回读并重算CRC32，与镜像元数据登记的值比对。
     * 向量表有效但内容不完整的镜像（覆写中途中断、写入出错）在这里被挡住，
     * 按SLOT0无效处理，交给下面的SLOT1恢复路径。 */
    if(fw_img_crc32_verify(SLOT0_ADDR, SLOT_SIZE, NULL) == FW_IMG_OK)
    {
      fw_boot_jump_to_app(SLOT0_ADDR);
    }
    boot_slot0_ok = 0;
  }
  if(boot_slot1_ok && !boot_slot0_ok)
  {
    /* 只有"固件已提交"才允许用SLOT1恢复SLOT0。
     * 而只看向量表两项时，IAP从偏移0开始发，
     * 第一帧(300字节)就覆盖了整个向量表——所以"收到一帧就断电"的残包
     * 也会被判为有效。整包CRC16通过的凭证是FW_BOOT_FLAG_VALID，
     * 没有它说明SLOT1里可能只是半包，搬过去会让App跑飞。 */
    if(boot_cur_flag != FW_BOOT_FLAG_VALID)
    {
      return;
    }
    //语义为slot0无效、需用slot1恢复；地址不参与输出，避免被误认为slot1的地址
    fw_msg_error(FW_MSG_ERR_SLOT0_BAD, 0);
    if(fw_strategy_perform() == 0)
    {
      //恢复同样要过完整校验：写入返回成功不等于Flash内容正确
      boot_slot0_ok = (fw_img_crc32_verify(SLOT0_ADDR, SLOT_SIZE, NULL) == FW_IMG_OK) ? 1 : 0;
      if(boot_slot0_ok)
      {
        fw_boot_jump_to_app(SLOT0_ADDR);
      }
    }
    else
    {
      fw_msg_error(FW_MSG_ERR_RECOVER, SLOT1_ADDR);
      /* 恢复可能在动到SLOT1之前就失败（KV写失败、目标页擦除失败），此时SLOT1里的
         新镜像仍然完好、下次还能再试；只有确认SLOT1确实不可用才擦掉，
         避免一次瞬态错误把两个槽位一起清空 */
      if(fw_img_meta_verify(SLOT1_ADDR, SLOT_SIZE, NULL) != FW_IMG_OK)
      {
        fw_port_flash_erase(SLOT1_ADDR, SLOT_SIZE);
      }
    }
  }
}
#endif /* FW_BOOT_UPGRADE_MODE == 0 || FW_BOOT_UPGRADE_MODE == 1 */


#if (FW_BOOT_BUILD_LOADER == 0)
/**
* @brief   串口升级的单次轮询(由主循环调用，非阻塞)
* @note    从平台层取一帧，按帧头分发给HEX协议；无法识别的帧静默丢弃，不作应答。
*          固件提交后提示一次，之后的复位由工作人员断电或按复位键完成。
*/
void fw_boot_iap_poll(void)
{
  static int inited = 0;//首次调用做一次性的准备
  static int committed = 0;//固件是否已提交(FW_BOOT_FLAG_VALID已写入)
  //缓冲按平台接收队列容量取，保证一次能把队列取空，不残留半个帧
  static uint8_t frame[FW_PORT_RX_BUF_SIZE];
  uint16_t n = 0;

  if(!inited)
  {
    inited = 1;
    fw_msg_serial_update();
  }
  //取一帧；没有就返回，不阻塞主循环
  if(!fw_port_read_frame(frame, (uint16_t)sizeof(frame), &n))
  {
    return;
  }
  //HEX二进制协议：0x55 0xAA开头；块首的串口噪声字节由解析器自行跳过
  if(n > 0u)
  {
    fw_iap_parse_hex_frame(frame, (uint32_t)n);
  }

  //固件已提交：提示一次，之后等工作人员复位
  if(fw_boot_flag_read() == FW_BOOT_FLAG_VALID)
  {
    if(!committed)
    {
      committed = 1;
      fw_msg_serial_update_done();
    }
  }
  else
  {
    committed = 0;   //标志已清除，下一轮升级重新允许提示
  }
}
#endif /* FW_BOOT_BUILD_LOADER == 0 */

/**
* @brief   Bootloader主处理函数，系统复位后调用
* @note    按固定六步走，内部没有模式分支(每步做什么由策略文件决定)。
*          升级确认机制(仅模式1/2)：OTA写完置VALID → 阶段2执行升级并跳转
*          → App启动后置CONFIRMED → 若App没起来，下次阶段3自动回滚
*/
void fw_boot_handler(void)
{
  //升级全程关闭接收中断，避免串口噪声中断风暴饿死Flash升级主循环
  fw_port_rx_irq_disable();
  //初始化暂存区KV，失败时仍恢复串口接收中断，保留串口升级这条恢复通道
  if(fw_boot_stage_init() != 0)
  {
    fw_msg_error(FW_MSG_ERR_KV_INIT, 0);
    fw_port_rx_irq_enable();
    return;
  }

  //========== 阶段0：读取本次启动的状态 ==========
  fw_strategy_state_read();
  fw_msg_kv_parts();//按分区表逐个打印统计(默认关)

  //========== 阶段0b：恢复上次被中断的升级(只有交换模式会真正做事) ==========
  fw_strategy_recover();

  //========== 阶段1：强制回滚 ==========
  fw_strategy_rollback_flag();

  //========== 阶段2：升级处理 ==========
  fw_strategy_upgrade();

  //========== 阶段3：未升级确认的自动回滚 ==========
  fw_strategy_unconfirmed();

  //========== 阶段4：选择有效镜像并跳转 ==========
  fw_strategy_select_and_jump();

  //========== 阶段5：没有可跳转的镜像 ==========
  fw_msg_error(FW_MSG_ERR_NO_IMAGE, 0);
  fw_boot_flag_write(FW_BOOT_FLAG_NONE);   //清除标志，避免反复重试
  //重新打开接收中断后返回，交给main的主循环处理后续帧
  fw_port_rx_irq_enable();
}

#endif /* FW_BOOT_BUILD_LOADER */

/**
* @file    fw_msg.c
* @brief   对外日志统一实现(bootloader/app共用)
* @note    文案集中在本文件，是否输出由fw_msg.h中对应的FW_MSG_EN_xxx决定。
*          输出统一经fw_port_log_printf()，本模块不访问串口与RCC。
*          KV的gc/error事件可能在Flash擦写期间回调进入，此时只做非阻塞输出，
*          平台层须尽快返回(见fw_port.h调用约定)。
*/

#include "fw_msg.h"
#include "fw_cfg.h"
#include "fw_boot.h"
#include "fw_kv.h"
#include "fw_port.h"    //日志出口与复位原因都走平台适配层，本模块不碰串口/RCC
#include <stdarg.h>     //va_list，fw_msg_app_ready()转发调用方字段
#include <stdio.h>      //snprintf/vsnprintf

//前缀(6字符宽，含分隔空格)与主体名
#if (FW_BOOT_BUILD_LOADER == 1)
  #define FW_MSG_TAG        "[APP] "
  #define FW_MSG_OWNER      "App"
#else
  #define FW_MSG_TAG        "[BL]  "
  #define FW_MSG_OWNER      "BootLoader"
#endif

//升级动作名
#if (FW_BOOT_UPGRADE_MODE == 0)
  #define FW_MSG_UPD_NAME   "overwrite"
#elif (FW_BOOT_UPGRADE_MODE == 1)
  #define FW_MSG_UPD_NAME   "swap"
#else
  #define FW_MSG_UPD_NAME   "xip"
#endif

#define FW_MSG_PROG_STEP    10u     //进度打印步长(%)

//App就绪行中项目字段的最大字节数；整行另含前缀与"Ready: "，受FW_PORT_LOG_LINE_MAX约束
#define FW_MSG_APP_DETAIL_MAX  48

//以下函数仅KV事件打印使用
#if FW_MSG_IS_ON(FW_MSG_EN_KV_GC) || FW_MSG_IS_ON(FW_MSG_EN_KV_ROTATE) || \
    FW_MSG_IS_ON(FW_MSG_EN_KV_ERROR)
  #define FW_MSG_KV_DEBUG_ANY   1
#endif

//进度节流用，仅升级进度打印需要
#if FW_MSG_IS_ON(FW_MSG_EN_UPDATE)
  static uint32_t fw_msg_last_prog = 0xFFFFFFFFu;
#endif

/* ==================== 内部工具(与使用者同开关) ==================== */

#if FW_MSG_IS_ON(FW_MSG_EN_BANNER)
/**
* @brief  __DATE__("Mmm dd yyyy")转成"yyyy-mm-dd"
* @param  out  输出缓冲区(至少11字节)
* @param  cap  缓冲区大小
* @note   手工解析，避免为一行banner拖进scanf库；格式不符则原样输出
*/
static void fw_msg_build_date(char *out, uint32_t cap)
{
  static const char *mon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  const char *date = __DATE__;   //固定"MMM DD YYYY"，个位日前面补空格
  int day;
  int year = 0;
  int idx = -1;

  if(date[3] != ' ' || date[6] != ' ')
  {
    snprintf(out, cap, "%s", __DATE__);
    return;
  }
  day = ((date[4] == ' ') ? 0 : (date[4] - '0') * 10) + (date[5] - '0');
  for(int i = 7; i < 11; i++)
  {
    if(date[i] < '0' || date[i] > '9')
    {
      snprintf(out, cap, "%s", __DATE__);
      return;
    }
    year = year * 10 + (date[i] - '0');
  }
  for(int i = 0; i < 12; i++)
  {
    if(date[0] == mon[i][0] && date[1] == mon[i][1] && date[2] == mon[i][2])
    {
      idx = i;
      break;
    }
  }
  if(idx < 0)
  {
    snprintf(out, cap, "%s", __DATE__);
    return;
  }
  snprintf(out, cap, "%04d-%02d-%02d", year, idx + 1, day);
}
#endif /* FW_MSG_EN_BANNER */

#if FW_MSG_IS_ON(FW_MSG_EN_BOOT_STATE)
/**
* @brief  升级标志的符号名
* @param  flag  标志值(FW_BOOT_FLAG_xxx)
* @return 符号名，未知值返回NULL
*/
static const char *fw_msg_flag_name(uint32_t flag)
{
  switch(flag)
  {
    case FW_BOOT_FLAG_NONE:      return "NONE";
    case FW_BOOT_FLAG_VALID:     return "VALID";
    case FW_BOOT_FLAG_ROLLBACK:  return "ROLLBACK";
    case FW_BOOT_FLAG_CONFIRMED: return "CONFIRMED";
    default:                  return NULL;
  }
}
#endif /* FW_MSG_EN_BOOT_STATE */

#if FW_MSG_IS_ON(FW_MSG_EN_ERROR)
/**
* @brief  错误码的文字描述
* @param  err  错误码(fw_msg_err_t)
* @return 英文描述
*/
static const char *fw_msg_err_text(fw_msg_err_t err)
{
  switch(err)
  {
    case FW_MSG_ERR_NO_IMAGE:   return "no valid image";
    case FW_MSG_ERR_SLOT0_BAD:  return "slot0 invalid, use slot1";
    case FW_MSG_ERR_RECOVER:    return "recover failed";
    case FW_MSG_ERR_KV_INIT:    return "kv init failed";
    case FW_MSG_ERR_VTOR_ALIGN: return "vector table not aligned";
    case FW_MSG_ERR_UPDATE:     return "update failed";
    case FW_MSG_ERR_ROLLBACK:   return "rollback failed";
    case FW_MSG_ERR_SLOT1_BAD:  return "target slot invalid, update skipped";
    default:                    return "unknown";
  }
}
#endif /* FW_MSG_EN_ERROR */

#ifdef FW_MSG_KV_DEBUG_ANY
/**
* @brief  按分区基址查分区名(分区表在fw_cfg.h)
* @param  base  分区基址
* @return 分区名，未登记返回"?"
* @note   分区名已改由fw_kv回调给出，本函数暂无调用者；仅在KV调试开关打开时编译
*/
static const char *fw_msg_part_name(uint32_t base)
{
  for(uint32_t i = 0; i < fw_kv_part_count; i++)
  {
    if(fw_kv_part_table[i].base == base)
    {
      return fw_kv_part_table[i].name;
    }
  }
  return "?";
}
#endif /* FW_MSG_KV_DEBUG_ANY */

/* ==================== 通用 ==================== */

/**
* @brief  取本次复位原因
* @return "POR"/"PIN"/"SW"/"IWDG"/"WWDG"/"LPWR"/"OBL"/"UNKNOWN"
* @note   平台层读取时会清除复位标志，因此本函数只向平台取值一次并缓存，
*         清标志的副作用仅发生一次，fw_msg_boot_state()重复调用时输出的原因仍然正确。
*/
const char *fw_msg_reset_reason(void)
{
  static const char *reason = NULL;   //首次调用向平台取值，之后使用缓存

  if(reason == NULL)
  {
    reason = fw_port_reset_reason_get();
  }
  return reason;
}

/**
* @brief  打印版本横幅(上电首行)
* @param  fw_version  固件版本串，由调用方(工程)提供；传NULL或空串则不输出版本号
* @note   输出如： [BL]  BootLoader V1.0.3 (2026-09-22)；无版本串时为 [BL]  BootLoader (2026-09-22)
*         仅使用编译日期，不使用__TIME__，保证相同源码的编译结果一致
*/
void fw_msg_banner(const char *fw_version)
{
#if FW_MSG_IS_ON(FW_MSG_EN_BANNER)
  char date[16];
  fw_msg_build_date(date, sizeof(date));
  if(fw_version != NULL && fw_version[0] != '\0')
  {
    fw_port_log_printf("%s%s %s (%s)\r\n", FW_MSG_TAG, FW_MSG_OWNER, fw_version, date);
  }
  else
  {
    fw_port_log_printf("%s%s (%s)\r\n", FW_MSG_TAG, FW_MSG_OWNER, date);
  }
#else
  (void)fw_version;
#endif
}

/**
* @brief  打印本次启动判定
* @param  flag      升级标志(FW_BOOT_FLAG_xxx)
* @param  slot0_ok  SLOT0是否有有效镜像
* @param  slot1_ok  SLOT1是否有有效镜像("--"表示无)
* @note   输出如： Boot: rst=POR flag=CONFIRMED slot0=OK slot1=--
*/
void fw_msg_boot_state(uint32_t flag, int slot0_ok, int slot1_ok)
{
#if FW_MSG_IS_ON(FW_MSG_EN_BOOT_STATE)
  const char *name = fw_msg_flag_name(flag);
  const char *s0 = (slot0_ok != 0) ? "OK" : "--";
  const char *s1 = (slot1_ok != 0) ? "OK" : "--";

  if(name == NULL)
  {
    //值不在FW_BOOT_FLAG_xxx表内(Flash内容异常等)：统一显示为UNKNOWN
    name = "UNKNOWN";
  }
  fw_port_log_printf("%sBoot: rst=%s flag=%s slot0=%s slot1=%s\r\n",
			FW_MSG_TAG, fw_msg_reset_reason(), name, s0, s1);
#else
  (void)flag;
  (void)slot0_ok;
  (void)slot1_ok;
#endif
}

/**
* @brief  打印错误提示
* @param  err     错误码(fw_msg_err_t)
* @param  detail  附加信息(如地址)，为0时省略
* @note   输出如： Err 0x02 slot0 invalid, use slot1
*/
void fw_msg_error(fw_msg_err_t err, uint32_t detail)
{
#if FW_MSG_IS_ON(FW_MSG_EN_ERROR)
  if(detail != 0u)
  {
    fw_port_log_printf("%sErr 0x%02X %s (0x%08X)\r\n", FW_MSG_TAG, (unsigned)err, fw_msg_err_text(err), (unsigned)detail);
  }
  else
  {
    fw_port_log_printf("%sErr 0x%02X %s\r\n", FW_MSG_TAG, (unsigned)err, fw_msg_err_text(err));
  }
#else
  (void)err;
  (void)detail;
#endif
}

/* ==================== BootLoader流程信息 ==================== */

/**
* @brief  打印已跳转到App
* @param  app_addr  跳转目标地址(当前不参与输出)
* @note   输出如： Jump to App
*/
void fw_msg_jump(uint32_t app_addr)
{
  (void)app_addr;
#if FW_MSG_IS_ON(FW_MSG_EN_JUMP)
  fw_port_log_printf("%sJump to App\r\n", FW_MSG_TAG);
#endif
}

/**
* @brief  打印升级动作开始
* @param  pages  本次要搬运的页数
* @note   输出如： Update(overwrite) start: 512 pages
*/
void fw_msg_update_start(uint32_t pages)
{
#if FW_MSG_IS_ON(FW_MSG_EN_UPDATE)
  fw_msg_last_prog = 0xFFFFFFFFu;   //新一轮升级，进度从0%重新输出
  fw_port_log_printf("%sUpdate(%s) start: %u pages\r\n", FW_MSG_TAG, FW_MSG_UPD_NAME, (unsigned)pages);
#else
  (void)pages;
#endif
}

/**
* @brief  打印升级进度
* @param  page   已完成的页数
* @param  pages  总页数
* @note   按10%一档节流，输出如： Update(overwrite) 40%
*/
void fw_msg_update_progress(uint32_t page, uint32_t pages)
{
#if FW_MSG_IS_ON(FW_MSG_EN_UPDATE)
  uint32_t percent;

  if(pages == 0u)
  {
    return;
  }
  //page*100的回绕界限约4295万页，远超本库任何分区，直接使用乘法求百分比
  percent = (page * 100u) / pages;
  percent -= percent % FW_MSG_PROG_STEP;
  if(percent == fw_msg_last_prog)
  {
    return;
  }
  fw_msg_last_prog = percent;
  fw_port_log_printf("%sUpdate(%s) %u%%\r\n", FW_MSG_TAG, FW_MSG_UPD_NAME, (unsigned)percent);
#else
  (void)page;
  (void)pages;
#endif
}

/**
* @brief  打印升级动作完成
* @note   输出如： Update(overwrite) OK
*/
void fw_msg_update_done(void)
{
#if FW_MSG_IS_ON(FW_MSG_EN_UPDATE)
  fw_port_log_printf("%sUpdate(%s) OK\r\n", FW_MSG_TAG, FW_MSG_UPD_NAME);
#endif
}

/**
* @brief  打印"上次升级被中断，正在恢复"
* @note   输出如： Recover: interrupted update, resuming
*/
void fw_msg_recover_start(void)
{
#if FW_MSG_IS_ON(FW_MSG_EN_RECOVER)
  fw_port_log_printf("%sRecover: interrupted update, resuming\r\n", FW_MSG_TAG);
#endif
}

/**
* @brief  打印恢复完成
* @note   输出如： Recover: OK
*/
void fw_msg_recover_done(void)
{
#if FW_MSG_IS_ON(FW_MSG_EN_RECOVER)
  fw_port_log_printf("%sRecover: OK\r\n", FW_MSG_TAG);
#endif
}

/**
* @brief  打印"未确认升级，开始回滚"
* @note   输出如： Rollback: unconfirmed update, restore previous image
*/
void fw_msg_rollback_start(void)
{
#if FW_MSG_IS_ON(FW_MSG_EN_ROLLBACK)
  fw_port_log_printf("%sRollback: unconfirmed update, restore previous image\r\n", FW_MSG_TAG);
#endif
}

/**
* @brief  打印回滚完成
* @note   输出如： Rollback: OK
*/
void fw_msg_rollback_done(void)
{
#if FW_MSG_IS_ON(FW_MSG_EN_ROLLBACK)
  fw_port_log_printf("%sRollback: OK\r\n", FW_MSG_TAG);
#endif
}

/**
* @brief  打印"没有可用固件，等待重新烧录"
* @note   输出如： Reflash required (SWD/JTAG/UART)
*/
void fw_msg_waiting(void)
{
#if FW_MSG_IS_ON(FW_MSG_EN_WAITING)
  fw_port_log_printf("%sReflash required (SWD/JTAG/UART)\r\n", FW_MSG_TAG);
#endif
}

/**
* @brief  打印"没有可用镜像，进入串口升级等待模式"
* @note   输出如： Serial update: waiting for firmware
*/
void fw_msg_serial_update(void)
{
#if FW_MSG_IS_ON(FW_MSG_EN_WAITING)
  fw_port_log_printf("%sSerial update: waiting for firmware\r\n", FW_MSG_TAG);
#endif
}

/**
* @brief  打印"串口固件已收到，准备复位执行升级"
*/
void fw_msg_serial_update_done(void)
{
#if FW_MSG_IS_ON(FW_MSG_EN_WAITING)
  fw_port_log_printf("%sSerial update: firmware received, reset to upgrade\r\n", FW_MSG_TAG);
#endif
}
/* ==================== App流程信息 ==================== */

/**
* @brief  打印App启动就绪
* @param  fmt  附加字段的格式串，传NULL或空串则只打"Ready"
* @param  ...  格式参数
* @note   库里不认识"波特率""标定点数"这类项目专有字段，字段名与顺序由调用方决定：
*         例如 fw_msg_app_ready("baud=%u cal=%u", baud, cal)
*         输出如： Ready: baud=115200 cal=2
*/
void fw_msg_app_ready(const char *fmt, ...)
{
#if FW_MSG_IS_ON(FW_MSG_EN_APP_READY)
  char detail[FW_MSG_APP_DETAIL_MAX];
  va_list ap;

  if(fmt == NULL || fmt[0] == '\0')
  {
    fw_port_log_printf("%sReady\r\n", FW_MSG_TAG);
    return;
  }
  //先将调用方字段拼为一段文本，再与前缀一次输出，保证整行原子
  va_start(ap, fmt);
  vsnprintf(detail, sizeof(detail), fmt, ap);
  va_end(ap);
  fw_port_log_printf("%sReady: %s\r\n", FW_MSG_TAG, detail);
#else
  (void)fmt;
#endif
}

/* ==================== 细节调试 ==================== */

/**
* @brief  按分区表逐个打印各分区统计
* @note   分区取自fw_cfg.h的FW_KV_PART_TABLE，分区数量随表变化；统计为只读操作，
*         临时实例经fw_kv_init()扫描，不写Flash
*/
void fw_msg_kv_parts(void)
{
#if FW_MSG_IS_ON(FW_MSG_EN_KV_PARTS)
  for(uint32_t i = 0; i < fw_kv_part_count; i++)
  {
    const fw_kv_part_t *part = &fw_kv_part_table[i];
    fw_kv_stats_t st;
    fw_kv_t kv;

    if(fw_kv_init(&kv, part) != FW_KV_OK)
    {
      fw_port_log_printf("%sKV(%s) init failed\r\n", FW_MSG_TAG, part->name);
      continue;
    }
    if(fw_kv_get_stats(&kv, &st) != FW_KV_OK)
    {
      continue;
    }
    fw_port_log_printf("%sKV(%s) sectors=%u active=%d full=%u empty=%u dirty=%u\r\n", FW_MSG_TAG,
        part->name, (unsigned)st.sector_count, (int)st.active_sector,
        (unsigned)st.full_sectors, (unsigned)st.empty_sectors, (unsigned)st.dirty_sectors);
    fw_port_log_printf("%sKV(%s) used=%u live=%u garbage=%u\r\n", FW_MSG_TAG,
        part->name, (unsigned)st.used_bytes, (unsigned)st.live_bytes,
        (unsigned)st.garbage_bytes);
  }
#endif
}

/**
* @brief  打印一次KV垃圾回收
* @param  part_name   分区名(fw_kv回调直接给出)
* @param  from_sec     被回收的扇区号
* @param  to_sec       搬迁目标扇区号
* @param  used_bytes   搬迁后目标扇区已用字节
* @note   输出如： KV(EEPROM) gc: sector 3 -> 5, used=40B
*         本函数在GC过程中调用，此时可能正在擦写Flash，输出须尽快返回
*/
void fw_msg_kv_gc(const char *part_name, int from_sec, int to_sec, uint32_t used_bytes)
{
#if FW_MSG_IS_ON(FW_MSG_EN_KV_GC)
  fw_port_log_printf("%sKV(%s) gc: sector %d -> %d, used=%uB\r\n", FW_MSG_TAG,
      (part_name != NULL) ? part_name : "?", from_sec, to_sec, (unsigned)used_bytes);
#else
  (void)part_name;
  (void)from_sec;
  (void)to_sec;
  (void)used_bytes;
#endif
}

/**
* @brief  打印KV扇区轮转
* @param  part_name   分区名(fw_kv回调直接给出)
* @param  sector   新启用的扇区号
* @param  seq      新扇区序号
* @note   输出如： KV(EEPROM) rotate: sector=7 seq=42
*         本函数在擦写过程中调用，输出须尽快返回
*/
void fw_msg_kv_rotate(const char *part_name, int sector, uint32_t seq)
{
#if FW_MSG_IS_ON(FW_MSG_EN_KV_ROTATE)
  fw_port_log_printf("%sKV(%s) rotate: sector=%d seq=%u\r\n", FW_MSG_TAG,
      (part_name != NULL) ? part_name : "?", sector, (unsigned)seq);
#else
  (void)part_name;
  (void)sector;
  (void)seq;
#endif
}

/**
* @brief  打印KV读写失败
* @param  part_name   分区名(fw_kv回调直接给出)
* @param  rc   fw_kv返回码
* @note   输出如： KV(EEPROM) error rc=-2
*         本函数多在擦写失败时调用，须尽快返回，避免延长擦写窗口
*/
void fw_msg_kv_error(const char *part_name, int rc)
{
#if FW_MSG_IS_ON(FW_MSG_EN_KV_ERROR)
  fw_port_log_printf("%sKV(%s) error rc=%d\r\n", FW_MSG_TAG,
      (part_name != NULL) ? part_name : "?", rc);
#else
  (void)part_name;
  (void)rc;
#endif
}

/**
* @brief  fw_kv日志事件适配器
* @param  evt  事件类型(见fw_kv.h的fw_kv_evt_t)
* @param  kv   KV实例
* @param  a    事件参数1
* @param  b    事件参数2
* @param  c    事件参数3
* @note   把库报出的事件转成本文件上面的fw_msg_kv_*输出，
*         各事件的参数含义见fw_kv.h的fw_kv_log_fn_t
*/
static void fw_msg_kv_log_adapter(fw_kv_evt_t evt, const fw_kv_t *kv, const char *part_name,
    int a, int b, uint32_t c)
{
  //分区名由回调单独给出，此处不解引用kv，保持与KV内部结构解耦
  (void)kv;

  switch(evt)
  {
    case FW_KV_EVT_GC:      //回收：a=源扇区，b=目标扇区，c=目标已用字节
      fw_msg_kv_gc(part_name, a, b, c);
      break;
    case FW_KV_EVT_ROTATE:  //轮转：a=扇区号，c=扇区序号
      fw_msg_kv_rotate(part_name, a, c);
      break;
    default:                //错误：a=返回码
      fw_msg_kv_error(part_name, a);
      break;
  }
}

/**
* @brief  把fw_kv的日志事件接到本模块的输出上
* @note   库不依赖任何日志模块，注册前它的日志调用都是空操作；
*         在kv初始化后调用一次即可(重复调用无副作用)
*/
void fw_msg_kv_log_attach(void)
{
  fw_kv_set_log(fw_msg_kv_log_adapter);
}

/**
* @brief  打印IAP会话开始
* @param  total  本次镜像总字节数
* @note   输出如： IAP: start total=40452B
*/
void fw_msg_iap_start(uint32_t total)
{
#if FW_MSG_IS_ON(FW_MSG_EN_IAP_START)
  fw_port_log_printf("%sIAP: start total=%uB\r\n", FW_MSG_TAG, (unsigned)total);
#else
  (void)total;
#endif
}

/**
* @brief  打印IAP整包提交成功(已写入FW_BOOT_FLAG_VALID)
* @param  total  本次镜像总字节数
* @note   输出如： IAP: done total=40452B, boot flag set
*/
void fw_msg_iap_done(uint32_t total)
{
#if FW_MSG_IS_ON(FW_MSG_EN_IAP_DONE)
  fw_port_log_printf("%sIAP: done total=%uB, boot flag set\r\n", FW_MSG_TAG, (unsigned)total);
#else
  (void)total;
#endif
}

/**
* @brief  打印IAP整包CRC不符
* @param  calc_crc  设备累计的整包CRC16
* @param  file_crc  上位机声明的整包CRC16
* @note   输出如： IAP: file crc error calc=0xDF2A file=0x1234
*/
void fw_msg_iap_crc_error(uint16_t calc_crc, uint16_t file_crc)
{
#if FW_MSG_IS_ON(FW_MSG_EN_IAP_CRC)
  fw_port_log_printf("%sIAP: file crc error calc=0x%04X file=0x%04X\r\n", FW_MSG_TAG,
      (unsigned)calc_crc, (unsigned)file_crc);
#else
  (void)calc_crc;
  (void)file_crc;
#endif
}

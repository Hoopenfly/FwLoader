#ifndef _FW_MSG_H_
#define _FW_MSG_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
* 对外日志：bootloader/app的提示文案全部集中在本文件，其它模块只调用
* 下面的函数集中生成文案，调用方不自行拼接字符串。
*   1) 两个工程共用同一份fw_msg.c/h，前缀"[BL] "/"[APP] "由FW_BOOT_BUILD_LOADER决定；
*   2) 全ASCII输出，行格式"<前缀><键=值 ...>"，便于上位机抓取；
*   3) 每一项打印的开关见下面"打印开关"，只改这里即可；
*   4) 输出统一经平台适配层的fw_port_log_printf()，本模块不直接调用平台调试输出；
*      换平台只改fw_port.c，文案与开关不变；单行长度上限见FW_PORT_LOG_LINE_MAX；
*   5) KV的gc/轮转/错误事件可能在Flash擦写期间回调进入，这些函数只做非阻塞输出，
*      平台层单次输出须设等待上限，不得等待整行发送完毕；
*   6) 本模块只提供升级流程通用文案；波特率、标定点数等项目专有字段由调用方
*      以格式串传入(见fw_msg_app_ready)，更换项目无需修改本库。
* ======================================================================== */

/* ==================== 打印开关 ====================
* 每一项：1=打印，0=不打印；FW_MSG_ENABLE=0时全部关闭。
* 关闭项的函数体与字符串都不参与编译，函数符号仍保留，调用方无需条件编译。
*/
#ifndef FW_MSG_ENABLE
  #define FW_MSG_ENABLE          1      //总开关
#endif

/* ---- 启动/运行流程(正常启动各1行) ---- */
#ifndef FW_MSG_EN_BANNER
  #define FW_MSG_EN_BANNER       1      //版本横幅
#endif
#ifndef FW_MSG_EN_BOOT_STATE
  #define FW_MSG_EN_BOOT_STATE   1      //启动判定
#endif
#ifndef FW_MSG_EN_JUMP
  #define FW_MSG_EN_JUMP         1      //跳转地址
#endif
#ifndef FW_MSG_EN_UPDATE
  #define FW_MSG_EN_UPDATE       1      //升级动作(开始/进度/完成)
#endif
#ifndef FW_MSG_EN_RECOVER
  #define FW_MSG_EN_RECOVER      1      //中断的交换恢复
#endif
#ifndef FW_MSG_EN_ROLLBACK
  #define FW_MSG_EN_ROLLBACK     1      //自动回滚
#endif
#ifndef FW_MSG_EN_WAITING
  #define FW_MSG_EN_WAITING      1      //停在bootloader时的周期提示
#endif
#ifndef FW_MSG_EN_ERROR
  #define FW_MSG_EN_ERROR        1      //错误提示
#endif
#ifndef FW_MSG_EN_APP_READY
  #define FW_MSG_EN_APP_READY    1      //App就绪
#endif

/* ---- 细节调试：与协议帧共用COM1，默认关闭 ---- */
#ifndef FW_MSG_EN_KV_PARTS
  #define FW_MSG_EN_KV_PARTS     0      //各分区统计(按分区表逐个打印)
#endif
#ifndef FW_MSG_EN_KV_GC
  #define FW_MSG_EN_KV_GC        0      //垃圾回收
#endif
#ifndef FW_MSG_EN_KV_ROTATE
  #define FW_MSG_EN_KV_ROTATE    0      //扇区轮转
#endif
#ifndef FW_MSG_EN_KV_ERROR
  #define FW_MSG_EN_KV_ERROR     0      //KV读写失败
#endif
#ifndef FW_MSG_EN_IAP_START
  #define FW_MSG_EN_IAP_START    0      //IAP会话开始
#endif
#ifndef FW_MSG_EN_IAP_DONE
  #define FW_MSG_EN_IAP_DONE     0      //IAP提交成功
#endif
#ifndef FW_MSG_EN_IAP_CRC
  #define FW_MSG_EN_IAP_CRC      0      //IAP整包CRC错
#endif

//总开关与单项同时为真才输出
#define FW_MSG_IS_ON(item)       ((FW_MSG_ENABLE != 0) && ((item) != 0))

//FwLoader库版本：由库维护者随发布更新，不使用#ifndef，避免被使用方无意覆盖
#define FW_LIB_VERSION      "1.0.0"

/* 错误码：日志"err 0xNN <描述>"与该枚举一一对应 */
typedef enum
{
  FW_MSG_ERR_NO_IMAGE     = 0x01,   //两个槽位都无有效镜像
  FW_MSG_ERR_SLOT0_BAD    = 0x02,   //SLOT0无效，尝试用SLOT1恢复
  FW_MSG_ERR_RECOVER      = 0x03,   //中断的交换恢复失败
  FW_MSG_ERR_KV_INIT      = 0x04,   //暂存区初始化失败
  FW_MSG_ERR_VTOR_ALIGN   = 0x05,   //向量表地址未对齐
  FW_MSG_ERR_UPDATE       = 0x06,   //升级动作失败
  FW_MSG_ERR_ROLLBACK     = 0x07,   //回滚失败
  FW_MSG_ERR_SLOT1_BAD    = 0x08    //升级目标槽位无有效镜像
}fw_msg_err_t;

/* ---------------- 通用 ---------------- */
//版本横幅：版本串由调用方(工程)提供，库不维护固件版本；传NULL或空串则只输出版本名与编译日期
void fw_msg_banner(const char *fw_version);     //开关FW_MSG_EN_BANNER
void fw_msg_error(fw_msg_err_t err, uint32_t detail);  //错误，开关FW_MSG_EN_ERROR
const char *fw_msg_reset_reason(void);          //复位原因字符串，不打印

/* ---------------- BootLoader ---------------- */
//启动判定：复位原因+升级标志+两槽位是否有有效镜像("--"=无)，开关FW_MSG_EN_BOOT_STATE
void fw_msg_boot_state(uint32_t flag, int slot0_ok, int slot1_ok);
void fw_msg_jump(uint32_t app_addr);            //已跳转到App，开关FW_MSG_EN_JUMP
//升级动作：开始(页数)/进度(10%一档)/完成，开关FW_MSG_EN_UPDATE
void fw_msg_update_start(uint32_t pages);
void fw_msg_update_progress(uint32_t page, uint32_t pages);
void fw_msg_update_done(void);
//中断的交换恢复，开关FW_MSG_EN_RECOVER
void fw_msg_recover_start(void);
void fw_msg_recover_done(void);
//未确认升级的自动回滚，开关FW_MSG_EN_ROLLBACK
void fw_msg_rollback_start(void);
void fw_msg_rollback_done(void);
void fw_msg_waiting(void);                      //停在bootloader的周期提示(仅未启用IAP时用)
//没有可用镜像，进入串口升级等待模式，开关FW_MSG_EN_WAITING
void fw_msg_serial_update(void);
//串口固件已收到，准备复位执行升级，开关FW_MSG_EN_WAITING
void fw_msg_serial_update_done(void);

/* ---------------- App ---------------- */
//启动就绪，开关FW_MSG_EN_APP_READY。库只输出"Ready"，项目专有字段由调用方给格式串，
//例如 fw_msg_app_ready("baud=%u cal=%u", baud, cal)；传NULL或空串则只输出"Ready"
void fw_msg_app_ready(const char *fmt, ...);

/* ---------------- 细节调试 ---------------- */
struct fw_kv;                                   //fw_kv.h的KV实例类型前向声明(日志回调签名使用)
//把fw_kv的事件(GC/轮转/错误)接入本模块输出
//在main()中调用一次，且早于任何fw_kv_init_by_name
void fw_msg_kv_log_attach(void);
//按FW_KV_PART_TABLE逐个打印分区统计(分区数量随表变化，换芯片无需改动)
void fw_msg_kv_parts(void);   //开关FW_MSG_EN_KV_PARTS
//分区名由fw_kv回调直接给出，本模块不访问KV实例字段(见fw_kv.h的fw_kv_log_fn_t)
void fw_msg_kv_gc(const char *part_name, int from_sec, int to_sec, uint32_t used_bytes); //开关FW_MSG_EN_KV_GC
void fw_msg_kv_rotate(const char *part_name, int sector, uint32_t seq);   //开关FW_MSG_EN_KV_ROTATE
void fw_msg_kv_error(const char *part_name, int rc);   //开关FW_MSG_EN_KV_ERROR
void fw_msg_iap_start(uint32_t total);   //开关FW_MSG_EN_IAP_START
void fw_msg_iap_done(uint32_t total);   //开关FW_MSG_EN_IAP_DONE
void fw_msg_iap_crc_error(uint16_t calc_crc, uint16_t file_crc);   //开关FW_MSG_EN_IAP_CRC
#ifdef __cplusplus
}
#endif

#endif /* _FW_MSG_H_ */

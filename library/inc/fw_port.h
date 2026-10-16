/**
* @file    fw_port.h
* @brief   平台适配层：串口动作、日志输出、复位原因
* @note    移植新平台时按本头文件重写fw_port.c。本层只提供硬件动作，
*          不含升级协议与业务逻辑(见fw_boot.c)。
*
* ==================== 调用约定(平台层实现必须满足) ====================
* 1) fw_port_log_printf()/fw_port_log_vprintf()不受库内开关影响，始终可用；
*    对外日志依赖它输出升级流程；平台后端自身的调试开关(如本平台的DBG_ENABLE)
*    不得在本层之上把输出编译掉；
* 2) 单次调用输出完整一行，实现须保证整行原子，不得被其它任务或中断的输出插入；
*    至少支持 %s %u %d %X %c %% 及宽度(如%08X)；
* 3) 单行长度不超过FW_PORT_LOG_LINE_MAX字节(含\r\n)，超长由实现截断；
* 4) 输出不得无限阻塞：升级流程会在Flash擦写前后打印，单次等待须设上限(建议10ms以内)；
* 5) fw_port_reset_reason_get()读取后清除复位标志，一次上电仅调用一次；
*    多次使用请在更上层缓存(见fw_msg_reset_reason)。
*/

#ifndef _FW_PORT_H_
#define _FW_PORT_H_

#include <stdarg.h>
#include "fw_cfg.h"  //平台能力：页大小、写粒度、接收缓冲
#include <stdint.h>

/**
* @brief   等串口把已排队的数据发完
* @note    轮询发送空标志，发完即返回，最长等待1秒。App与BootLoader两种构建均可用。
*/
void fw_port_wait_tx_complete(void);

/**
* @brief   升级期间关闭串口接收中断
* @note    擦写Flash期间不接收数据；接收中断若保持开启，RS485总线空闲噪声会累积为
*          ORE溢出，恢复中断后引发中断风暴。
*/
void fw_port_rx_irq_disable(void);

/**
* @brief   重新打开串口接收中断
*/
void fw_port_rx_irq_enable(void);

/**
* @brief   取一帧收到的数据(非阻塞)
* @param   buf     接收缓冲区
* @param   maxlen  缓冲区大小
* @param   len     输出：实际取到的字节数
* @return  1=取到数据，0=暂时没有
* @note    仅在串口空闲(整帧收完)后取数据：整帧最多数百字节，需数十毫秒收齐；
*          边收边取会把半帧当作整帧，导致后续字节错位。
*          由中断把字节搬进软件FIFO，此处只负责取走；不要改成轮询接收寄存器，
*          LPUART只有一字节硬件缓冲，主循环一旦不在轮询就会丢字节。
*/
int fw_port_read_frame(uint8_t *buf, uint16_t maxlen, uint16_t *len);

/**
* @brief   发送一段数据，等待发送完成
* @param   buf     数据
* @param   len     长度
* @note    发送完成才返回，避免调用方紧接复位时截断应答。
*/
void fw_port_send_buf(const uint8_t *buf, uint16_t len);

/* ==================== 非易失存储原语(平台层实现) ==================== */
/* 库不引用平台头文件，读/写/擦都经这三个原语；返回值 0 表示成功。
 * 页大小与写粒度由 fw_cfg.h 声明，库据此切分操作。 */

/**
* @brief   读主Flash
* @param   addr  起始地址
* @param   buf   数据缓冲区
* @param   size  读取字节数
* @return  0成功，非0失败
*/
int fw_port_flash_read(uint32_t addr, uint8_t *buf, uint32_t size);

/**
* @brief   写主Flash
* @param   addr  起始地址
* @param   buf   数据缓冲区
* @param   size  写入字节数
* @return  0成功，非0失败
*/
int fw_port_flash_write(uint32_t addr, const uint8_t *buf, uint32_t size);

/**
* @brief   擦主Flash
* @param   addr  起始地址(页对齐)
* @param   size  擦除字节数(页对齐)
* @return  0成功，非0失败
*/
int fw_port_flash_erase(uint32_t addr, uint32_t size);

/**
* @brief   读数据EEPROM
* @return  0成功，非0失败
*/
int fw_port_eeprom_read(uint32_t addr, uint8_t *buf, uint32_t size);

/**
* @brief   写数据EEPROM
* @return  0成功，非0失败
*/
int fw_port_eeprom_write(uint32_t addr, const uint8_t *buf, uint32_t size);

/**
* @brief   擦数据EEPROM
* @return  0成功，非0失败
*/
int fw_port_eeprom_erase(uint32_t addr, uint32_t size);

/* ==================== 跳转(平台层实现) ==================== */

/**
* @brief   跳转到指定向量表的应用程序，不返回
* @param   app_addr  应用起始地址(即其向量表地址)
* @note    实现须关闭平台已启动的时基(如SysTick)、把向量表重定位到该地址、
*          设置主栈指针后跳转；本函数不返回。
*/
void fw_port_jump_to_app(uint32_t app_addr);

/* ==================== 中断开关(平台层实现) ==================== */
/* 库内的临界区与跳转前关中断都经这两个接口，不直接用内核指令。 */

/**
* @brief   关中断
*/
void fw_port_irq_disable(void);

/**
* @brief   开中断
*/
void fw_port_irq_enable(void);

/* ==================== 日志与复位原因(App与BootLoader都要用) ==================== */

/* 一行日志的最大字节数(含结尾的\r\n)，即平台层行缓冲大小。
 * 超长行由实现截断，并在截断处补\r\n，保证上位机可按行切分；新增文案不得超出此上限 */
#ifndef FW_PORT_LOG_LINE_MAX
  #define FW_PORT_LOG_LINE_MAX  64
#endif

/**
* @brief   输出一行日志(平台层实现，约定见文件头)
* @param   fmt  格式串(编译期字面量)
* @param   ...  格式参数
* @note    库的对外日志均由此输出，不直接调用平台调试输出函数；
*          换平台只改fw_port.c，日志文案与开关见fw_msg。
*/
void fw_port_log_printf(const char *fmt, ...);

/**
* @brief   输出一行日志(变参版)
* @param   fmt  格式串(编译期字面量)
* @param   ap   已用va_start初始化的参数列表
* @note    与fw_port_log_printf()同一出口与约定。库内需拼接调用方字段的函数
*          (如fw_msg_app_ready())使用本接口，保证单次写出整行。
*/
void fw_port_log_vprintf(const char *fmt, va_list ap);

/* ==================== fw_kv 临界区(平台层实现) ==================== */
/* fw_kv的每个对外接口内部是一串多步操作：读写实例状态，必要时触发扇区轮转与GC。
 * 这串操作必须整体原子，否则并发访问会破坏实例状态与Flash上的一致性。
 * 内核只调用下面两个函数，不关心它是关中断还是互斥锁：
 *   无OS环境：关中断/开中断；
 *   有OS环境：互斥锁的获取/释放——临界区可能包含Flash擦写，不能关中断。
 * 实现放在fw_port.c，两个工程按各自的运行环境给出实现。 */

/**
* @brief   初始化临界区资源(有OS时创建互斥锁)，可重复调用
*/
void fw_port_kv_crit_init(void);

/**
* @brief   进入fw_kv临界区
*/
void fw_port_kv_crit_lock(void);

/**
* @brief   退出fw_kv临界区
*/
void fw_port_kv_crit_unlock(void);

/**
* @brief   取本次复位原因
* @return  静态字符串："POR"/"PIN"/"SW"/"IWDG"/"WWDG"/"LPWR"/"OBL"/"UNKNOWN"
* @note    读取后清除复位标志，一次上电只应调用一次，第二次起返回"UNKNOWN"。
*/
const char *fw_port_reset_reason_get(void);

#endif /* _FW_PORT_H_ */

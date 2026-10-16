/**
* @file    fw_port.c
* @brief   平台适配层实现(当前平台为STM32L0，移植时按fw_port.h重写)
*/

#include "fw_port.h"
#include "usart.h"      //COM1、comGetChar、comSendBuf、UartGetEventFlags、UartTxEmpty
#include "debug.h"      //dbg_printf：平台的调试输出通道
#include "stm32l0xx.h"  //RCC->CSR(复位原因)、NVIC_SystemReset(断言失败)、HAL_GetTick
#include "myflash.h"   //flash_read/flash_write/flash_erase
#include "eeprom.h"    //eeprom_read/eeprom_write/eeprom_erase
#include "fw_iap.h"     //FW_IAP_RX_FRAME_MAX(编译期校验)
#include <stdarg.h>
#include <stdio.h>

#if (FW_BOOT_BUILD_LOADER == 0)

void fw_port_rx_irq_disable(void)
{
  LPUART1->CR1 &= ~(USART_CR1_RXNEIE | USART_CR1_IDLEIE);
}

/**
* @brief   重新打开串口接收中断
*/
void fw_port_rx_irq_enable(void)
{
  LPUART1->CR1 |= (USART_CR1_RXNEIE | USART_CR1_IDLEIE);
}

/**
* @brief   取一帧收到的数据(非阻塞)
* @param   buf     接收缓冲区
* @param   maxlen  缓冲区大小
* @param   len     输出：实际取到的字节数
* @return  1=取到数据，0=暂时没有
* @note    仅在串口空闲(整帧收完)后取数据：整帧最多数百字节，需数十毫秒收齐；
*          边收边取会把半帧当作整帧，导致后续字节错位。
*/
int fw_port_read_frame(uint8_t *buf, uint16_t maxlen, uint16_t *len)
{
  uint16_t n = 0;
  uint8_t byte;

  //空闲中断置位表示整帧已收完
  if((UartGetEventFlags() & (1U << COM1)) == 0U)
  {
    return 0;
  }
  while(n < maxlen && comGetChar(COM1, &byte))
  {
    buf[n++] = byte;
  }
  *len = n;
  return (n > 0U) ? 1 : 0;
}


#endif /* FW_BOOT_BUILD_LOADER == 0 */
/**
* @brief   发送一段数据，等待发送完成
*/
void fw_port_send_buf(const uint8_t *buf, uint16_t len)
{
  uint32_t t0;
  comSendBuf(COM1, buf, len);
  //最长等待100ms，超时返回，不阻塞升级流程
  t0 = HAL_GetTick();
  while(!UartTxEmpty(COM1) && (HAL_GetTick() - t0) < 100u)
  {
  }
}


/* ==================== 发送等待(App与BootLoader都要用) ==================== */

/**
* @brief   等串口把已排队的数据发完
* @note    轮询发送空标志，发完即返回，最长等待1秒。App与BootLoader两种构建均可用。
*/
void fw_port_wait_tx_complete(void)
{
  uint32_t t0 = HAL_GetTick();
  //UartTxEmpty查询软件发送标志(由发送中断清零)，须在关闭中断前调用
  while(!UartTxEmpty(COM1) && (HAL_GetTick() - t0) < 1000u)
  {
  }
}

/* ==================== 日志与复位原因 ==================== */
/* 本段函数App与BootLoader均需使用，不置于FW_BOOT_BUILD_LOADER条件编译内 */

/* 日志行缓冲置于各函数栈上(不超过FW_PORT_LOG_LINE_MAX)，不使用静态缓冲，本层日志可重入 */

/**
* @brief   取本次复位原因
* @return  "POR"/"PIN"/"SW"/"IWDG"/"WWDG"/"LPWR"/"OBL"/"UNKNOWN"
* @note    一次复位可能置位多个标志，按"具体优先"的顺序判断；读取后清除标志，
*          因此只调用一次。本函数为STM32L0专有实现，换芯片只改此处；上层不访问RCC->CSR。
*/
const char *fw_port_reset_reason_get(void)
{
  uint32_t csr = RCC->CSR;
  const char *reason = "UNKNOWN";

  if((csr & RCC_CSR_IWDGRSTF) != 0u) reason = "IWDG";
  else if((csr & RCC_CSR_WWDGRSTF) != 0u) reason = "WWDG";
  else if((csr & RCC_CSR_SFTRSTF ) != 0u) reason = "SW";
  else if((csr & RCC_CSR_LPWRRSTF) != 0u) reason = "LPWR";
  else if((csr & RCC_CSR_OBLRSTF ) != 0u) reason = "OBL";
  else if((csr & RCC_CSR_PINRSTF ) != 0u) reason = "PIN";
  else if((csr & RCC_CSR_PORRSTF ) != 0u) reason = "POR";

  //读取后清除标志，避免下次复位重复上报旧标志
  RCC->CSR |= RCC_CSR_RMVF;
  return reason;
}

/**
* @brief   输出一行日志(变参版)
* @param   fmt  格式串(编译期字面量)
* @param   ap   已初始化的参数列表
* @note    本函数负责组装整行并限制长度，再交给平台调试输出通道；上层无需处理
*          行长与原子性。调用约定见fw_port.h文件头。
*/
void fw_port_log_vprintf(const char *fmt, va_list ap)
{
  char line[FW_PORT_LOG_LINE_MAX];
  int n;

  n = vsnprintf(line, sizeof(line), fmt, ap);
  if(n <= 0)
  {
    return;
  }
  if(n >= (int)sizeof(line))
  {
    //超长被截断：将末尾两字节改写为\r\n，保证上位机可按行切分
    line[sizeof(line) - 3u] = '\r';
    line[sizeof(line) - 2u] = '\n';
  }
  //交由平台调试输出通道发送(此处只输出字符串，不做格式化)
  dbg_printf("%s", line);
}

/**
* @brief   输出一行日志
* @param   fmt  格式串(编译期字面量)
* @param   ...  格式参数
* @note    转发至变参版，库的日志出口唯一
*/
void fw_port_log_printf(const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  fw_port_log_vprintf(fmt, ap);
  va_end(ap);
}

/* ==================== 非易失存储原语 ==================== */
/* 库只依赖这些原语；具体器件由平台头与平台后端提供。 */

int fw_port_flash_read(uint32_t addr, uint8_t *buf, uint32_t size)
{
  return (flash_read(addr, buf, size) == FLASH_OK) ? 0 : -1;
}

int fw_port_flash_write(uint32_t addr, const uint8_t *buf, uint32_t size)
{
  return (flash_write(addr, buf, size) == FLASH_OK) ? 0 : -1;
}

int fw_port_flash_erase(uint32_t addr, uint32_t size)
{
  return (flash_erase(addr, size) == FLASH_OK) ? 0 : -1;
}

int fw_port_eeprom_read(uint32_t addr, uint8_t *buf, uint32_t size)
{
  return (eeprom_read(addr, buf, size) == FLASH_OK) ? 0 : -1;
}

int fw_port_eeprom_write(uint32_t addr, const uint8_t *buf, uint32_t size)
{
  return (eeprom_write(addr, buf, size) == FLASH_OK) ? 0 : -1;
}

int fw_port_eeprom_erase(uint32_t addr, uint32_t size)
{
  return (eeprom_erase(addr, size) == FLASH_OK) ? 0 : -1;
}

void fw_port_irq_disable(void)
{
  __disable_irq();
}

void fw_port_irq_enable(void)
{
  __enable_irq();
}

/* 编译期校验：平台接收缓冲必须放得下最长的IAP帧 */
#if (FW_IAP_RX_FRAME_MAX > FW_PORT_RX_BUF_SIZE)
	#error "FW_PORT_RX_BUF_SIZE is too small for IAP frames"
#endif
/* ==================== 跳转 ==================== */

void fw_port_jump_to_app(uint32_t app_addr)
{
  typedef void (*app_func_t)(void);
  //关闭时基，避免跳转后仍产生中断
  SysTick->CTRL = 0;
  SysTick->LOAD = 0;
  SysTick->VAL = 0;
  //向量表重定位到应用
  SCB->VTOR = app_addr;
  //设置主栈指针并跳转
  __set_MSP(*(volatile uint32_t *)app_addr);
  ((app_func_t)(*(volatile uint32_t *)(app_addr + 4u)))();
  while(1)
  {
  }
}
/* ==================== fw_kv 临界区 ==================== */
/* 两种实现由configUSE_FREERTOS选择：无OS关中断，有OS用互斥锁。
 * 临界区可能包含Flash擦写，有OS时不能关中断，否则影响系统实时性。 */
#ifndef configUSE_FREERTOS
  #define configUSE_FREERTOS  0
#endif

#if (configUSE_FREERTOS == 1)
#include "FreeRTOS.h"
#include "semphr.h"

static SemaphoreHandle_t g_kv_mutex = NULL;

void fw_port_kv_crit_init(void)
{
  if(g_kv_mutex == NULL)
  {
    g_kv_mutex = xSemaphoreCreateMutex();
  }
}

void fw_port_kv_crit_lock(void)
{
  fw_port_kv_crit_init();
  if(g_kv_mutex != NULL)
  {
    xSemaphoreTake(g_kv_mutex, portMAX_DELAY);
  }
}

void fw_port_kv_crit_unlock(void)
{
  if(g_kv_mutex != NULL)
  {
    xSemaphoreGive(g_kv_mutex);
  }
}
#else
void fw_port_kv_crit_init(void)
{
}

void fw_port_kv_crit_lock(void)
{
  __disable_irq();
}

void fw_port_kv_crit_unlock(void)
{
  __enable_irq();
}
#endif

/**
* @brief   断言失败处理
* @param   cond  失败的条件文本
* @param   file  源文件名
* @param   line  行号
* @note    输出一行后主动复位，不依赖看门狗喂狗位置与调试器连接状态。
*          复位后BootLoader重新执行升级流程；fw_kv为掉电安全设计，不会停留在中间状态。
*          如需改为停机等调试器，将NVIC_SystemReset()替换为死循环。
*/
void fw_assert_fail(const char *cond, const char *file, uint32_t line)
{
  const char *base = file;

  //仅取文件名，去除__FILE__可能携带的路径(Keil下为长相对路径)，避免整行超长被截断
  if(file != NULL)
  {
    for(const char *p = file; *p != '\0'; p++)
    {
      if(*p == '/' || *p == '\\')
      {
        base = p + 1;
      }
    }
  }
  fw_port_log_printf("[ASSERT] %s:%u %s\r\n",
      (base != NULL) ? base : "?", (unsigned)line, (cond != NULL) ? cond : "?");
  //等待本行发送完成，避免复位截断断言日志
  fw_port_wait_tx_complete();
  //主动复位，使设备回到确定状态
  NVIC_SystemReset();
  while(1)
  {
    //NVIC_SystemReset()不返回，此处仅作兜底
  }
}

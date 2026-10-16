/**
* @file    fw_assert.h
* @brief   库内运行时断言，用于检查内部不变量
* @note    使用范围：
*          1) 外部输入错误(串口帧、Flash返回码、分区表内容)返回错误码，不使用断言；
*          2) 仅用于库内部推导出的条件，调用方无法传入；
*          3) FW_ASSERT_ENABLE为0时条件与字符串均不占用Flash，调用方无需条件编译；
*          4) 失败动作由平台层的fw_assert_fail()实现，库不规定具体行为。
*/

#ifndef _FW_ASSERT_H_
#define _FW_ASSERT_H_

#include <stdint.h>

/* 断言总开关：1=保留检查，0=全部编译掉 */
#ifndef FW_ASSERT_ENABLE
  #define FW_ASSERT_ENABLE 1
#endif

/**
* @brief  断言失败处理(平台层实现，不返回)
* @param  cond  失败条件的字符串形式
* @param  file  源文件名
* @param  line  行号
* @note   实现方负责输出与停机/复位，禁止在此访问Flash；默认实现见fw_port.c
*/
void fw_assert_fail(const char *cond, const char *file, uint32_t line);

#if (FW_ASSERT_ENABLE != 0)
  /* 条件不得含副作用：开关关闭后整个表达式不参与编译 */
  #define FW_ASSERT(cond)                              \
    do                                                 \
    {                                                  \
      if(!(cond))                                      \
      {                                                \
        fw_assert_fail(#cond, __FILE__, __LINE__);     \
      }                                                \
    }while(0)
  /* 条件本身含副作用(自增、赋值)时使用：第一个参数在开关关闭时仍被求值，
   * 例如 FW_ASSERT_EVAL(cnt++, cnt <= FW_KV_MAX_KEYS)。
   * 第二个参数不得重复同一副作用，否则开关打开时会求值两次 */
  #define FW_ASSERT_EVAL(expr, cond)                    \
    do                                                  \
    {                                                   \
      (void)(expr);                                     \
      FW_ASSERT(cond);                                  \
    }while(0)
#else
  #define FW_ASSERT(cond)            ((void)0)
  #define FW_ASSERT_EVAL(expr, cond) ((void)(expr))
#endif

/* 编译期断言：用于常量条件(表大小、结构体长度)，不占用运行时间与Flash。
 * _Static_assert要求C11；ARMCC5等工具链改用其自带的静态断言 */
#define FW_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)

#endif /* _FW_ASSERT_H_ */

#ifndef _FW_DELTA_H_
#define _FW_DELTA_H_

#include <stdint.h>

/* ==================== 差分记录格式 ====================
 * 全部小端，记录流描述的是“代码区”，不含镜像元数据：
 *   COPY:    0x01 | src_off u32 | len u32
 *   LITERAL: 0x02 | len u32 | data[len]
 *   DIFF:    0x03 | src_off u32 | len u32 | count u16 | (pos u16, val u8)*count
 *
 * DIFF 表示“旧块 + 稀疏差异”：设备先把旧镜像里的 len 字节读进缓冲，
 * 再按 pos 逐点加上 val（模 256），len 不超过 FW_DELTA_BUF_SIZE。
 * 它用来压缩“块内只有少量字节变化”的情况，避免整块走 LITERAL。
 *
 * 本模块只负责把记录流还原成代码字节，不关心Flash地址、镜像元数据、
 * CRC32与升级标志；旧数据从read回调取，新数据经write回调按顺序交出。
 */

#define FW_DELTA_OP_COPY       0x01u
#define FW_DELTA_OP_LITERAL    0x02u
#define FW_DELTA_OP_DIFF       0x03u
#define FW_DELTA_BUF_SIZE      128u

/* 错误码 */
#define FW_DELTA_OK              0
#define FW_DELTA_ERR_PARAM      -1   //参数非法
#define FW_DELTA_ERR_FORMAT     -2   //记录格式错误
#define FW_DELTA_ERR_RANGE      -3   //偏移或长度越界
#define FW_DELTA_ERR_IO         -4   //读旧镜像或写新镜像失败
#define FW_DELTA_ERR_INCOMPLETE -5   //记录流不完整或输出长度不符

/**
* @brief   从旧镜像读一段数据
* @param   user    调用方上下文
* @param   offset  代码区内的偏移
* @param   buf     输出缓冲区
* @param   len     读取字节数
* @return  0成功，非0失败
*/
typedef int (*fw_delta_read_fn)(void *user, uint32_t offset, uint8_t *buf, uint32_t len);

/**
* @brief   按顺序交出一段重建出的代码
* @param   user    调用方上下文
* @param   offset  代码区内的偏移(从0开始递增)
* @param   buf     数据
* @param   len     字节数
* @return  0成功，非0失败
*/
typedef int (*fw_delta_write_fn)(void *user, uint32_t offset, const uint8_t *buf, uint32_t len);

/* 解码器上下文：由调用方静态分配，字段仅供本模块使用 */
typedef struct
{
  uint32_t code_len;               //目标代码总长
  uint32_t old_len;                //旧代码总长
  uint32_t out_len;                //已交给write回调的字节数
  uint32_t src;                    //源偏移(COPY/DIFF)
  uint32_t remain;                 //当前记录输出字节数(COPY/LITERAL)或DIFF块长
  uint16_t out_fill;               //输出缓冲已填字节数
  uint8_t  op;                     //当前记录类型，0=空闲
  uint8_t  hdr[12];                //记录头收集缓冲
  uint8_t  hdr_have;               //已收集的记录头字节数
  uint8_t  hdr_need;               //当前记录还需要几个头字节
  uint8_t  diff_hdr[3];            //DIFF的(pos u16, val u8)
  uint8_t  diff_have;              //DIFF三元组已收字节数
  uint8_t  diff_started;           //DIFF块是否已读入缓冲
  uint16_t diff_left;              //DIFF剩余差异个数
  uint8_t  buf[FW_DELTA_BUF_SIZE]; //输出缓冲
  fw_delta_read_fn  read;          //读旧镜像
  fw_delta_write_fn write;         //写新镜像
  void *user;                      //回调上下文
}fw_delta_ctx_t;

/**
* @brief   初始化解码器
* @param   ctx       解码器上下文
* @param   code_len  目标代码总长(新镜像image_len-64)
* @param   old_len   旧代码总长
* @param   read      读旧镜像回调
* @param   write     写新镜像回调
* @param   user      回调上下文
* @return  FW_DELTA_OK或负错误码
*/
int fw_delta_init(fw_delta_ctx_t *ctx, uint32_t code_len, uint32_t old_len,
                  fw_delta_read_fn read, fw_delta_write_fn write, void *user);

/**
* @brief   喂入一段差分流
* @param   ctx   解码器上下文
* @param   data  差分流字节
* @param   len   字节数
* @return  FW_DELTA_OK或负错误码
* @note    可以按任意长度分片喂入，记录头与记录内容都允许跨片。
*/
int fw_delta_feed(fw_delta_ctx_t *ctx, const uint8_t *data, uint32_t len);

/**
* @brief   结束差分流并检查完整性
* @param   ctx  解码器上下文
* @return  FW_DELTA_OK或负错误码
* @note    要求没有未完成的记录，且重建总长度等于code_len。
*/
int fw_delta_finish(fw_delta_ctx_t *ctx);

/**
* @brief   查询已重建的代码长度
* @param   ctx  解码器上下文
* @return  已重建字节数(含尚未交给write回调的缓冲数据)
*/
uint32_t fw_delta_output_len(const fw_delta_ctx_t *ctx);

#endif /* _FW_DELTA_H_ */

#ifndef _IAP_H_
#define _IAP_H_

#include <stdint.h>
#include "fw_boot.h"
#include "fw_img.h"   //镜像元数据

/* 差分升级只在App构建启用：BL没有旧镜像可作基版本，收到START_DELTA回不支持 */
#ifndef FW_IAP_DELTA_ENABLE
  #define FW_IAP_DELTA_ENABLE  (FW_BOOT_BUILD_LOADER == 1)
#endif

#if FW_IAP_DELTA_ENABLE
  #include "fw_delta.h"
#endif

/* App构建与Boot构建都带串口升级：两个槽位都无有效镜像时，BootLoader停在串口升级模式 */

/* ==================== 接收状态(fw_iap_get_status() 的返回值) ==================== */
typedef enum
{
  FW_IAP_IDLE    = 0,   //空闲(没有进行中的会话)
  FW_IAP_RUNNING,       //进行中
  FW_IAP_DONE           //已完成(整包收完并提交)
}iap_state_t;

//单次IAP写入上限：只写一个槽位，防止越界写到STAGE分区
#define FW_IAP_IMAGE_MAX_SIZE        SLOT_SIZE

/* ==================== HEX协议 ====================
 * 统一帧(多字节字段一律小端)：
 *   SOF(2) VER_TYPE(1) OFFSET(4) LEN(2) DATA CRC16(2)
 *   帧总长 = 11 + LEN；CRC16覆盖SOF..DATA结束(含SOF)，初值0xFFFF
 *   VER_TYPE = (VER << 4) | TYPE，VER=1
 *
 * 类型：
 *   START(0x00)：OFFSET=0，LEN=64，DATA=64字节镜像元数据(整包)
 *   START_DELTA(0x03)：OFFSET=0，LEN=72，DATA=64字节镜像元数据
 *                     + base_crc32(4) + delta_len(4)(差分)
 *   DATA (0x01)：整包：OFFSET=代码偏移，DATA=明文代码
 *                差分：OFFSET=差分流偏移，DATA=差分记录流
 *   RESP (0x02)：OFFSET=已确认/期望位置，LEN=1，DATA[0]=状态码
 *
 * 设备收到START后按镜像元数据的image_len算出代码长度(code_len=image_len-64)；
 * 整包收满code_len字节、差分收满delta_len字节后自动校验并提交，没有单独的
 * 结束帧；最后一帧的RESP即提交结果。代码完整性由镜像元数据里的code_crc32保证：
 * 设备边收边算，不符回0x0D。差分还会先校验旧镜像的base_crc32，不符回0x0E。
 * 任何一步失败都作废会话，要求上位机从START整包重传。
 */
#define FW_IAP_HEX_SOF0              0x55u
#define FW_IAP_HEX_SOF1              0xAAu
#define FW_IAP_HEX_VER               0x01u
#define FW_IAP_HEX_TYPE_START        0x00u
#define FW_IAP_HEX_TYPE_DATA         0x01u
#define FW_IAP_HEX_TYPE_RESP         0x02u
#define FW_IAP_HEX_TYPE_START_DELTA  0x03u
#define FW_IAP_HEX_HDR_LEN           9u      //SOF2 + VER_TYPE1 + OFFSET4 + LEN2
#define FW_IAP_HEX_CRC_LEN           2u

//单帧DATA上限，工程可按串口接收缓冲调整
#ifndef FW_IAP_HEX_DATA_MAX
  #define FW_IAP_HEX_DATA_MAX        256u
#endif

//单帧DATA区最大长度：START帧的镜像元数据与DATA帧都要放得下
#define FW_IAP_HEX_MAX_PAYLOAD       ((FW_IAP_HEX_DATA_MAX > FW_IMG_META_LEN) ? FW_IAP_HEX_DATA_MAX : FW_IMG_META_LEN)

//最长的入帧(字节)：调用方给的接收缓冲不得小于它
#define FW_IAP_HEX_FRAME_MAX         (FW_IAP_HEX_HDR_LEN + FW_IAP_HEX_MAX_PAYLOAD + FW_IAP_HEX_CRC_LEN)
#define FW_IAP_RX_FRAME_MAX          FW_IAP_HEX_FRAME_MAX

//RESP状态码
#define FW_IAP_HEX_ST_OK             0x00u     //成功
#define FW_IAP_HEX_ST_BAD_FRAME      0x01u     //坏帧
#define FW_IAP_HEX_ST_NOT_STARTED    0x02u     //未开始
#define FW_IAP_HEX_ST_SIZE           0x03u     //镜像过大或长度不符
#define FW_IAP_HEX_ST_WRITE_FAIL     0x04u     //写Flash失败
#define FW_IAP_HEX_ST_COMMIT_FAIL    0x06u     //提交失败
#define FW_IAP_HEX_ST_IMAGE_INVALID  0x07u     //镜像无效
#define FW_IAP_HEX_ST_HW_MISMATCH    0x08u     //水印不符
#define FW_IAP_HEX_ST_OFFSET_ERROR   0x09u     //OFFSET不连续
#define FW_IAP_HEX_ST_VERSION_OLD    0x0Au     //版本过低(新镜像版本号不高于当前镜像)
#define FW_IAP_HEX_ST_CODE_CRC       0x0Du     //代码CRC32不符
#define FW_IAP_HEX_ST_BASE_MISMATCH  0x0Eu     //旧镜像CRC32与差分基版本不符
#define FW_IAP_HEX_ST_DELTA_FORMAT   0x0Fu     //差分记录流格式/长度错误
#define FW_IAP_HEX_ST_DELTA_UNSUPPORT 0x10u    //当前构建不支持差分(BL)

/* ==================== 会话状态 ====================
 * 会话只有一条，静态保存。镜像元数据在START帧收到后缓存在这里，
 * 代码收满后先写镜像元数据到代码末尾，再校验并提交。
 */
typedef struct
{
  uint32_t write_addr;               //代码区起始地址
  uint32_t code_len;                 //代码长度=image_len-镜像元数据长度
  uint32_t image_len;                //镜像逻辑总长=代码+镜像元数据
  uint32_t received_size;            //整包：已写入代码字节；差分：已收差分流字节
  uint32_t code_crc;                 //代码CRC32的累加中间值
  uint32_t delta_len;                //差分流总长(仅差分模式)
  uint32_t old_addr;                 //旧镜像槽位起始地址(仅差分模式)
  uint32_t out_frontier;             //新镜像已擦除到的绝对地址(仅差分模式)
  uint8_t  pack_mode;                //0=整包，1=差分
  uint8_t  in_progress;              //1=会话进行中
  uint8_t  percent;                  //接收进度(0~100)
  uint8_t  img_meta[FW_IMG_META_LEN];  //镜像元数据原始字节
  fw_img_meta_t info;                 //解析后的镜像元数据
#if FW_IAP_DELTA_ENABLE
  fw_delta_ctx_t delta;              //差分解码器
#endif
}iap_page_cache_t;

/**
* @brief   查询IAP接收状态
* @param   total_size     输出：镜像逻辑总长(含镜像元数据)
* @param   received_size  输出：已接收字节数
* @param   percent        输出：接收进度(0~100)
* @return  FW_IAP_IDLE/FW_IAP_RUNNING/FW_IAP_DONE(指针必须有效)
*/
iap_state_t fw_iap_get_status(uint32_t *total_size, uint32_t *received_size, uint8_t *percent);

/**
* @brief   解析HEX协议的字节流
* @param   buf  本次收到的字节，就地解析(缓冲由调用方提供，容量>=FW_IAP_RX_FRAME_MAX且可写)
* @param   len  字节数
* @note    帧未收全或CRC不通过就丢弃，由上位机超时重传该帧。
*/
void fw_iap_parse_hex_frame(uint8_t *buf, uint32_t len);


#endif /* _IAP_H_ */

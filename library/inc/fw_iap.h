#ifndef _IAP_H_
#define _IAP_H_

#include <stdint.h>
#include "fw_boot.h"

/* App构建必带IAP；BootLoader构建由FW_BOOT_IAP_ENABLE决定是否内置(见fw_boot.h) */
#if (FW_BOOT_BUILD_LOADER == 1) || (FW_BOOT_IAP_ENABLE != 0)

/* ==================== 返回码 ==================== */
typedef enum
{
  FW_IAP_SUCCESS       = 0,    //成功
  FW_IAP_ERR_PARAM     = -1,   //参数错误
  FW_IAP_ERR_NOT_START = -2,   //未开始(没收到起始帧)
  FW_IAP_ERR_ERASE     = -3,   //擦除失败
  FW_IAP_ERR_WRITE     = -4,   //写Flash失败
  FW_IAP_ERR_CRC       = -5,   //CRC校验失败
  FW_IAP_ERR_INVALID   = -6,   //无效镜像
  FW_IAP_ERR_SIZE      = -7    //镜像超过一个槽位
}iap_err_t;

/* ==================== 接收状态(fw_iap_get_status() 的返回值) ==================== */
typedef enum
{
  FW_IAP_IDLE    = 0,   //空闲(没有进行中的会话)
  FW_IAP_RUNNING,       //进行中
  FW_IAP_DONE           //已完成(整包收完并提交)
}iap_state_t;

/* ==================== 文本协议($UP) ====================
 * 帧：$UP OOOOOO TTTTTT CCCCC DATA
 *     24字节帧头("$UP " + 偏移6 + 空格 + 总长6 + 空格 + 整包CRC5 + 空格) + 数据
 *     偏移/总大小/CRC都是十进制；每帧DATA<=FW_IAP_TEXT_DATA_MAX
 * 应答：正常收帧回 "$UP 已收长度"；出错回 "$ + 英文提示"
 */
#define FW_IAP_TEXT_HDR_LEN          24u     //帧头固定长度
#define FW_IAP_TEXT_DATA_MAX         300u    //上位机每帧DATA上限
#define FW_IAP_TEXT_FRAME_MAX        (FW_IAP_TEXT_HDR_LEN + FW_IAP_TEXT_DATA_MAX)  //=324, 所有入帧里最长的一帧

//单次IAP写入上限：只写一个槽位(SLOT1或XIP模式的对侧槽位)，防止越界写到STAGE分区
#define FW_IAP_IMAGE_MAX_SIZE        SLOT_SIZE

/* ==================== HEX协议 ====================
 * 统一帧(多字节字段大端)：SOF(2) VER_TYPE(1) OFFSET(4) LEN(2) DATA CRC16(2)
 *   帧总长 = 11 + LEN；CRC16覆盖SOF..DATA结束(含SOF)，初值0xFFFF
 *   类型：START(0x00) 起始帧：OFFSET=0，DATA=镜像总大小(4字节大端)
 *         DATA (0x01) 数据帧：OFFSET=本帧数据在镜像内的偏移
 *         END  (0x02) 结束帧：OFFSET=镜像总长度，DATA=整包CRC16(2字节大端)
 *         ACK  (0x03)        ：LEN=0，OFFSET=已确认写入到的位置
 *         NAK  (0x04)        ：LEN=1，DATA[0]=状态码，OFFSET=期望位置
 *   处理：START开始一轮会话(总大小超过一个槽位直接拒收)；DATA要求
 *         OFFSET==已写入长度(丢帧/乱序回NAK)，OFFSET+LEN==已写入长度判为重传、
 *         只重发ACK；END校验整包CRC16与镜像有效性后提交；任何一步失败都
 *         作废会话并回OFFSET=0，让上位机整包重传。
 */
#define FW_IAP_HEX_SOF0              0x55u
#define FW_IAP_HEX_SOF1              0xAAu
#define FW_IAP_HEX_VER               0x01u
#define FW_IAP_HEX_TYPE_START        0x00u
#define FW_IAP_HEX_TYPE_DATA         0x01u
#define FW_IAP_HEX_TYPE_END          0x02u
#define FW_IAP_HEX_TYPE_ACK          0x03u
#define FW_IAP_HEX_TYPE_NAK          0x04u
#define FW_IAP_HEX_HDR_LEN           9u      //SOF2 + VER_TYPE1 + OFFSET4 + LEN2
#define FW_IAP_HEX_CRC_LEN           2u
#define FW_IAP_HEX_MAX_PAYLOAD       256u    //单帧DATA上限(上位机每帧256字节)
#define FW_IAP_HEX_FRAME_MAX         (FW_IAP_HEX_HDR_LEN + FW_IAP_HEX_MAX_PAYLOAD + FW_IAP_HEX_CRC_LEN)

//最长的入帧(字节)：调用方给的接收缓冲不得小于它(缓冲大小属串口层配置，
//应答经fw_port_send_buf()发出，本模块不直接引用平台头文件)
#define FW_IAP_RX_FRAME_MAX          ((FW_IAP_TEXT_FRAME_MAX > FW_IAP_HEX_FRAME_MAX) ? FW_IAP_TEXT_FRAME_MAX : FW_IAP_HEX_FRAME_MAX)

//NAK状态码(与文本协议的英文提示一一对应)
#define FW_IAP_HEX_ST_OK             0x00u
#define FW_IAP_HEX_ST_BAD_FRAME      0x01u     //$BAD FRAME
#define FW_IAP_HEX_ST_NOT_STARTED    0x02u     //$NOT STARTED
#define FW_IAP_HEX_ST_SIZE           0x03u     //$IMAGE TOO BIG
#define FW_IAP_HEX_ST_WRITE_FAIL     0x04u     //$WRITE FAIL
#define FW_IAP_HEX_ST_CRC_ERROR      0x05u     //$CRC ERROR
#define FW_IAP_HEX_ST_COMMIT_FAIL    0x06u     //$COMMIT FAIL
#define FW_IAP_HEX_ST_IMAGE_INVALID  0x07u     //$IMAGE INVALID
#define FW_IAP_HEX_ST_HW_MISMATCH    0x08u     //预留：型号不符
#define FW_IAP_HEX_ST_OFFSET_ERROR   0x09u     //OFFSET不连续(丢帧/乱序)

/* ==================== 会话状态(文本/HEX共用) ==================== */
typedef struct
{
  uint32_t page_index;  //文本：当前页内偏移(0 ~ FW_FLASH_PAGE_SIZE-1)
  uint32_t write_addr;  //文本：当前Flash写入地址
  uint32_t total_size;  //镜像总大小(字节，来自起始帧)
  uint32_t received_size;  //已写入字节数(=下一帧期望的OFFSET)
  uint8_t  in_progress;  //1=会话进行中
  uint8_t  percent;  //接收进度(0~100)
  uint16_t file_crc;  //整包文件CRC16累加值(初值0xFFFF)
  uint8_t *page_buf;  //文本：页缓冲指针(指向 iap_page_buf)
}iap_page_cache_t;

//接口
int fw_iap_process_data(uint8_t *data, uint32_t len, uint32_t total, uint8_t frame_type);//文本协议
iap_state_t fw_iap_get_status(uint32_t *total_size, uint32_t *received_size, uint8_t *percent);//查询接收进度
void fw_iap_parse_text_frame(uint8_t *frame, uint32_t len);//文本：整帧输入
/* HEX：把本次收到的字节就地解析(缓冲由调用方提供，容量>=267字节且可写)。
 * 一帧没收全或CRC不过就直接丢弃，由上位机超时重传该帧。 */
void fw_iap_parse_hex_frame(uint8_t *buf, uint32_t len);

#endif

#endif /* _IAP_H_ */

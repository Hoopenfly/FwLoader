#ifndef _FW_IMG_H_
#define _FW_IMG_H_

#include <stdint.h>
#include "fw_cfg.h"

/* ==================== 固件镜像元数据 ====================
 * 只描述镜像本身，不涉及加密、哈希与签名：固件按明文传输、按明文落盘，
 * 镜像元数据完整性由CRC16保证，代码完整性由code_crc32保证，来源由项目水印保证。
 *
 * 镜像元数据固定64字节，全部小端、无填充，作为START帧的DATA下发；
 * 设备收满 image_len - 64 字节代码后，把镜像元数据紧贴代码末尾写入。
 * 槽位布局： [代码(code_len字节)][镜像元数据(64字节)][空余擦除态]
 * image_len 是逻辑长度（代码 + 镜像元数据），用于传输与校验，不表示Flash占用长度。
 */

/* ==================== 镜像元数据格式 ====================
 * 偏移  长度  字段
 *   0     4    魔数 'IMGH'
 *   4     1    镜像元数据格式版本
 *   5     1    保留，必须为0
 *   6     2    meta_len：整个镜像元数据长度
 *   8     4    image_len：镜像逻辑总长 = 代码长度 + meta_len
 *  12     4    image_ver：镜像版本，单调递增
 *  16     4    project_id：项目水印
 *  20     2    customer_id
 *  22     2    batch_id
 *  24     4    build_date，如20261020
 *  28     4    code_crc32：代码的CRC32
 *  32    30    保留，必须为0
 *  62     2    CRC16，覆盖前62字节
 */
#define FW_IMG_META_MAGIC      0x48474D49u  //"IMGH"小端
#define FW_IMG_META_FMT_VER    1u           //镜像元数据格式版本
#define FW_IMG_META_FIXED_LEN  28u          //固定字段长度
#define FW_IMG_META_CRC32_OFS  28u          //code_crc32字段偏移
#define FW_IMG_META_RESV_OFS   32u          //保留字段偏移
#define FW_IMG_META_RESV_LEN   30u          //保留字段长度
#define FW_IMG_META_CRC_LEN    2u           //CRC16长度
#define FW_IMG_META_LEN        64u          //镜像元数据总长度

/* 镜像元数据的校验结果：0成功，负值为具体原因 */
#define FW_IMG_OK             0
#define FW_IMG_ERR_TRAILER   -2   //镜像元数据格式不符或与槽位不自洽
#define FW_IMG_ERR_READ      -3   //Flash读取失败
#define FW_IMG_ERR_PROJECT   -6   //镜像的项目标识与本机不符
#define FW_IMG_ERR_CRC       -4   //代码CRC32与镜像元数据登记值不符

/* 镜像元数据解析结果(由fw_img_meta_parse填充) */
typedef struct
{
  uint32_t magic;        //魔数FW_IMG_META_MAGIC
  uint8_t  format_ver;   //镜像元数据格式版本FW_IMG_META_FMT_VER
  uint16_t meta_len;      //整个镜像元数据长度FW_IMG_META_LEN
  uint32_t image_len;    //镜像逻辑总长=代码+meta_len
  uint32_t image_ver;    //镜像版本(编码见FW_IMG_VER)
  uint32_t project_id;   //项目水印
  uint16_t customer_id;  //客户号
  uint16_t batch_id;     //批次号
  uint32_t build_date;   //构建日期
  uint32_t code_crc32;   //代码的CRC32
}fw_img_meta_t;

/* ==================== 版本号 ====================
 * 版本按 主.次.修订 打包进32位，单调递增；设备侧拒绝低于已记录版本的镜像。
 * 打包工具与设备必须用同一编码规则。
 */
#define FW_IMG_VER(major, minor, patch)  \
  ((((uint32_t)(major) & 0xFFu) << 24) | (((uint32_t)(minor) & 0xFFu) << 16) | (((uint32_t)(patch) & 0xFFu) << 8))

/**
* @brief   计算CRC16(Modbus)
* @param   data  数据指针
* @param   len   字节数
* @return  CRC值，初值0xFFFF
* @note    镜像元数据与串口帧共用同一算法，调用方不必各自实现。
*/
uint16_t fw_img_crc16(const uint8_t *data, uint32_t len);

/* ==================== 代码CRC32 ====================
 * 标准反射CRC32(多项式0xEDB88320，初值0xFFFFFFFF，结果取反)，
 * 与上位机Get-Crc32一致。设备接收代码时逐帧累加，收满后与镜像元数据比对。
 */
#define FW_IMG_CRC32_INIT    0xFFFFFFFFu
#define FW_IMG_CRC32_FINAL(crc)  ((uint32_t)(crc) ^ 0xFFFFFFFFu)

/**
* @brief   逐段更新CRC32
* @param   crc   上一段的中间值(首段传FW_IMG_CRC32_INIT)
* @param   data  数据指针
* @param   len   字节数
* @return  新的中间值；最终值需再经FW_IMG_CRC32_FINAL()取反
*/
uint32_t fw_img_crc32_update(uint32_t crc, const uint8_t *data, uint32_t len);

/**
* @brief   解析镜像元数据并校验其自洽性
* @param   buf  镜像元数据原始字节
* @param   len  可用字节数
* @param   out  输出结构
* @return  FW_IMG_OK=合法，负值=格式不符
*/
int fw_img_meta_parse(const uint8_t *buf, uint32_t len, fw_img_meta_t *out);

/**
* @brief   构造镜像元数据并填入CRC16
* @param   meta  输入字段，image_len/image_ver/project_id/code_crc32必须已填好
* @param   buf  输出缓冲，至少FW_IMG_META_LEN字节
* @param   len  缓冲可用长度
* @return  FW_IMG_OK=成功，负值=参数非法
* @note    保留字段一律置0，CRC16覆盖前62字节；与fw_img_meta_parse互为逆操作。
*/
int fw_img_meta_build(const fw_img_meta_t *meta, uint8_t *buf, uint32_t len);

/**
* @brief   检查镜像的项目水印是否与本机一致
* @param   meta  已解析的镜像元数据
* @return  0=一致，非0=不符
*/
int fw_img_meta_project_ok(const fw_img_meta_t *meta);

/**
* @brief   检查镜像版本是否不低于设备记录的最低版本
* @param   image_ver  镜像版本
* @param   min_ver    设备记录的最低允许版本
* @return  0=允许，非0=版本过低
*/
int fw_img_ver_ok(uint32_t image_ver, uint32_t min_ver);

/**
* @brief   在槽位内定位并解析镜像元数据
* @param   addr       槽位起始地址
* @param   slot_size  槽位大小
* @param   out        输出：解析结果
* @return  FW_IMG_OK=找到，负值=没有或无效
* @note    镜像元数据紧贴代码末尾，从槽位起始按字节扫描魔数定位；扫描不到时
*          回退读取槽位末尾64字节，兼容镜像元数据固贴槽位末尾的旧布局。
*/
int fw_img_locate(uint32_t addr, uint32_t slot_size, fw_img_meta_t *out);

/**
* @brief   校验一个槽位里的镜像
* @param   addr       槽位起始地址
* @param   slot_size  槽位大小
* @param   image_ver  输出：镜像版本，可为NULL
* @return  FW_IMG_OK=有效，负值=FW_IMG_ERR_*
* @note    只校验镜像元数据本身与项目水印；代码完整性由镜像元数据里的code_crc32在
*          传输阶段校验，这里不重复读取代码。
*/
int fw_img_verify(uint32_t addr, uint32_t slot_size, uint32_t *image_ver);


/**
* @brief   完整校验一个槽位里的镜像：定位元数据、查水印、回读代码算CRC32比对
* @param   addr       槽位起始地址
* @param   slot_size  槽位大小
* @param   image_ver  输出：镜像版本，可为NULL
* @return  FW_IMG_OK=有效，负值=FW_IMG_ERR_*
* @note    与fw_img_verify()的区别：本函数把代码区从Flash读回来重新累加CRC32，
*          与元数据里的code_crc32比对，能发现写入不完整或Flash内容损坏。
*          代价是遍历整个代码区，用时与镜像大小和器件读取速度成正比。
*/
int fw_img_verify_full(uint32_t addr, uint32_t slot_size, uint32_t *image_ver);
#endif /* _FW_IMG_H_ */

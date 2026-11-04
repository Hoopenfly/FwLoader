#ifndef FW_DELTA_CODEC_H                            //头文件保护宏，避免重复包含
#define FW_DELTA_CODEC_H                            //置位头文件保护宏

#include <cstdint>                                  //uint8_t、uint16_t、uint32_t、uint64_t
#include <string>                                   //std::string
#include <vector>                                   //std::vector

#include "fw_image.h"                               //ImageMeta、crc16、crc32

namespace fw                                        //固件镜像与差分处理命名空间
{

//差分生成参数
struct DeltaOptions
{
  uint32_t cdc_min = 128;         //CDC最小块
  uint32_t cdc_avg_bits = 8;      //CDC平均块=1<<avg_bits
  uint32_t cdc_max = 1024;        //CDC最大块
  uint32_t fixed_size = 512;      //固定分块基线
  uint32_t literal_merge_max = 4096;  //相邻LITERAL合并上限
};

//差分统计
struct DeltaStats
{
  uint64_t delta_size = 0;                            //记录流总字节数
  uint64_t copy_bytes = 0;                            //COPY记录覆盖的字节数
  uint64_t literal_bytes = 0;                         //LITERAL记录携带的字节数
  uint64_t diff_bytes = 0;                            //DIFF记录覆盖的字节数
  uint32_t copy_ops = 0;                              //COPY记录条数
  uint32_t literal_ops = 0;                           //LITERAL记录条数
  uint32_t diff_ops = 0;                              //DIFF记录条数
  uint32_t chunks = 0;                                //新镜像切块总数
  std::string strategy;           //"cdc"或"fixed"
};

//一个完整的差分包：新镜像元数据 + 基版本CRC32 + 记录流
struct DeltaPack
{
  ImageMeta meta;                                 //新镜像元数据，含补全后的image_len与code_crc32
  uint32_t base_crc32 = 0;                            //旧镜像代码区CRC32，应用前用于校验基版本
  std::vector<uint8_t> payload;                       //差分记录流，为空表示生成失败
  DeltaStats stats;                                   //本次差分统计
};

/**
 * @brief   生成差分记录流
 * @param   old_code  旧镜像代码区(不含镜像元数据)
 * @param   new_code  新镜像代码区(不含镜像元数据)
 * @param   meta    新镜像元数据(函数会补全image_len/code_crc32)
 * @param   opt       参数
 * @return  差分包；payload为空表示失败
 * @note    CDC与固定分块各算一遍，取较小者；生成后立即本地应用自校验。
 */
DeltaPack make_delta(const std::vector<uint8_t> &old_code,
                     const std::vector<uint8_t> &new_code,
                     const ImageMeta &meta,
                     const DeltaOptions &opt);

/**
 * @brief   应用差分包
 * @param   old_code  旧镜像代码区
 * @param   pack      差分包
 * @param   out       输出：新代码区
 * @return  true成功，false失败(格式错、越界或长度不符)
 */
bool apply_delta(const std::vector<uint8_t> &old_code,
                 const DeltaPack &pack,
                 std::vector<uint8_t> &out);

/**
 * @brief   应用一段差分记录流
 * @param   old_code  旧镜像代码区
 * @param   payload   记录流
 * @param   code_len  期望的新代码长度
 * @param   out       输出：新代码区
 * @return  true成功，false失败
 */
bool apply_delta_stream(const std::vector<uint8_t> &old_code,
                        const std::vector<uint8_t> &payload,
                        uint32_t code_len,
                        std::vector<uint8_t> &out);

} // namespace fw

#endif // FW_DELTA_CODEC_H

#ifndef FW_IMAGE_H                                  //头文件保护宏，避免同一编译单元重复展开
#define FW_IMAGE_H                                  //置位头文件保护宏

#include <cstddef>                                  //std::size_t
#include <cstdint>                                  //uint8_t、uint16_t、uint32_t等定宽整型
#include <string>                                   //std::string
#include <vector>                                   //std::vector

namespace fw                                        //固件镜像与差分处理命名空间
{

//镜像元数据固定64字节，字段全部小端，与设备端 fw_img.h 保持一致
constexpr uint32_t kImgMagic    = 0x48474D49u;  //"IMGH"
constexpr uint8_t  kImgFmtVer   = 1u;           //镜像元数据格式版本号，设备端只接受该版本
constexpr uint32_t kImgMetaLen   = 64u;          //镜像元数据固定长度，单位字节
constexpr uint32_t kImgCrc32Ofs = 28u;          //代码区CRC32在镜像元数据内的偏移
constexpr uint32_t kImgCrc16Ofs = 62u;          //镜像元数据CRC16在镜像元数据内的偏移，位于头部最后2字节

//镜像元数据字段
struct ImageMeta
{
  uint32_t magic = kImgMagic;                             //镜像魔数，固定为"IMGH"的ASCII小端
  uint8_t  format_ver = kImgFmtVer;                       //镜像元数据格式版本
  uint16_t meta_len = static_cast<uint16_t>(kImgMetaLen);   //镜像元数据长度，固定64字节
  uint32_t image_len = 0;       //代码 + 镜像元数据
  uint32_t image_ver = 0;       //主.次.修订 打包
  uint32_t project_id = 1;      //项目水印
  uint16_t customer_id = 0;                               //客户编号，区分定制版本
  uint16_t batch_id = 0;                                  //生产批次号
  uint32_t build_date = 0;                                //构建日期，由构建脚本填写
  uint32_t code_crc32 = 0;      //代码区CRC32
};

/**
 * @brief   计算CRC16(Modbus)
 * @param   data  数据
 * @param   len   字节数
 * @return  CRC值，初值0xFFFF
 */
uint16_t crc16(const uint8_t *data, std::size_t len);

/**
 * @brief   计算CRC32(标准反射算法)
 * @param   data  数据
 * @param   len   字节数
 * @return  CRC值，初值0xFFFFFFFF并取反
 */
uint32_t crc32(const uint8_t *data, std::size_t len);

/**
 * @brief   按镜像元数据结构生成64字节
 * @param   h  镜像元数据
 * @return  64字节镜像元数据(含CRC16)
 */
std::vector<uint8_t> build_meta(const ImageMeta &h);

/**
 * @brief   解析64字节镜像元数据
 * @param   buf  数据
 * @param   len  字节数
 * @param   out  输出
 * @return  0成功，非0失败
 */
int parse_meta(const uint8_t *buf, std::size_t len, ImageMeta &out);

/**
 * @brief   检测并剥离数据末尾自带的64字节镜像元数据
 * @param   data  输入输出：完整文件字节，命中时原地剥离镜像元数据
 * @param   out   输出：被剥离的镜像元数据，未命中时不修改
 * @return  1命中并已剥离、0没有镜像元数据按纯代码处理、-1镜像元数据存在但内容不完整
 * @note    判据为魔数、格式版本、头部长度字段与头部CRC16，与parse_meta一致；
 *          头部声明的总长度还必须等于文件实际长度，否则按损坏处理，
 *          避免把半截镜像元数据当成代码烧进设备。
 */
int strip_embedded_meta(std::vector<uint8_t> &data, ImageMeta &out);

/**
 * @brief   只读取文件末尾的镜像元数据，不把整份文件读进内存
 * @param   path  固件文件路径
 * @param   out   输出：命中时填充解析结果
 * @return  1命中、0文件没有镜像元数据、-1镜像元数据存在但不完整或文件打不开
 * @note    判据与strip_embedded_meta一致：魔数、格式版本、头部长度、头部CRC16，
 *          且头部声明的总长度必须等于文件实际大小。用于界面选中文件后立即显示版本与水印。
 */
int inspect_meta_file(const std::string &path, ImageMeta &out);

/**
 * @brief   把 主.次.修订 打包成32位版本号
 */
constexpr uint32_t make_version(uint8_t major, uint8_t minor, uint8_t patch)
{
  return (static_cast<uint32_t>(major) << 24)          //主版本号占用最高字节
       | (static_cast<uint32_t>(minor) << 16)          //次版本号占用次高字节
       | (static_cast<uint32_t>(patch) << 8);          //修订号占用第三字节，最低字节固定为0
}

} // namespace fw

#endif // FW_IMAGE_H

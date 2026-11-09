#include "fw_image.h"                               //镜像元数据结构与CRC计算接口

#include <cstring>                                  //C字符串与内存操作函数
#include <fstream>                                  //std::ifstream，按位置读取文件尾部

namespace fw                                        //固件镜像与差分处理命名空间
{

/**
 * @brief   计算CRC16(Modbus)
 * @param   data  数据
 * @param   len   字节数
 * @return  CRC值，初值0xFFFF
 */
uint16_t crc16(const uint8_t *data, std::size_t len)
{
  uint16_t crc = 0xFFFFu;                           //Modbus协议规定的寄存器初值

  if(data == nullptr)                               //空指针无法参与校验
  {
    return crc;                                     //无数据时返回初值
  }
  for(std::size_t i = 0; i < len; i++)              //逐字节参与运算
  {
    crc ^= data[i];                                 //当前字节异或到低8位
    for(int b = 0; b < 8; b++)                      //每个字节按位处理8次
    {
      if(crc & 1u)                                  //最低位为1时需异或多项式
      {
        crc = static_cast<uint16_t>((crc >> 1) ^ 0xA001u);  //右移一位后异或反向多项式0xA001
      }
      else                                          //最低位为0时不做异或
      {
        crc >>= 1;                                  //仅右移一位
      }
    }
  }
  return crc;                                       //返回16位校验值
}

/**
 * @brief   计算CRC32(标准反射算法)
 * @param   data  数据
 * @param   len   字节数
 * @return  CRC值，初值0xFFFFFFFF并取反
 */
uint32_t crc32(const uint8_t *data, std::size_t len)
{
  uint32_t crc = 0xFFFFFFFFu;                       //标准反射算法初值全1

  if(data == nullptr)                               //空指针无法参与校验
  {
    return 0u;                                      //无数据时返回0，与空数据CRC一致
  }
  for(std::size_t i = 0; i < len; i++)              //逐字节参与运算
  {
    crc ^= data[i];                                 //当前字节异或到低8位
    for(int b = 0; b < 8; b++)                      //每个字节按位处理8次
    {
      if(crc & 1u)                                  //最低位为1时需异或多项式
      {
        crc = (crc >> 1) ^ 0xEDB88320u;             //右移一位后异或反射多项式0xEDB88320
      }
      else                                          //最低位为0时不做异或
      {
        crc >>= 1;                                  //仅右移一位
      }
    }
  }
  return crc ^ 0xFFFFFFFFu;                         //结果按标准取反后返回
}

/**
 * @brief   写小端16位
 */
static void put_u16(std::vector<uint8_t> &out, uint16_t v)
{
  out.push_back(static_cast<uint8_t>(v & 0xFFu));           //低字节在前
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));    //高字节在后
}

/**
 * @brief   写小端32位
 */
static void put_u32(std::vector<uint8_t> &out, uint32_t v)
{
  out.push_back(static_cast<uint8_t>(v & 0xFFu));           //第0字节
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));    //第1字节
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));   //第2字节
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));   //第3字节
}

/**
 * @brief   读小端16位
 */
static uint16_t get_u16(const uint8_t *p)
{
  return static_cast<uint16_t>(p[0] | (p[1] << 8));         //低字节在前，拼成16位
}

/**
 * @brief   读小端32位
 */
static uint32_t get_u32(const uint8_t *p)
{
  return static_cast<uint32_t>(p[0])                        //第0字节为最低8位
       | (static_cast<uint32_t>(p[1]) << 8)                 //第1字节左移8位
       | (static_cast<uint32_t>(p[2]) << 16)                //第2字节左移16位
       | (static_cast<uint32_t>(p[3]) << 24);               //第3字节左移24位
}

/**
 * @brief   按镜像元数据结构生成64字节
 * @param   h  镜像元数据
 * @return  64字节镜像元数据(含CRC16)
 */
std::vector<uint8_t> build_meta(const ImageMeta &h)
{
  std::vector<uint8_t> out;  //镜像元数据字节缓冲
  out.reserve(kImgMetaLen);                          //一次预留64字节，避免中途扩容

  put_u32(out, kImgMagic);                          //偏移0：镜像魔数
  out.push_back(h.format_ver);                      //偏移4：格式版本，使用调用方指定的版本
  out.push_back(0u);                       //偏移5保留
  put_u16(out, static_cast<uint16_t>(kImgMetaLen));  //偏移6：镜像元数据长度
  put_u32(out, h.image_len);                        //偏移8：镜像总长度
  put_u32(out, h.image_ver);                        //偏移12：版本号
  put_u32(out, h.project_id);                       //偏移16：项目水印
  put_u16(out, h.customer_id);                      //偏移20：客户编号
  put_u16(out, h.batch_id);                         //偏移22：生产批次
  put_u32(out, h.build_date);                       //偏移24：构建日期
  put_u32(out, h.code_crc32);              //偏移28
  out.resize(kImgCrc16Ofs, 0u);            //保留到偏移62
  put_u16(out, crc16(out.data(), kImgCrc16Ofs));    //偏移62：头部CRC16，覆盖前62字节
  return out;                                       //返回完整64字节镜像元数据
}

/**
 * @brief   解析64字节镜像元数据
 * @param   buf  数据
 * @param   len  字节数
 * @param   out  输出
 * @return  0成功，非0失败
 */
int parse_meta(const uint8_t *buf, std::size_t len, ImageMeta &out)
{
  if(buf == nullptr || len < kImgMetaLen)            //缓冲区为空或不足一个头部长度
  {
    return -1;                                      //参数非法
  }
  if(get_u32(buf) != kImgMagic || buf[4] != kImgFmtVer)  //魔数或格式版本不匹配
  {
    return -1;                                      //不是本工程生成的镜像
  }
  if(get_u16(buf + 6) != kImgMetaLen)                //头部自述长度必须等于64
  {
    return -1;                                      //长度字段异常
  }

  ImageMeta parsed;                               //先解析到临时对象，全部校验通过后再写出
  parsed.magic = get_u32(buf);                      //偏移0：镜像魔数
  parsed.format_ver = buf[4];                       //偏移4：格式版本
  parsed.meta_len = get_u16(buf + 6);                //偏移6：镜像元数据长度
  parsed.image_len = get_u32(buf + 8);              //偏移8：镜像总长度
  parsed.image_ver = get_u32(buf + 12);             //偏移12：版本号
  parsed.project_id = get_u32(buf + 16);            //偏移16：项目水印
  parsed.customer_id = get_u16(buf + 20);           //偏移20：客户编号
  parsed.batch_id = get_u16(buf + 22);              //偏移22：生产批次
  parsed.build_date = get_u32(buf + 24);            //偏移24：构建日期
  parsed.code_crc32 = get_u32(buf + kImgCrc32Ofs);  //偏移28：代码区CRC32

  if(parsed.image_len <= kImgMetaLen)                //镜像长度必须大于头部本身
  {
    return -1;                                      //空代码区视为非法镜像
  }
  if(crc16(buf, kImgCrc16Ofs) != get_u16(buf + kImgCrc16Ofs))  //重算头部CRC16并比对
  {
    return -1;                                      //头部数据被破坏
  }

  out = parsed;                                     //全部校验通过后才更新输出参数
  return 0;                                         //解析成功
}

/**
 * @brief   检测并剥离数据末尾自带的64字节镜像元数据
 * @param   data  输入输出：完整文件字节，命中时原地剥离镜像元数据
 * @param   out   输出：被剥离的镜像元数据，未命中时不修改
 * @return  1命中并已剥离、0没有镜像元数据按纯代码处理、-1镜像元数据存在但内容不完整
 */
int strip_embedded_meta(std::vector<uint8_t> &data, ImageMeta &out)
{
  if(data.size() <= kImgMetaLen)                     //不足一个头部长度，不可能自带镜像元数据
  {
    return 0;                                       //按纯代码处理
  }
  const std::size_t ofs = data.size() - kImgMetaLen; //镜像元数据固定贴在文件末尾
  const uint8_t *p = data.data() + ofs;             //指向镜像元数据首字节

  if(get_u32(p) != kImgMagic)                       //魔数不符说明是纯代码文件
  {
    return 0;                                       //交给上层补写镜像元数据
  }

  ImageMeta parsed;                               //先解析到临时对象，校验通过后再写出
  if(parse_meta(p, kImgMetaLen, parsed) != 0)      //格式版本、头部长度或头部CRC16不符
  {
    return -1;                                      //魔数对但头部已损坏，不能当成代码烧
  }
  if(parsed.image_len != data.size())               //头部声明的总长必须等于文件实际长度
  {
    return -1;                                      //文件被截断或多出字节
  }

  out = parsed;                                     //记录被剥离的镜像元数据供调用方提示
  data.resize(ofs);                                 //移除末尾64字节，只保留代码区
  return 1;                                         //命中并已剥离
}

/**
 * @brief   只读取文件末尾的镜像元数据，不把整份文件读进内存
 * @param   path  固件文件路径
 * @param   out   输出：命中时填充解析结果
 * @return  1命中、0文件没有镜像元数据、-1镜像元数据存在但不完整或文件打不开
 */
int inspect_meta_file(const std::string &path, ImageMeta &out)
{
  std::ifstream in(path, std::ios::binary | std::ios::ate);  //以二进制打开并直接定位到文件末尾
  if(!in)                                           //文件不存在或被占用
  {
    return -1;                                      //交给上层按"读不了"提示
  }

  const std::streamoff size = in.tellg();           //文件总长度，-1表示获取失败
  if(size < static_cast<std::streamoff>(kImgMetaLen))  //不足一个镜像元数据长度
  {
    return 0;                                       //不可能自带镜像元数据，按纯代码处理
  }
  if(size > static_cast<std::streamoff>(0xFFFFFFFFull))  //镜像长度字段是32位，超长文件无法表示
  {
    return -1;                                      //按文件不合法处理，避免截断后误判相等
  }

  in.seekg(size - static_cast<std::streamoff>(kImgMetaLen));  //定位到末尾64字节
  uint8_t buf[kImgMetaLen] = {0};                   //只读镜像元数据，代码区完全不加载
  in.read(reinterpret_cast<char *>(buf), kImgMetaLen);       //读入64字节
  if(!in)                                           //读取失败
  {
    return -1;                                      //按不完整处理
  }

  if(get_u32(buf) != kImgMagic)                     //魔数不符说明是纯代码文件
  {
    return 0;                                       //上层提示需选择发布包
  }

  ImageMeta parsed;                                 //先解析到临时对象，校验通过后再写出
  if(parse_meta(buf, kImgMetaLen, parsed) != 0)     //格式版本、长度字段或头部CRC16不符
  {
    return -1;                                      //魔数对但内容损坏
  }
  if(parsed.image_len != static_cast<uint32_t>(size))  //头部声明的总长必须等于文件实际大小
  {
    return -1;                                      //文件被截断或尾部不是紧贴代码
  }

  out = parsed;                                     //交给调用方取版本与水印
  return 1;                                         //命中
}

} // namespace fw

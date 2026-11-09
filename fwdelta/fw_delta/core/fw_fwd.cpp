#include "fw_fwd.h"                                 //差分文件打包与解析接口

#include <cstdio>                                   //std::remove、std::rename
#include <cstring>                                  //std::memcmp
#include <fstream>                                  //文件流读写
#include <iterator>                                 //std::istreambuf_iterator

#include "fw_image.h"                               //build_meta、parse_meta

namespace fw                                        //固件镜像与差分处理命名空间
{

namespace
{

//差分文件格式：magic(4) + 版本(1) + 保留(3) + 镜像元数据(64) + base_crc32(4) + delta_len(4) + 记录流
constexpr char kFileMagic[4] = {'F', 'W', 'D', 'L'};  //文件魔数，固定ASCII"FWDL"
constexpr uint8_t kFileVer = 1u;                     //文件格式版本号
constexpr std::size_t kFileHdrLen = 4 + 1 + 3 + kImgMetaLen + 4 + 4;  //文件头总长，共80字节

/**
 * @brief   写小端32位
 */
void put_u32(std::vector<uint8_t> &out, uint32_t v)
{
  out.push_back(static_cast<uint8_t>(v & 0xFFu));           //第0字节
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));    //第1字节
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));   //第2字节
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));   //第3字节
}

/**
 * @brief   读小端32位
 */
uint32_t get_u32(const uint8_t *p)
{
  return static_cast<uint32_t>(p[0])                        //第0字节为最低8位
       | (static_cast<uint32_t>(p[1]) << 8)                 //第1字节左移8位
       | (static_cast<uint32_t>(p[2]) << 16)                //第2字节左移16位
       | (static_cast<uint32_t>(p[3]) << 24);               //第3字节左移24位
}

} // namespace

/**
 * @brief   把差分包打包成.fwd文件字节
 * @param   pack  差分包
 * @return  文件字节；失败返回空
 */
std::vector<uint8_t> build_fwd(const DeltaPack &pack)
{
  if(pack.payload.empty())  //空记录流无法组成有效差分包
  {
    return std::vector<uint8_t>();  //交给调用方按失败处理
  }
  std::vector<uint8_t> out;  //输出文件字节
  const std::vector<uint8_t> meta = build_meta(pack.meta);  //按镜像元数据生成64字节

  out.insert(out.end(), kFileMagic, kFileMagic + 4);        //偏移0：文件魔数
  out.push_back(kFileVer);                                  //偏移4：文件格式版本
  out.push_back(0u);                                        //偏移5：保留字节
  out.push_back(0u);                                        //偏移6：保留字节
  out.push_back(0u);                                        //偏移7：保留字节
  out.insert(out.end(), meta.begin(), meta.end());      //偏移8：64字节镜像元数据
  put_u32(out, pack.base_crc32);                            //偏移72：旧镜像代码区CRC32
  put_u32(out, static_cast<uint32_t>(pack.payload.size())); //偏移76：记录流长度
  out.insert(out.end(), pack.payload.begin(), pack.payload.end());  //偏移80起：记录流本体
  return out;                                               //返回完整文件字节
}

/**
 * @brief   解析.fwd文件字节
 * @param   data  文件字节
 * @param   pack  输出
 * @return  true成功
 */
bool parse_fwd(std::vector<uint8_t> data, DeltaPack &pack)
{
  if(data.size() < kFileHdrLen)                             //长度不足一个完整文件头
  {
    return false;                                           //数据截断
  }
  if(std::memcmp(data.data(), kFileMagic, 4) != 0 || data[4] != kFileVer)  //魔数或版本不匹配
  {
    return false;                                           //不是本工程生成的差分文件
  }
  DeltaPack result;  //先在临时对象解析，失败不污染输出参数
  if(parse_meta(data.data() + 8, kImgMetaLen, result.meta) != 0)  //解析偏移8处的镜像元数据
  {
    return false;                                           //镜像元数据非法
  }
  result.base_crc32 = get_u32(data.data() + 8 + kImgMetaLen);          //旧镜像代码区CRC32
  const uint32_t delta_len = get_u32(data.data() + 8 + kImgMetaLen + 4);  //记录流声明长度
  if(delta_len == 0u)                                       //空记录流无法执行升级
  {
    return false;                                           //按非法文件处理
  }
  if(kFileHdrLen + static_cast<std::size_t>(delta_len) != data.size())  //声明长度必须与文件实际长度一致
  {
    return false;                                           //长度不一致，拒绝半包或多余数据
  }
  //丢掉文件头后直接接管剩余字节，避免整文件与记录流各占一份内存
  data.erase(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(kFileHdrLen));
  result.payload = std::move(data);                         //记录流本体移入结果
  pack = std::move(result);                                 //结果整体移到输出参数
  return true;                                              //解析成功
}

/**
 * @brief   写.fwd文件
 * @param   path  文件路径
 * @param   pack  差分包
 * @return  true成功
 */
bool write_fwd(const std::string &path, const DeltaPack &pack)
{
  const std::vector<uint8_t> data = build_fwd(pack);         //先序列化成完整文件字节
  const std::string tmpPath = path + ".tmp";                 //先写临时文件，写完再替换目标
  {
    std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);  //二进制覆盖写临时文件

    if(!out)                                                 //路径不可写或权限不足
    {
      return false;                                          //打开失败
    }
    //一次写入全部文件字节，避免分多次写产生半个文件
    out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    out.flush();                                             //显式刷盘，确保数据落到文件
    if(!out)                                                 //写入失败时清理临时文件
    {
      std::remove(tmpPath.c_str());                          //删除残留的临时文件
      return false;                                          //按失败返回
    }
  }
  //替换前先把旧文件改名备份，替换成功后再删除备份；任一步失败都能恢复原文件
  const std::string bakPath = path + ".bak";                 //旧文件备份路径
  bool hadOld = false;                                       //目标文件是否存在并已在备份位
  if(std::ifstream(path, std::ios::binary).good())           //只在确实存在旧文件时才做备份
  {
    hadOld = (std::rename(path.c_str(), bakPath.c_str()) == 0);  //旧文件改名备份
    if(!hadOld)                                              //备份名被占用时清掉残留再试一次
    {
      std::remove(bakPath.c_str());                          //删除上一次残留的备份
      hadOld = (std::rename(path.c_str(), bakPath.c_str()) == 0);  //重试改名
    }
  }

  if(std::rename(tmpPath.c_str(), path.c_str()) != 0)        //临时文件原子替换目标文件
  {
    if(hadOld)                                               //替换失败时把旧文件还原
    {
      std::rename(bakPath.c_str(), path.c_str());
    }
    std::remove(tmpPath.c_str());                            //清理临时文件
    return false;                                            //按失败返回
  }
  if(hadOld)                                                 //替换成功后丢弃备份
  {
    std::remove(bakPath.c_str());
  }
  return true;                                               //替换完成才算写成功
}

/**
 * @brief   读.fwd文件
 * @param   path  文件路径
 * @param   pack  输出
 * @return  true成功
 */
bool read_fwd(const std::string &path, DeltaPack &pack)
{
  std::ifstream in(path, std::ios::binary);                  //二进制只读打开

  if(!in)                                                    //文件不存在或无读权限
  {
    return false;                                            //打开失败
  }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());  //一次性读入全部文件内容
  return parse_fwd(std::move(data), pack);                   //交给解析函数校验格式，字节直接移交
}

} // namespace fw

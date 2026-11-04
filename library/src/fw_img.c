/**
 * @file    fw_img.c
 * @brief   固件镜像元数据：解析、定位、水印校验、版本校验
 * @note    只处理明文镜像元数据；不依赖加密、哈希与签名模块。
 */

#include "fw_img.h"
#include <string.h>
#include "fw_port.h"   //fw_port_flash_read

/**
* @brief   读小端16位
* @param   p  数据指针
* @return  读到的值
*/
static uint16_t fw_img_rd_u16(const uint8_t *p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/**
* @brief   读小端32位
* @param   p  数据指针
* @return  读到的值
*/
static uint32_t fw_img_rd_u32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/**
* @brief   写小端16位
* @param   p      数据指针
* @param   value  要写入的值
*/
static void fw_img_wr_u16(uint8_t *p, uint16_t value)
{
  p[0] = (uint8_t)(value & 0xFFu);
  p[1] = (uint8_t)((value >> 8) & 0xFFu);
}

/**
* @brief   写小端32位
* @param   p      数据指针
* @param   value  要写入的值
*/
static void fw_img_wr_u32(uint8_t *p, uint32_t value)
{
  p[0] = (uint8_t)(value & 0xFFu);
  p[1] = (uint8_t)((value >> 8) & 0xFFu);
  p[2] = (uint8_t)((value >> 16) & 0xFFu);
  p[3] = (uint8_t)((value >> 24) & 0xFFu);
}

/**
* @brief   计算CRC16(Modbus)
* @param   data  数据指针
* @param   len   字节数
* @return  CRC值，初值0xFFFF
*/
uint16_t fw_img_crc16(const uint8_t *data, uint32_t len)
{
  uint16_t crc = 0xFFFFu;

  if(data == NULL)
  {
    return crc;
  }
  //逐位计算，与上位机Get-Crc16保持一致
  for(uint32_t i = 0u; i < len; i++)
  {
    crc ^= data[i];
    for(uint32_t b = 0u; b < 8u; b++)
    {
      if(crc & 1u)
      {
        crc = (uint16_t)((crc >> 1) ^ 0xA001u);
      }
      else
      {
        crc >>= 1;
      }
    }
  }
  return crc;
}

/**
* @brief   逐段更新CRC32
* @param   crc   上一段的中间值(首段传FW_IMG_CRC32_INIT)
* @param   data  数据指针
* @param   len   字节数
* @return  新的中间值；最终值需再经FW_IMG_CRC32_FINAL()取反
* @note    标准反射CRC32，多项式0xEDB88320，与上位机Get-Crc32同一算法。
*/
uint32_t fw_img_crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
  if(data == NULL)
  {
    return crc;
  }
  for(uint32_t i = 0u; i < len; i++)
  {
    crc ^= data[i];
    for(uint32_t b = 0u; b < 8u; b++)
    {
      if(crc & 1u)
      {
        crc = (crc >> 1) ^ 0xEDB88320u;
      }
      else
      {
        crc >>= 1;
      }
    }
  }
  return crc;
}

/**
* @brief   解析镜像元数据并校验其自洽性
* @param   buf  镜像元数据原始字节
* @param   len  可用字节数
* @param   out  输出结构
* @return  FW_IMG_OK=合法，负值=格式不符
* @note    校验内容：长度、魔数、格式版本、头长、长度自洽与CRC16。
*/
int fw_img_meta_parse(const uint8_t *buf, uint32_t len, fw_img_meta_t *out)
{
  if(buf == NULL || out == NULL)
  {
    return -1;
  }
  if(len < FW_IMG_META_LEN)
  {
    return -1;
  }
  memset(out, 0, sizeof(*out));
  //按固定布局逐字段读取，全部小端
  out->magic = fw_img_rd_u32(buf);
  out->format_ver = buf[4];
  out->meta_len = fw_img_rd_u16(buf + 6u);
  out->image_len = fw_img_rd_u32(buf + 8u);
  out->image_ver = fw_img_rd_u32(buf + 12u);
  out->project_id = fw_img_rd_u32(buf + 16u);
  out->customer_id = fw_img_rd_u16(buf + 20u);
  out->batch_id = fw_img_rd_u16(buf + 22u);
  out->build_date = fw_img_rd_u32(buf + 24u);
  out->code_crc32 = fw_img_rd_u32(buf + FW_IMG_META_CRC32_OFS);
  //魔数、格式版本与头长必须与本实现一致
  if(out->magic != FW_IMG_META_MAGIC || out->format_ver != FW_IMG_META_FMT_VER)
  {
    return -1;
  }
  if(out->meta_len != FW_IMG_META_LEN)
  {
    return -1;
  }
  //镜像必须至少容纳镜像元数据本身
  if(out->image_len <= FW_IMG_META_LEN)
  {
    return -1;
  }
  //CRC覆盖前62字节，CRC本身在最后2字节
  if(fw_img_crc16(buf, FW_IMG_META_LEN - FW_IMG_META_CRC_LEN) !=
     fw_img_rd_u16(buf + FW_IMG_META_LEN - FW_IMG_META_CRC_LEN))
  {
    return -1;
  }
  return FW_IMG_OK;
}

/**
* @brief   构造镜像元数据并填入CRC16
* @param   meta  输入字段，image_len/image_ver/project_id/code_crc32必须已填好
* @param   buf  输出缓冲，至少FW_IMG_META_LEN字节
* @param   len  缓冲可用长度
* @return  FW_IMG_OK=成功，负值=参数非法
* @note    保留字段一律置0，CRC16覆盖前62字节；与fw_img_meta_parse互为逆操作。
*/
int fw_img_meta_build(const fw_img_meta_t *meta, uint8_t *buf, uint32_t len)
{
  if(meta == NULL || buf == NULL || len < FW_IMG_META_LEN)
  {
    return -1;
  }
  //整块清零，保留字段保持确定的0值
  memset(buf, 0, FW_IMG_META_LEN);
  fw_img_wr_u32(buf, FW_IMG_META_MAGIC);
  buf[4] = (uint8_t)FW_IMG_META_FMT_VER;
  buf[5] = 0u;
  fw_img_wr_u16(buf + 6u, (uint16_t)FW_IMG_META_LEN);
  fw_img_wr_u32(buf + 8u, meta->image_len);
  fw_img_wr_u32(buf + 12u, meta->image_ver);
  fw_img_wr_u32(buf + 16u, meta->project_id);
  fw_img_wr_u16(buf + 20u, meta->customer_id);
  fw_img_wr_u16(buf + 22u, meta->batch_id);
  fw_img_wr_u32(buf + 24u, meta->build_date);
  fw_img_wr_u32(buf + FW_IMG_META_CRC32_OFS, meta->code_crc32);
  //CRC16覆盖前62字节，最后2字节为CRC本身
  fw_img_wr_u16(buf + FW_IMG_META_LEN - FW_IMG_META_CRC_LEN,
                fw_img_crc16(buf, FW_IMG_META_LEN - FW_IMG_META_CRC_LEN));
  return FW_IMG_OK;
}

/**
* @brief   检查镜像的项目水印是否与本机一致
* @param   meta  已解析的镜像元数据
* @return  0=一致，非0=不符
*/
int fw_img_meta_project_ok(const fw_img_meta_t *meta)
{
  if(meta == NULL)
  {
    return -1;
  }
  return (meta->project_id == FW_IMG_PROJECT_ID) ? 0 : -1;
}

/**
* @brief   检查镜像版本是否不低于设备记录的最低版本
* @param   image_ver  镜像版本
* @param   min_ver    设备记录的最低允许版本
* @return  0=允许，非0=版本过低
*/
int fw_img_ver_ok(uint32_t image_ver, uint32_t min_ver)
{
  return (image_ver >= min_ver) ? 0 : -1;
}

/**
* @brief   在槽位内定位并解析镜像元数据
* @param   addr       槽位起始地址
* @param   slot_size  槽位大小
* @param   out        输出：解析结果
* @return  FW_IMG_OK=找到，负值=没有或无效
* @note    先按旧布局读槽位末尾64字节；未命中则从槽位起始逐字节扫描魔数，
*          定位紧贴代码末尾的镜像元数据。
*/
int fw_img_locate(uint32_t addr, uint32_t slot_size, fw_img_meta_t *out)
{
  uint8_t raw[FW_IMG_META_LEN];
  uint32_t off;
  int rc = FW_IMG_ERR_TRAILER;

  if(out == NULL || slot_size < FW_IMG_META_LEN)
  {
    return FW_IMG_ERR_TRAILER;
  }
  //兼容旧布局：镜像元数据固贴槽位末尾，先读末尾64字节，命中即最短路径
  if(fw_port_flash_read(addr + slot_size - FW_IMG_META_LEN, raw, FW_IMG_META_LEN) != 0)
  {
    return FW_IMG_ERR_READ;
  }
  //旧布局语义是元数据紧贴槽位末尾，逻辑总长必须正好等于槽位大小
  if(fw_img_meta_parse(raw, FW_IMG_META_LEN, out) == FW_IMG_OK && out->image_len == slot_size)
  {
    return FW_IMG_OK;
  }
  //当前布局：镜像元数据紧贴代码末尾，从槽位起始扫描魔数。
  //按块读入RAM后线性扫描，避免逐字节发起跨函数Flash读
  {
    uint8_t scan[256];
    uint32_t chunk;
    const uint32_t overlap = FW_IMG_META_LEN - 1u;   //块间保留重叠，跨块的魔数不会被漏掉

    off = FW_IMG_META_LEN;
    while((off + FW_IMG_META_LEN) <= slot_size)
    {
      chunk = slot_size - off;
      if(chunk > (uint32_t)sizeof(scan))
      {
        chunk = (uint32_t)sizeof(scan);
      }
      if(fw_port_flash_read(addr + off, scan, chunk) != 0)
      {
        return FW_IMG_ERR_READ;
      }
      for(uint32_t i = 0; (i + 4u) <= chunk; i++)
      {
        if(fw_img_rd_u32(scan + i) != FW_IMG_META_MAGIC)
        {
          continue;
        }
        //魔数命中后整块读入并做完整校验，解析失败则继续向后找
        if((off + i + FW_IMG_META_LEN) > slot_size)
        {
          continue;
        }
        if(fw_port_flash_read(addr + off + i, raw, FW_IMG_META_LEN) != 0)
        {
          return FW_IMG_ERR_READ;
        }
        if(fw_img_meta_parse(raw, FW_IMG_META_LEN, out) != FW_IMG_OK)
        {
          continue;
        }
        //头紧贴代码：逻辑总长必须正好落在头结束处，避免代码里的魔数误命中
        if(out->image_len != (off + i + FW_IMG_META_LEN))
        {
          continue;
        }
        return FW_IMG_OK;
      }
      //本块已覆盖到槽位末尾则结束，否则步进时保留重叠区
      if(chunk < (uint32_t)sizeof(scan))
      {
        break;
      }
      off += chunk - overlap;
    }
  }
  return rc;
}

/**
* @brief   校验一个槽位里的镜像
* @param   addr       槽位起始地址
* @param   slot_size  槽位大小
* @param   image_ver  输出：镜像版本，可为NULL
* @return  FW_IMG_OK=有效，负值=FW_IMG_ERR_*
*/
int fw_img_verify(uint32_t addr, uint32_t slot_size, uint32_t *image_ver)
{
  fw_img_meta_t meta;
  int rc;

  rc = fw_img_locate(addr, slot_size, &meta);
  if(rc != FW_IMG_OK)
  {
    return rc;
  }
  //水印：项目标识不符的镜像不属于本机
  if(fw_img_meta_project_ok(&meta) != 0)
  {
    return FW_IMG_ERR_PROJECT;
  }
  if(image_ver != NULL)
  {
    *image_ver = meta.image_ver;
  }
  return FW_IMG_OK;
}

/* 回读代码算CRC32时用的分块缓冲：用栈上缓冲，避免给库再加一块常驻静态区 */
#define FW_IMG_CRC32_CHUNK   256u

/**
* @brief   完整校验一个槽位里的镜像
* @param   addr       槽位起始地址
* @param   slot_size  槽位大小
* @param   image_ver  输出：镜像版本，可为NULL
* @return  FW_IMG_OK=有效，负值=FW_IMG_ERR_*
* @note    先定位镜像元数据与查水印，再把代码区从Flash读回来重新累加CRC32，
*          与元数据里的code_crc32比对。能发现写入不完整、Flash内容损坏这类
*          "向量表还在但内容已经不对"的情况。
*/
int fw_img_verify_full(uint32_t addr, uint32_t slot_size, uint32_t *image_ver)
{
  fw_img_meta_t meta;
  uint32_t crc = FW_IMG_CRC32_INIT;
  uint32_t code_len;
  uint32_t off = 0;
  uint8_t buf[FW_IMG_CRC32_CHUNK];
  int rc;

  rc = fw_img_locate(addr, slot_size, &meta);
  if(rc != FW_IMG_OK)
  {
    return rc;
  }
  if(fw_img_meta_project_ok(&meta) != 0)
  {
    return FW_IMG_ERR_PROJECT;
  }
  //镜像至少要能装下元数据本身
  if(meta.image_len <= FW_IMG_META_LEN)
  {
    return FW_IMG_ERR_TRAILER;
  }
  code_len = meta.image_len - FW_IMG_META_LEN;
  //分块读回代码区，边读边累加CRC32
  while(off < code_len)
  {
    uint32_t n = code_len - off;
    if(n > sizeof(buf))
    {
      n = sizeof(buf);
    }
    if(fw_port_flash_read(addr + off, buf, n) != 0)
    {
      return FW_IMG_ERR_READ;
    }
    crc = fw_img_crc32_update(crc, buf, n);
    off += n;
  }
  if(FW_IMG_CRC32_FINAL(crc) != meta.code_crc32)
  {
    return FW_IMG_ERR_CRC;
  }
  if(image_ver != NULL)
  {
    *image_ver = meta.image_ver;
  }
  return FW_IMG_OK;
}

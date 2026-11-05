/**
 * @file    fw_img.c
 * @brief   固件镜像元数据：解析、定位、水印校验、版本校验
 * @note    只处理明文镜像元数据；不依赖加密、哈希与签名模块。
 */

#include "fw_img.h"
#include <string.h>
#include "fw_port.h"   //fw_port_flash_read

//回读代码算CRC32时的分块缓冲：用栈上缓冲，避免给库再加一块常驻静态区。
//取128字节而非256：该函数的栈帧与fw_img_locate()的扫描缓冲同时存在，小块能把峰值压住
#define FW_IMG_CRC32_CHUNK   128u

/* 半字节查表(16项，64字节)：标准反射CRC32，多项式0xEDB88320。
 * 每字节分两次查表，等价于8次移位判断，结果与上位机一致。 */
static const uint32_t crc32_nibble_table[16] =
{
  0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
  0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
  0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
  0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu
};

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
    //每字节分两个半字节查表，替代8次移位判断
    crc ^= data[i];
    crc = (crc >> 4) ^ crc32_nibble_table[crc & 0x0Fu];
    crc = (crc >> 4) ^ crc32_nibble_table[crc & 0x0Fu];
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
* @brief   校验镜像的项目水印是否与本机一致
* @param   meta  已解析的镜像元数据
* @return  FW_IMG_OK=一致，FW_IMG_ERR_PROJECT=不符，FW_IMG_ERR_TRAILER=入参为空
*/
int fw_img_project_verify(const fw_img_meta_t *meta)
{
  if(meta == NULL)
  {
    return FW_IMG_ERR_TRAILER;
  }
  return (meta->project_id == FW_IMG_PROJECT_ID) ? FW_IMG_OK : FW_IMG_ERR_PROJECT;
}

/**
* @brief   校验镜像版本是否不低于设备记录的最低版本
* @param   image_ver  镜像版本
* @param   min_ver    设备记录的最低允许版本
* @return  FW_IMG_OK=允许，FW_IMG_ERR_VERSION=版本过低
*/
int fw_img_ver_verify(uint32_t image_ver, uint32_t min_ver)
{
  return (image_ver >= min_ver) ? FW_IMG_OK : FW_IMG_ERR_VERSION;
}

/**
* @brief   检查槽位起始处的向量表是否可用
* @param   addr  镜像在Flash中的起始地址
* @return  FW_IMG_OK=可用，负值=FW_IMG_ERR_*
* @note    只读向量表前8字节：初始栈顶必须落在RAM内，复位向量必须落在主Flash内。
*          该检查不依赖镜像元数据，用于元数据还没写入的早期阶段（IAP首帧、提交前）。
*/
int fw_img_vectors_ok(uint32_t addr)
{
  uint8_t vec[8];
  uint32_t msp;
  uint32_t reset_addr;

  //地址本身要落在主Flash内
  if(addr < FLASH_BASE_ADDR || addr >= (FLASH_BASE_ADDR + FLASH_TOTAL_SIZE))
  {
    return FW_IMG_ERR_ADDR;
  }
  //一次读齐初始栈顶与复位向量两项
  if(fw_port_flash_read(addr, vec, sizeof(vec)) != 0)
  {
    return FW_IMG_ERR_READ;
  }
  //按本机端序拷贝，向量表是MCU自己写进去的，不参与跨设备格式
  memcpy(&msp, vec, sizeof(msp));
  memcpy(&reset_addr, vec + sizeof(msp), sizeof(reset_addr));
  //初始栈顶必须落在RAM内
  if(msp < RAM_BASE_ADDR || msp >= RAM_END_ADDR)
  {
    return FW_IMG_ERR_VECTOR;
  }
  //复位向量必须落在主Flash内
  if(reset_addr < FLASH_BASE_ADDR || reset_addr >= (FLASH_BASE_ADDR + FLASH_TOTAL_SIZE))
  {
    return FW_IMG_ERR_VECTOR;
  }
  /* 复位向量的bit0必须为1：偶地址同样能通过上面的范围检查，但跳过去执行会立刻
     HardFault，而此时栈指针与VTOR已经切到新镜像，没有机会退回 */
  if((reset_addr & 1u) == 0u)
  {
    return FW_IMG_ERR_VECTOR;
  }
  return FW_IMG_OK;
}

/**
* @brief   在槽位内定位并解析镜像元数据
* @param   addr       槽位起始地址
* @param   slot_size  槽位大小
* @param   out        输出：解析结果
* @return  FW_IMG_OK=找到，负值=没有或无效
* @note    从槽位起始按块扫描紧贴代码末尾的当前布局元数据；扫描不到时才退回
*          槽位末尾那一份旧布局元数据。
*/
int fw_img_locate(uint32_t addr, uint32_t slot_size, fw_img_meta_t *out)
{
  uint8_t raw[FW_IMG_META_LEN];
  fw_img_meta_t tail_meta;
  uint32_t off;
  int tail_ok = 0;

  if(out == NULL || slot_size < FW_IMG_META_LEN)
  {
    return FW_IMG_ERR_TRAILER;
  }
  /* 先把槽位末尾64字节按旧布局解析一次：结果只留作扫描不着时的兜底，
     不能在这里直接返回。新镜像写在槽位前部、旧布局元数据还留在末尾时，
     直接采信末尾这一份会遮蔽前面的新镜像（按旧image_len算CRC必然不符） */
  if(fw_port_flash_read(addr + slot_size - FW_IMG_META_LEN, raw, FW_IMG_META_LEN) != 0)
  {
    return FW_IMG_ERR_READ;
  }
  //旧布局语义是元数据紧贴槽位末尾，逻辑总长必须正好等于槽位大小
  if(fw_img_meta_parse(raw, FW_IMG_META_LEN, &tail_meta) == FW_IMG_OK &&
     tail_meta.image_len == slot_size)
  {
    tail_ok = 1;
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
  //当前位置布局都没找到，才退回槽位末尾那一份旧布局元数据
  if(tail_ok)
  {
    *out = tail_meta;
    return FW_IMG_OK;
  }
  return FW_IMG_ERR_TRAILER;
}

/**
* @brief   校验一个槽位里的镜像元数据：定位、自洽性、项目水印
* @param   addr       槽位起始地址
* @param   slot_size  槽位大小
* @param   image_ver  输出：镜像版本，可为NULL
* @return  FW_IMG_OK=有效，负值=FW_IMG_ERR_*
* @note    只读到镜像元数据为止，不读代码区，耗时与镜像大小无关。
*          代码完整性由fw_img_crc32_verify()把关。
*/
int fw_img_meta_verify(uint32_t addr, uint32_t slot_size, uint32_t *image_ver)
{
  fw_img_meta_t meta;
  int rc;

  rc = fw_img_locate(addr, slot_size, &meta);
  if(rc != FW_IMG_OK)
  {
    return rc;
  }
  //水印：项目标识不符的镜像不属于本机
  if(fw_img_project_verify(&meta) != FW_IMG_OK)
  {
    return FW_IMG_ERR_PROJECT;
  }
  if(image_ver != NULL)
  {
    *image_ver = meta.image_ver;
  }
  return FW_IMG_OK;
}

/**
* @brief   完整校验一个槽位里的镜像：向量表、元数据、水印、代码CRC32
* @param   addr       槽位起始地址
* @param   slot_size  槽位大小
* @param   image_ver  输出：镜像版本，可为NULL
* @return  FW_IMG_OK=有效，负值=FW_IMG_ERR_*
* @note    先查向量表，再定位镜像元数据与查水印，最后把代码区从Flash读回来
*          重新累加CRC32，与元数据里的code_crc32比对。能发现写入不完整、
*          Flash内容损坏这类"向量表还在但内容已经不对"的情况。
*/
int fw_img_crc32_verify(uint32_t addr, uint32_t slot_size, uint32_t *image_ver)
{
  fw_img_meta_t meta;
  uint32_t crc = FW_IMG_CRC32_INIT;
  uint32_t code_len;
  uint32_t off = 0;
  uint8_t buf[FW_IMG_CRC32_CHUNK];
  int rc;

  //向量表不合格就没必要再读整片代码区
  rc = fw_img_vectors_ok(addr);
  if(rc != FW_IMG_OK)
  {
    return rc;
  }
  rc = fw_img_locate(addr, slot_size, &meta);
  if(rc != FW_IMG_OK)
  {
    return rc;
  }
  if(fw_img_project_verify(&meta) != FW_IMG_OK)
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

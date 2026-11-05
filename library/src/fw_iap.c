/**
 * @file    fw_iap.c
 * @brief   IAP协议解析与镜像写入
 * @note    只有HEX一种协议：START带64字节镜像元数据，DATA传代码，RESP应答。
 *          设备收满image_len-64字节后自动校验并提交，没有单独的结束帧。
 */

#include "fw_iap.h"
#include <string.h>
#include "fw_port.h"   //fw_port_send_buf
#include "fw_msg.h"

/* App构建与Boot构建都带串口升级，本文件两种构建都参与编译 */

/* 编译期保护：平台串口接收缓冲必须放得下最长的IAP帧 */
#if (FW_IAP_RX_FRAME_MAX > FW_PORT_RX_BUF_SIZE)
  #error "FW_PORT_RX_BUF_SIZE is too small for IAP frames"
#endif

//IAP响应接口：发送经平台适配层，本模块不直接调用平台串口函数
#define FW_IAP_SEND(buf, len)  fw_port_send_buf((const uint8_t *)(buf), (uint16_t)(len))

//IAP接收状态缓存：镜像元数据在START帧收到后缓存在这里，收满后写Flash末尾
static iap_page_cache_t cache = {0};

/**
 * @brief   IAP升级提交：在协议层校验通过后写入Boot标志
 * @param   image_len  镜像逻辑总长(代码+镜像元数据)
 * @return  0表示成功
 * @note    只置标志；镜像大小由镜像元数据携带，交换/覆写模式在BL侧按元数据取。
 */
static int fw_iap_commit_upgrade(uint32_t image_len)
{
  if(image_len == 0u)
  {
    return -1;//未接收到有效数据
  }
  if(fw_boot_flag_write(FW_BOOT_FLAG_VALID) != 0)
  {
    return -1;
  }
#if (FW_BOOT_UPGRADE_MODE == 1)
  /* 交换模式只有进度为NONE时才执行新交换；上一次回滚留下的ROLLED_BACK
     会让新镜像停在SLOT1不被换入SLOT0，提交时一并清掉 */
  if(fw_boot_progress_write(FW_SWAP_PROGRESS_NONE) != 0)
  {
    return -1;
  }
#endif
  fw_msg_iap_done(image_len);
  return 0;
}

/**
 * @brief   按升级模式确定本会话的写入目标地址
 * @return  目标槽位的起始地址
 * @note    覆写/交换模式写入SLOT1；XIP模式写入当前运行槽位的对侧槽位。
 */
static uint32_t fw_iap_write_target(void)
{
#if (FW_BOOT_UPGRADE_MODE == 2)
  return (fw_xip_get_run_slot() == FW_XIP_RUN_SLOT0) ? SLOT1_ADDR : SLOT0_ADDR;
#else
  return SLOT1_ADDR;
#endif
}

/**
 * @brief   组装并发送一个RESP帧(下位机→上位机)
 * @param   offset  位置(成功时为已确认写入位置，失败时为期望位置)
 * @param   status  状态码，0表示成功
 * @note    多字节字段小端；CRC16覆盖SOF到DATA结束，小端发送。
 */
static void fw_iap_hex_send_resp(uint32_t offset, uint8_t status)
{
  uint8_t buf[FW_IAP_HEX_HDR_LEN + 1u + FW_IAP_HEX_CRC_LEN];
  uint16_t crc;

  buf[0] = FW_IAP_HEX_SOF0;
  buf[1] = FW_IAP_HEX_SOF1;
  buf[2] = (uint8_t)((FW_IAP_HEX_VER << 4) | (FW_IAP_HEX_TYPE_RESP & 0x0Fu));
  //OFFSET小端
  buf[3] = (uint8_t)(offset & 0xFFu);
  buf[4] = (uint8_t)((offset >> 8) & 0xFFu);
  buf[5] = (uint8_t)((offset >> 16) & 0xFFu);
  buf[6] = (uint8_t)((offset >> 24) & 0xFFu);
  //LEN=1，DATA[0]=状态码
  buf[7] = 1u;
  buf[8] = 0u;
  buf[9] = status;
  crc = fw_img_crc16(buf, FW_IAP_HEX_HDR_LEN + 1u);
  buf[FW_IAP_HEX_HDR_LEN + 1u] = (uint8_t)(crc & 0xFFu);
  buf[FW_IAP_HEX_HDR_LEN + 2u] = (uint8_t)((crc >> 8) & 0xFFu);
  FW_IAP_SEND(buf, sizeof(buf));
}

/**
 * @brief   会话失败：作废会话并回RESP
 * @param   status  状态码
 */
static void fw_iap_hex_fail(uint8_t status)
{
  cache.received_size = 0u;
  cache.in_progress = 0u;
  fw_iap_hex_send_resp(0u, status);
}

/**
 * @brief   把一段代码写进本会话的目标槽位(每页首次写入前擦除一次)
 * @param   offset  该段数据在代码内的偏移
 * @param   data    数据指针
 * @param   len     数据长度
 * @return  0成功，非0失败
 * @note    数据按OFFSET递增写入，每页在本次会话首次写入时擦除一次，
 *          之后同页仅追加编程；页尾未覆盖部分保持擦除态。
 */
static int fw_iap_hex_write_data(uint32_t offset, const uint8_t *data, uint16_t len)
{
  uint32_t written = 0u;
  uint32_t frontier = cache.write_addr + cache.received_size;   //本会话已写入到的绝对地址

  while(written < len)
  {
    uint32_t addr = cache.write_addr + offset + written;
    uint32_t page_start = addr & ~((uint32_t)FW_FLASH_PAGE_SIZE - 1u);
    uint32_t n = (uint32_t)FW_FLASH_PAGE_SIZE - (addr - page_start);

    if(n > (uint32_t)(len - written))
    {
      n = (uint32_t)(len - written);
    }
    //本页在本次会话中尚未写入(即未擦除)：先擦除一次
    if(page_start >= frontier)
    {
      if(fw_port_flash_erase(page_start, FW_FLASH_PAGE_SIZE) != 0)
      {
        return -1;
      }
    }
    //仅编程本帧实际覆盖的字节，页尾未覆盖部分保持擦除态；
    //addr 与 n 可能不是写粒度整数倍，对齐由 fw_port_flash_write 内部按粒度读-改-写完成
    if(fw_port_flash_write(addr, data + written, n) != 0)
    {
      return -1;
    }
    written += n;
  }
  return 0;
}

/**
 * @brief   把缓存的镜像元数据写到代码末尾
 * @return  0成功，非0失败
 * @note    镜像元数据紧贴代码末尾，起始位置为写入槽位基址加代码长度。写之前先
 *          读出所在页，把镜像元数据覆盖到页内对应位置后整页回写，避免擦掉同页里已写入的代码尾部。
 */
static int fw_iap_meta_write(void)
{
  uint32_t meta_addr = cache.write_addr + cache.code_len;
  uint32_t meta_end = meta_addr + FW_IMG_META_LEN;
  uint32_t page_start = meta_addr & ~((uint32_t)FW_FLASH_PAGE_SIZE - 1u);
  uint32_t page_end = (meta_end + FW_FLASH_PAGE_SIZE - 1u) & ~((uint32_t)FW_FLASH_PAGE_SIZE - 1u);
  uint8_t page[FW_FLASH_PAGE_SIZE];

  for(uint32_t p = page_start; p < page_end; p += FW_FLASH_PAGE_SIZE)
  {
    uint32_t from = (meta_addr > p) ? meta_addr : p;
    uint32_t to = (meta_end < (p + FW_FLASH_PAGE_SIZE)) ? meta_end : (p + FW_FLASH_PAGE_SIZE);

    //整页读出，保留原有代码字节
    if(fw_port_flash_read(p, page, FW_FLASH_PAGE_SIZE) != 0)
    {
      return -1;
    }
    //把镜像元数据覆盖到页内对应位置
    memcpy(page + (from - p), cache.img_meta + (from - meta_addr), to - from);
    //擦除后整页回写
    if(fw_port_flash_erase(p, FW_FLASH_PAGE_SIZE) != 0)
    {
      return -1;
    }
    if(fw_port_flash_write(p, page, FW_FLASH_PAGE_SIZE) != 0)
    {
      return -1;
    }
  }
  return 0;
}

/**
 * @brief   取当前运行镜像所在槽位的起始地址
 * @return  槽位起始地址
 * @note    覆写/交换模式运行镜像在SLOT0；XIP模式在正在运行的槽位。
 *          整包与差分两条路径都用它定位"设备当前这一份固件"。
 */
static uint32_t fw_iap_old_slot(void)
{
#if (FW_BOOT_UPGRADE_MODE == 2)
  return (fw_xip_get_run_slot() == FW_XIP_RUN_SLOT0) ? SLOT0_ADDR : SLOT1_ADDR;
#else
  return SLOT0_ADDR;
#endif
}

/**
 * @brief   START帧的公共校验：解析镜像元数据、长度、水印、版本
 * @param   data  镜像元数据原始字节
 * @param   len   可用字节数
 * @return  FW_IAP_HEX_ST_OK或具体状态码
 */
static uint8_t fw_iap_start_common(const uint8_t *data, uint32_t len)
{
  uint32_t code_len;
  fw_img_meta_t cur_meta;

  if(len < FW_IMG_META_LEN)
  {
    return FW_IAP_HEX_ST_BAD_FRAME;
  }
  memcpy(cache.img_meta, data, FW_IMG_META_LEN);
  if(fw_img_meta_parse(cache.img_meta, FW_IMG_META_LEN, &cache.info) != FW_IMG_OK)
  {
    return FW_IAP_HEX_ST_IMAGE_INVALID;
  }
  //长度自洽：整体不能超过一个槽位，代码至少放得下向量表
  if(cache.info.image_len <= FW_IMG_META_LEN || cache.info.image_len > FW_IAP_IMAGE_MAX_SIZE)
  {
    return FW_IAP_HEX_ST_SIZE;
  }
  code_len = cache.info.image_len - FW_IMG_META_LEN;
  //代码段至少要放得下完整元数据区，否则收尾时fw_img_locate永远定位不到
  if(code_len < FW_IMG_META_LEN)
  {
    return FW_IAP_HEX_ST_SIZE;
  }
  //水印：项目标识不符的镜像不属于本机
  if(fw_img_project_verify(&cache.info) != FW_IMG_OK)
  {
    return FW_IAP_HEX_ST_HW_MISMATCH;
  }
  /* 版本单调：下限取"当前正在运行的那份固件"的版本，不另存历史水位。
   * 设备回滚到低版本后运行版本随之降低，试图刷到低于当前运行版本的行为会被拒绝。 */
  if((fw_img_locate(fw_iap_old_slot(), SLOT_SIZE, &cur_meta) == FW_IMG_OK) &&
     (cur_meta.image_ver != 0u) &&
     (fw_img_ver_verify(cache.info.image_ver, cur_meta.image_ver) != FW_IMG_OK))
  {
    return FW_IAP_HEX_ST_VERSION_OLD;
  }
  cache.code_len = code_len;
  cache.image_len = cache.info.image_len;
  return FW_IAP_HEX_ST_OK;
}

/**
 * @brief   处理START帧：整包升级
 * @param   data  64字节镜像元数据
 * @param   len   DATA长度
 */
static void fw_iap_hex_start(const uint8_t *data, uint16_t len)
{
  uint8_t st;

  //START只接受完整的64字节镜像元数据
  if(len != FW_IMG_META_LEN)
  {
    fw_iap_hex_fail(FW_IAP_HEX_ST_BAD_FRAME);
    return;
  }
  st = fw_iap_start_common(data, len);
  if(st != FW_IAP_HEX_ST_OK)
  {
    fw_iap_hex_fail(st);
    return;
  }
  //建立会话：目标槽位由升级模式决定
  cache.pack_mode = 0u;
  cache.write_addr = fw_iap_write_target();
  cache.received_size = 0u;
  cache.code_crc = FW_IMG_CRC32_INIT;   //代码CRC32从初值开始累加
  cache.percent = 0u;
  cache.in_progress = 1u;
  fw_msg_iap_start(cache.image_len);
  fw_iap_hex_send_resp(0u, FW_IAP_HEX_ST_OK);
}

#if FW_IAP_DELTA_ENABLE

/**
 * @brief   校验旧镜像的CRC32是否与差分基版本一致
 * @param   base_crc32    差分包声明的旧镜像CRC32
 * @param   old_code_len  输出：旧镜像代码长度
 * @return  1=一致，0=不一致或旧镜像无效
 * @note    直接比对旧镜像元数据里登记的代码CRC32，元数据自身有CRC16保护，
 *          不需要重读整个代码区；代码区若已损坏，会在收满后的整体校验阶段
 *          以代码CRC32不符拒绝提交，不会把坏镜像置为有效。
 */
static int fw_iap_base_check(uint32_t base_crc32, uint32_t *old_code_len)
{
  fw_img_meta_t old_meta;
  uint32_t old_addr = fw_iap_old_slot();

  {
    if(fw_img_locate(old_addr, SLOT_SIZE, &old_meta) != FW_IMG_OK)
    {
      return 0;
    }
  }
  if(old_meta.image_len <= FW_IMG_META_LEN || old_meta.image_len > SLOT_SIZE)
  {
    return 0;
  }
  if(old_meta.code_crc32 != base_crc32)
  {
    return 0;
  }
  *old_code_len = old_meta.image_len - FW_IMG_META_LEN;
  return 1;
}

/**
 * @brief   差分解码器的读旧镜像回调
 * @param   user    会话缓存
 * @param   offset  代码区偏移
 * @param   buf     输出缓冲区
 * @param   len     读取字节数
 * @return  0成功，非0失败
 */
static int fw_iap_delta_read(void *user, uint32_t offset, uint8_t *buf, uint32_t len)
{
  iap_page_cache_t *c = (iap_page_cache_t *)user;
  return fw_port_flash_read(c->old_addr + offset, buf, len);
}

/**
 * @brief   差分解码器的写新镜像回调
 * @param   user    会话缓存
 * @param   offset  代码区偏移
 * @param   buf     数据
 * @param   len     字节数
 * @return  0成功，非0失败
 * @note    边写边累加新镜像CRC32；按页首次擦除后顺序写入。
 */
static int fw_iap_delta_write(void *user, uint32_t offset, const uint8_t *buf, uint32_t len)
{
  iap_page_cache_t *c = (iap_page_cache_t *)user;
  uint32_t written = 0u;

  //代码CRC32按重建出的字节顺序累加
  c->code_crc = fw_img_crc32_update(c->code_crc, buf, len);
  while(written < len)
  {
    uint32_t addr = c->write_addr + offset + written;
    uint32_t page = addr & ~((uint32_t)FW_FLASH_PAGE_SIZE - 1u);
    uint32_t n = (uint32_t)FW_FLASH_PAGE_SIZE - (addr - page);

    if(n > (len - written))
    {
      n = len - written;
    }
    //本页在本次会话中尚未写入(即未擦除)：先擦除一次
    if(page >= c->out_frontier)
    {
      if(fw_port_flash_erase(page, FW_FLASH_PAGE_SIZE) != 0)
      {
        return -1;
      }
      c->out_frontier = page + FW_FLASH_PAGE_SIZE;
    }
    //addr 与 n 可能不是写粒度整数倍，对齐由 fw_port_flash_write 内部按粒度读-改-写完成
    if(fw_port_flash_write(addr, buf + written, n) != 0)
    {
      return -1;
    }
    written += n;
  }
  return 0;
}

/**
 * @brief   处理START_DELTA帧：校验基版本并建立差分会话
 * @param   data  64字节镜像元数据 + base_crc32(4) + delta_len(4)
 * @param   len   DATA长度
 */
static void fw_iap_hex_start_delta(const uint8_t *data, uint16_t len)
{
  uint32_t base_crc32;
  uint32_t delta_len;
  uint32_t old_code_len = 0u;
  uint8_t st;

  if(len != (uint16_t)(FW_IMG_META_LEN + 8u))
  {
    fw_iap_hex_fail(FW_IAP_HEX_ST_BAD_FRAME);
    return;
  }
  st = fw_iap_start_common(data, FW_IMG_META_LEN);
  if(st != FW_IAP_HEX_ST_OK)
  {
    fw_iap_hex_fail(st);
    return;
  }
  //附加字段全部小端
  base_crc32 = (uint32_t)data[FW_IMG_META_LEN] |
               ((uint32_t)data[FW_IMG_META_LEN + 1u] << 8) |
               ((uint32_t)data[FW_IMG_META_LEN + 2u] << 16) |
               ((uint32_t)data[FW_IMG_META_LEN + 3u] << 24);
  delta_len = (uint32_t)data[FW_IMG_META_LEN + 4u] |
              ((uint32_t)data[FW_IMG_META_LEN + 5u] << 8) |
              ((uint32_t)data[FW_IMG_META_LEN + 6u] << 16) |
              ((uint32_t)data[FW_IMG_META_LEN + 7u] << 24);
  if(delta_len == 0u || delta_len > FW_IAP_IMAGE_MAX_SIZE)
  {
    fw_iap_hex_fail(FW_IAP_HEX_ST_SIZE);
    return;
  }
  //旧镜像必须与差分基版本一致
  if(fw_iap_base_check(base_crc32, &old_code_len) == 0)
  {
    fw_iap_hex_fail(FW_IAP_HEX_ST_BASE_MISMATCH);
    return;
  }
  //建立差分会话
  cache.pack_mode = 1u;
  cache.delta_len = delta_len;
  cache.write_addr = fw_iap_write_target();
  cache.old_addr = fw_iap_old_slot();
  cache.out_frontier = cache.write_addr;
  cache.received_size = 0u;
  cache.code_crc = FW_IMG_CRC32_INIT;
  cache.percent = 0u;
  cache.in_progress = 1u;
  if(fw_delta_init(&cache.delta, cache.code_len, old_code_len,
                   fw_iap_delta_read, fw_iap_delta_write, &cache) != FW_DELTA_OK)
  {
    fw_iap_hex_fail(FW_IAP_HEX_ST_DELTA_FORMAT);
    return;
  }
  fw_msg_iap_start(cache.image_len);
  fw_iap_hex_send_resp(0u, FW_IAP_HEX_ST_OK);
}

#endif /* FW_IAP_DELTA_ENABLE */

/**
 * @brief   收满后的校验与提交
 * @note    依次检查向量表、代码CRC32、写镜像元数据、定位复核镜像元数据、写升级标志，
 *          全部通过后记录镜像版本作为下次升级的下限。
 */
static void fw_iap_hex_finish(void)
{
  //向量表：栈顶落在RAM、复位向量落在Flash
  if(fw_img_vectors_ok(cache.write_addr) != FW_IMG_OK)
  {
    fw_iap_hex_fail(FW_IAP_HEX_ST_IMAGE_INVALID);
    return;
  }
  //代码CRC32：与镜像元数据里登记的值比对
  if(FW_IMG_CRC32_FINAL(cache.code_crc) != cache.info.code_crc32)
  {
    fw_iap_hex_fail(FW_IAP_HEX_ST_CODE_CRC);
    return;
  }
  //镜像元数据写代码末尾
  if(fw_iap_meta_write() != 0)
  {
    fw_iap_hex_fail(FW_IAP_HEX_ST_WRITE_FAIL);
    return;
  }
  //从Flash重新定位并校验镜像元数据，确认落盘内容可读且水印正确
  if(fw_img_meta_verify(cache.write_addr, SLOT_SIZE, NULL) != FW_IMG_OK)
  {
    fw_iap_hex_fail(FW_IAP_HEX_ST_IMAGE_INVALID);
    return;
  }
  //提交
  if(fw_iap_commit_upgrade(cache.image_len) != 0)
  {
    fw_iap_hex_fail(FW_IAP_HEX_ST_COMMIT_FAIL);
    return;
  }
  cache.in_progress = 0u;
  cache.percent = 100u;
  fw_iap_hex_send_resp(cache.received_size, FW_IAP_HEX_ST_OK);
}

#if FW_IAP_DELTA_ENABLE
/**
 * @brief   差分收满后的校验与提交
 * @note    依次检查记录流完整性、代码CRC32、写镜像元数据、定位复核、写升级标志。
 */
static void fw_iap_hex_delta_finish(void)
{
  //记录流必须完整，重建长度必须等于镜像声明的代码长度
  if(fw_delta_finish(&cache.delta) != FW_DELTA_OK)
  {
    fw_iap_hex_fail(FW_IAP_HEX_ST_DELTA_FORMAT);
    return;
  }
  //代码CRC32：与镜像元数据里登记的值比对
  if(FW_IMG_CRC32_FINAL(cache.code_crc) != cache.info.code_crc32)
  {
    fw_iap_hex_fail(FW_IAP_HEX_ST_CODE_CRC);
    return;
  }
  //镜像元数据写代码末尾
  if(fw_iap_meta_write() != 0)
  {
    fw_iap_hex_fail(FW_IAP_HEX_ST_WRITE_FAIL);
    return;
  }
  //从Flash重新定位并校验镜像元数据
  if(fw_img_meta_verify(cache.write_addr, SLOT_SIZE, NULL) != FW_IMG_OK)
  {
    fw_iap_hex_fail(FW_IAP_HEX_ST_IMAGE_INVALID);
    return;
  }
  //提交
  if(fw_iap_commit_upgrade(cache.image_len) != 0)
  {
    fw_iap_hex_fail(FW_IAP_HEX_ST_COMMIT_FAIL);
    return;
  }
  cache.in_progress = 0u;
  cache.percent = 100u;
  fw_iap_hex_send_resp(cache.received_size, FW_IAP_HEX_ST_OK);
}
#endif /* FW_IAP_DELTA_ENABLE */

/**
 * @brief   处理DATA帧：写Flash，收满即校验并提交
 * @param   offset  本帧在代码内的偏移
 * @param   data    数据指针
 * @param   len     数据长度
 */
static void fw_iap_hex_data(uint32_t offset, const uint8_t *data, uint16_t len)
{
  if(len == 0u)
  {
    fw_iap_hex_send_resp(cache.received_size, FW_IAP_HEX_ST_SIZE);
    return;
  }
  //会话已结束：末帧重传只回RESP，不重复写Flash。
  //用减法判等，避免offset为大值时offset+len溢出后被误判成合法重传
  if(cache.in_progress == 0u)
  {
    if(cache.received_size > 0u && offset <= cache.received_size
       && (cache.received_size - offset) == (uint32_t)len)
    {
      fw_iap_hex_send_resp(cache.received_size, FW_IAP_HEX_ST_OK);
      return;
    }
    fw_iap_hex_send_resp(cache.received_size, FW_IAP_HEX_ST_NOT_STARTED);
    return;
  }
  //重传：帧尾恰好落在写入前沿，只回RESP，不重复写入
  if(offset <= cache.received_size && (cache.received_size - offset) == (uint32_t)len)
  {
    fw_iap_hex_send_resp(cache.received_size, FW_IAP_HEX_ST_OK);
    return;
  }
  //OFFSET必须与已写入长度严格衔接
  if(offset != cache.received_size)
  {
    fw_iap_hex_send_resp(cache.received_size, FW_IAP_HEX_ST_OFFSET_ERROR);
    return;
  }
#if FW_IAP_DELTA_ENABLE
  //差分模式：received_size是差分流偏移，喂给解码器，由回调写Flash
  if(cache.pack_mode == 1u)
  {
    if((cache.received_size + (uint32_t)len) > cache.delta_len)
    {
      fw_iap_hex_send_resp(cache.received_size, FW_IAP_HEX_ST_SIZE);
      return;
    }
    if(fw_delta_feed(&cache.delta, data, len) != FW_DELTA_OK)
    {
      fw_iap_hex_fail(FW_IAP_HEX_ST_DELTA_FORMAT);
      return;
    }
    cache.received_size += len;
    cache.percent = (uint8_t)((cache.received_size * 100u) / cache.delta_len);
    if(cache.received_size == cache.delta_len)
    {
      fw_iap_hex_delta_finish();
      return;
    }
    fw_iap_hex_send_resp(cache.received_size, FW_IAP_HEX_ST_OK);
    return;
  }
#endif
  //长度保护：不得超过START声明的代码长度
  if((cache.received_size + (uint32_t)len) > cache.code_len)
  {
    fw_iap_hex_send_resp(cache.received_size, FW_IAP_HEX_ST_SIZE);
    return;
  }
  //写Flash
  if(fw_iap_hex_write_data(offset, data, len) != 0)
  {
    fw_iap_hex_fail(FW_IAP_HEX_ST_WRITE_FAIL);
    return;
  }
  //首帧落盘后确认向量表可用，尽早区分传输错误与镜像错误
  if(offset == 0u && fw_img_vectors_ok(cache.write_addr) != FW_IMG_OK)
  {
    fw_iap_hex_fail(FW_IAP_HEX_ST_IMAGE_INVALID);
    return;
  }
  //边收边算代码CRC32，收满后与镜像元数据比对
  cache.code_crc = fw_img_crc32_update(cache.code_crc, data, len);
  cache.received_size += len;
  cache.percent = (uint8_t)((cache.received_size * 100u) / cache.code_len);
  //收满即进入校验与提交
  if(cache.received_size == cache.code_len)
  {
    fw_iap_hex_finish();
    return;
  }
  fw_iap_hex_send_resp(cache.received_size, FW_IAP_HEX_ST_OK);
}

/**
 * @brief   处理一个已通过CRC校验的HEX协议帧
 * @param   f  完整帧(至少11字节，长度由LEN字段决定)
 */
static void fw_iap_hex_handle_frame(uint8_t *f)
{
  uint8_t ver_type = f[2];
  uint8_t type = (uint8_t)(ver_type & 0x0Fu);
  //OFFSET与LEN均为小端
  uint32_t offset = (uint32_t)f[3] | ((uint32_t)f[4] << 8) |
                    ((uint32_t)f[5] << 16) | ((uint32_t)f[6] << 24);
  uint16_t len = (uint16_t)((uint16_t)f[7] | ((uint16_t)f[8] << 8));
  const uint8_t *data = f + FW_IAP_HEX_HDR_LEN;

  //版本号不符：帧本身可信(CRC已通过)，回RESP而非静默丢弃
  if((ver_type >> 4) != FW_IAP_HEX_VER)
  {
    fw_iap_hex_send_resp(cache.received_size, FW_IAP_HEX_ST_BAD_FRAME);
    return;
  }
  if(type == FW_IAP_HEX_TYPE_START)
  {
    //START的OFFSET必须为0
    if(offset != 0u)
    {
      fw_iap_hex_send_resp(cache.received_size, FW_IAP_HEX_ST_BAD_FRAME);
      return;
    }
    fw_iap_hex_start(data, len);
    return;
  }
  if(type == FW_IAP_HEX_TYPE_START_DELTA)
  {
    //START_DELTA的OFFSET必须为0
    if(offset != 0u)
    {
      fw_iap_hex_send_resp(cache.received_size, FW_IAP_HEX_ST_BAD_FRAME);
      return;
    }
#if FW_IAP_DELTA_ENABLE
    fw_iap_hex_start_delta(data, len);
#else
    //BL没有旧镜像可作基版本，明确回不支持，让上位机改发整包
    fw_iap_hex_send_resp(cache.received_size, FW_IAP_HEX_ST_DELTA_UNSUPPORT);
#endif
    return;
  }
  if(type == FW_IAP_HEX_TYPE_DATA)
  {
    fw_iap_hex_data(offset, data, len);
    return;
  }
  //上位机不应发送RESP帧，直接忽略
  if(type == FW_IAP_HEX_TYPE_RESP)
  {
    return;
  }
  fw_iap_hex_send_resp(cache.received_size, FW_IAP_HEX_ST_BAD_FRAME);
}

/**
* @brief   查询接收进度
* @param   total_size     输出：镜像逻辑总长(含镜像元数据)
* @param   received_size  输出：已接收字节数
* @param   percent        输出：进度百分比
* @return  会话状态
*/
iap_state_t fw_iap_get_status(uint32_t *total_size, uint32_t *received_size, uint8_t *percent)
{
  *total_size = cache.image_len;
  *received_size = cache.received_size;
  *percent = cache.percent;
  if(cache.in_progress != 0u)
  {
    return FW_IAP_RUNNING;
  }
  //差分会话的received_size是差分流偏移，与code_len量纲不同，按delta_len判完成
  if(cache.received_size > 0u
     && cache.received_size >= ((cache.pack_mode == 1u) ? cache.delta_len : cache.code_len))
  {
    return FW_IAP_DONE;
  }
  *percent = 0u;   //空闲：无会话，进度按0上报
  return FW_IAP_IDLE;
}

/**
* @brief   解析HEX协议的字节流
* @param   buf  本次收到的字节，就地解析(下位机App里就是串口接收缓冲)
* @param   len  字节数
* @note    在buf中查找SOF，按LEN取整帧并校验CRC16后处理，粘帧/脏字节/坏帧均可自同步；
*          帧未收全或校验不通过则丢弃，由上位机超时重传。
*/
void fw_iap_parse_hex_frame(uint8_t *buf, uint32_t len)
{
  uint32_t i;
  uint32_t have;
  uint16_t payload_len;
  uint32_t frame_len;
  uint16_t crc;

  if(buf == NULL || len == 0u)
  {
    return;//参数非法
  }
  have = len;

  for(;;)
  {
    //1.在缓冲中查找帧头SOF
    for(i = 0; (i + 1u) < have; i++)
    {
      if(buf[i] == FW_IAP_HEX_SOF0 && buf[i + 1u] == FW_IAP_HEX_SOF1)
      {
        break;
      }
    }
    if((i + 1u) >= have)
    {
      break;
    }
    if(i > 0u)
    {
      //丢弃SOF之前的无效字节
      have -= i;
      memmove(buf, buf + i, have);
    }
    //2.帧头是否收全
    if(have < FW_IAP_HEX_HDR_LEN)
    {
      break;
    }
    payload_len = (uint16_t)((uint16_t)buf[7] | ((uint16_t)buf[8] << 8));
    if(payload_len > FW_IAP_HEX_MAX_PAYLOAD)
    {
      //长度字段非法：跳过一个字节重新查找SOF
      have--;
      memmove(buf, buf + 1u, have);
      continue;
    }
    frame_len = FW_IAP_HEX_HDR_LEN + (uint32_t)payload_len + FW_IAP_HEX_CRC_LEN;
    if(have < frame_len)
    {
      break;   //本帧未收全：丢弃，由上位机超时重传
    }
    //3.CRC16校验(覆盖SOF..DATA结束)，小端
    crc = fw_img_crc16(buf, FW_IAP_HEX_HDR_LEN + (uint32_t)payload_len);
    if(crc != (uint16_t)((uint16_t)buf[FW_IAP_HEX_HDR_LEN + payload_len] |
                         ((uint16_t)buf[FW_IAP_HEX_HDR_LEN + payload_len + 1u] << 8)))
    {
      //坏帧：丢弃并从该SOF之后重新查找
      have--;
      memmove(buf, buf + 1u, have);
      continue;
    }
    //4.处理本帧
    fw_iap_hex_handle_frame(buf);
    //5.移除已处理的帧，继续处理缓冲中剩余字节
    have -= frame_len;
    if(have == 0u)
    {
      break;
    }
    memmove(buf, buf + frame_len, have);
  }
}


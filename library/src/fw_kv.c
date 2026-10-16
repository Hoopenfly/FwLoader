#include "fw_kv.h"
#include "fw_assert.h"
#include "fw_port.h"    //fw_port_kv_crit_lock/unlock：临界区由平台层提供
#include <stddef.h>
#include <string.h>

/* ==================== 日志回调 ==================== */
//库里所有日志都经这个回调报出去，没有注册就什么都不做
static fw_kv_log_fn_t fw_kv_log = NULL;
#define FW_KV_LOG(evt, kv, a, b, c)		do{ if(fw_kv_log != NULL) fw_kv_log((evt), (kv), (kv)->name, (a), (b), (c)); }while(0)

/**
* @brief  注册日志回调
* @param  fn  回调函数，传NULL取消注册
* @note   库不依赖任何日志模块，注册前所有日志调用都是空操作
*/
void fw_kv_set_log(fw_kv_log_fn_t fn)
{
	fw_kv_log = fn;
}

/* ==================== 临界区 ==================== */
/* 保护方式由平台适配层提供，内核不关心是关中断还是互斥锁(见fw_port.h) */
#define FW_KV_LOCK()			fw_port_kv_crit_lock()
#define FW_KV_UNLOCK()			fw_port_kv_crit_unlock()

//扇区头结构，固定16字节
typedef struct
{
  uint32_t magic;				//扇区有效标记，固定为FW_KV_SECTOR_MAGIC
  uint32_t seq;					//扇区序号，单调递增，用于识别最新扇区
  uint32_t status;			//扇区状态：EMPTY/ACTIVE/FULL
  uint32_t reserved;		//保留字段(不使用)，保持擦除态
}wl_sector_hdr_t;

/* 扇区头字段顺序说明(按写入先后排列，写入是从低地址到高地址逐字进行的)：
 *   1) magic   ：最先写，用来识别fw_kv扇区
 *   2) seq     ：扇区序号，决定新旧
 *   3) status  ：最后写，充当提交标志：仅当它写入成功，扇区才有效
 *   4) reserved：不使用，保持擦除态(因此只写前12字节)
 * 这样安排的掉电语义：扇区头只写了一半时status仍是擦除态，
 * fw_kv_sector_is_valid()判为无效，该扇区会被当作空扇区重新擦除，
 * 不会被误判为有效扇区加垃圾序号；magic/seq置于前面也便于调试时识别。
 */

//KV节点头结构，固定16字节，后跟value，最后跟4字节CRC
typedef struct
{
  uint32_t magic;				//节点有效标记，固定为FW_KV_NODE_MAGIC
  uint32_t key;					//键（32位整数）
  uint32_t value_len;		//值长度（字节）
  uint32_t status;			//节点状态：VALID/DELETED
}wl_node_hdr_t;

//GC时暂存有效记录的缓冲区
static uint8_t gc_buf[FW_KV_GC_BUF_SIZE];

/* 常驻key映射(key→最新记录位置)位于实例内(见fw_kv.h的fw_kv_t.key_map)：
 * get与GC均直接查询该映射，不逐条遍历整个分区。 */

/**
* @brief  重置GC键映射
* @param  kv  KV实例
*/
static void fw_kv_key_map_reset(fw_kv_t *kv)
{
  kv->key_count = 0;
  kv->key_map_ok = 1;
}

/**
* @brief  向GC键映射中插入或更新一个key的位置
* @param  key     键值
* @param  seq     所在扇区序号
* @param  sector  所在扇区索引
* @param  offset  在扇区内的偏移
* @note   同一key只保留seq最大（最新）的记录
*/
static void fw_kv_key_map_put(fw_kv_t *kv, uint32_t key, uint32_t seq, uint32_t sector, uint32_t offset)
{
  //查找是否已有该key的记录
  for(uint32_t i = 0; i < kv->key_count; i++)
  {
    if(kv->key_map[i].key == key)
    {
      //保留seq更大（更新）的记录
      if(seq > kv->key_map[i].seq ||
      (seq == kv->key_map[i].seq && offset > kv->key_map[i].offset))
      {
        kv->key_map[i].seq = seq;
        kv->key_map[i].sector = sector;
        kv->key_map[i].offset = offset;
      }
      return;
    }
  }

  //新key，插入映射表
  if(kv->key_count < FW_KV_MAX_KEYS)
  {
    kv->key_map[kv->key_count].key = key;
    kv->key_map[kv->key_count].seq = seq;
    kv->key_map[kv->key_count].sector = sector;
    kv->key_map[kv->key_count].offset = offset;
    kv->key_count++;
  }
  else
  {
    kv->key_map_ok = 0;  //key数量超限，后续回退到全扫描
  }
  //映射表下标不得越过数组边界，越界写会破坏实例内存
  FW_ASSERT(kv->key_count <= FW_KV_MAX_KEYS);
}

/**
* @brief  判断(sector, offset)处记录是否是该key的最新一条记录
* @param  key     键值
* @param  sector  扇区索引
* @param  offset  扇区内偏移
* @return 1=是最新记录，0=不是
*/
static int fw_kv_key_is_newest(fw_kv_t *kv, uint32_t key, uint32_t sector, uint32_t offset)
{
  for(uint32_t i = 0; i < kv->key_count; i++)
  {
    if(kv->key_map[i].key == key)
    {
      return(kv->key_map[i].sector == sector && kv->key_map[i].offset == offset);
    }
  }
  return 0;
}

/* ==================== 基础工具 ==================== */

/**
* @brief  FNV-1a哈希更新函数
* @param  h     当前哈希值
* @param  data  待哈希的数据指针
* @param  len   数据长度
* @return 更新后的哈希值
* @note   用于节点CRC校验
*/
static uint32_t fw_kv_fnv1a_update(uint32_t h, const uint8_t *data, uint32_t len)
{
  for(uint32_t i = 0; i < len; i++)
  {
    h ^= data[i];
    h *= 0x01000193u;  //FNV-1a质数
  }
  return h;
}

/**
* @brief  计算指定扇区的起始地址
* @param  kv     KV实例指针
* @param  index  扇区索引
* @return 扇区起始绝对地址
*/
static uint32_t fw_kv_sector_addr(fw_kv_t *kv, uint32_t index)
{
  /* 扇区索引越界会计算出其它分区的地址，擦写操作将破坏该分区数据。
   * 该索引由库内部推导，调用方无法传入，故以断言守住边界 */
  FW_ASSERT(index < kv->sector_count);
  return kv->base + index * kv->sector_size;
}

/**
* @brief  读取扇区头
* @param  kv     KV实例指针
* @param  index  扇区索引
* @param  hdr    输出扇区头指针
* @return 0成功，非0失败
*/
static int fw_kv_sector_read_hdr(fw_kv_t *kv, uint32_t index, wl_sector_hdr_t *hdr)
{
  if(kv->ops.read(fw_kv_sector_addr(kv, index), (uint8_t *)hdr, sizeof(*hdr)) != 0)
  {
    return FW_KV_ERR_FLASH;
  }
  return FW_KV_OK;
}

/**
* @brief  判断扇区头是否有效
* @param  hdr  扇区头指针
* @return 1=有效，0=无效
*/
static int fw_kv_sector_is_valid(const wl_sector_hdr_t *hdr)
{
  return (hdr->magic == FW_KV_SECTOR_MAGIC) &&
         (hdr->status == FW_KV_SECTOR_ACTIVE || hdr->status == FW_KV_SECTOR_FULL);
}

/**
* @brief  擦除整个扇区
* @param  kv     KV实例指针
* @param  index  扇区索引
* @return 0成功，非0失败
*/
static int fw_kv_sector_erase(fw_kv_t *kv, uint32_t index)
{
  if(kv->ops.erase(fw_kv_sector_addr(kv, index), kv->sector_size) != 0)
  {
    return FW_KV_ERR_FLASH;
  }

  return FW_KV_OK;
}


/**
* @brief  判断扇区是否整体处于擦除态(全0)
* @param  kv         KV实例指针
* @param  sec_index  扇区索引
* @return 1=整体已擦除，0=还有已编程的字
* @note   用于跳过不必要的扇区擦除：GC的搬迁目标(备用扇区)正常情况下
*         就是上一次GC擦干净的空扇区，可直接写入，无需再擦除(省一半擦除)
*/
static int fw_kv_sector_is_erased(fw_kv_t *kv, uint32_t sec_index)
{
  uint32_t word = FW_ERASED_WORD;
  uint32_t addr = fw_kv_sector_addr(kv, sec_index);

  for(uint32_t off = 0; off < kv->sector_size; off += sizeof(uint32_t))
  {
    if(kv->ops.read(addr + off, (uint8_t *)&word, sizeof(word)) != 0)
    {
      return 0;//读失败：按"需要擦除"处理，最坏只是多擦一次
    }
    if(word != FW_ERASED_WORD)
    {
      return 0;
    }
  }
  return 1;
}


/**
* @brief  写入扇区头
* @param  kv           KV实例指针
* @param  index        扇区索引
* @param  seq          扇区序号
* @param  status       扇区状态（ACTIVE/FULL）
* @return 0成功，非0失败
* @note   只写前12字节(magic/seq/status)，reserved保留不动：
*         既少编程1个字，又使status成为真正的提交标志
*/
static int fw_kv_sector_write_hdr(fw_kv_t *kv, uint32_t index, uint32_t seq, uint32_t status)
{
  wl_sector_hdr_t hdr;

  hdr.magic = FW_KV_SECTOR_MAGIC;
  hdr.seq = seq;
  hdr.status = status;
  hdr.reserved = FW_ERASED_WORD;  //保留字段：保持擦除态

  if(kv->ops.write(fw_kv_sector_addr(kv, index), (const uint8_t *)&hdr,
          sizeof(wl_sector_hdr_t) - sizeof(uint32_t)) != 0)
  {
    return FW_KV_ERR_FLASH;
  }
  return FW_KV_OK;
}

/* ==================== KV 节点扫描 ==================== */

/**
* @brief  读取节点头
* @param  kv         KV实例指针
* @param  sec_index  扇区索引
* @param  offset     扇区内偏移
* @param  hdr        输出节点头指针
* @return 0成功，非0失败
*/
static int fw_kv_node_read_hdr(fw_kv_t *kv, uint32_t sec_index, uint32_t offset, wl_node_hdr_t *hdr)
{
  if(kv->ops.read(fw_kv_sector_addr(kv, sec_index) + offset, (uint8_t *)hdr, sizeof(*hdr)) != 0)
  {
    return FW_KV_ERR_FLASH;
  }
  return FW_KV_OK;
}

/**
* @brief  校验节点CRC
* @param  kv         KV实例指针
* @param  sec_index  扇区索引
* @param  offset     扇区内偏移
* @param  hdr        节点头指针
* @return 1=校验通过，0=校验失败
* @note   CRC计算范围：key + value_len + status + value（不含magic）
*/
static int fw_kv_node_crc_ok(fw_kv_t *kv, uint32_t sec_index, uint32_t offset, const wl_node_hdr_t *hdr)
{
  uint32_t addr = fw_kv_sector_addr(kv, sec_index) + offset;
  uint32_t h = 0x811C9DC5u;//FNV-1a初始值
  uint32_t left = hdr->value_len;
  uint32_t pos = sizeof(wl_node_hdr_t);
  uint32_t stored;
  uint8_t chunk[64];
  /* 跳过magic，校验key/value_len/status共12字节 */
  h = fw_kv_fnv1a_update(h, (const uint8_t *)hdr + sizeof(hdr->magic), sizeof(hdr->key) + sizeof(hdr->value_len) + sizeof(hdr->status));
  //校验value
  while(left > 0)
  {
    uint32_t n = (left > sizeof(chunk)) ? sizeof(chunk) : left;
    if(kv->ops.read(addr + pos, chunk, n) != 0)
    {
      return 0;
    }
    h = fw_kv_fnv1a_update(h, chunk, n);
    pos += n;
    left -= n;
  }
  //读取存储的CRC并比较
  if(kv->ops.read(addr + pos, (uint8_t *)&stored, sizeof(stored)) != 0)
  {
    return 0;
  }
  return(stored == h) ? 1 : 0;
}

/**
* @brief  扫描扇区，返回有效数据结束位置
* @param  kv         KV实例指针
* @param  sec_index  扇区索引
* @return 有效数据结束偏移（用于计算活动扇区已用字节）
*/
static uint32_t fw_kv_sector_scan_used(fw_kv_t *kv, uint32_t sec_index)
{
  uint32_t offset = sizeof(wl_sector_hdr_t);
  uint32_t end = kv->sector_size;
  while(offset + sizeof(wl_node_hdr_t) <= end)
  {
    wl_node_hdr_t hdr;
    uint32_t rec_len;
    if(fw_kv_node_read_hdr(kv, sec_index, offset, &hdr) != FW_KV_OK)
    {
      break;
    }
    if(hdr.magic == FW_ERASED_WORD)
    {
      break;//遇到空尾，停止扫描
    }
    if(hdr.magic != FW_KV_NODE_MAGIC)
    {
      break;//魔数不匹配，数据损坏，截断
    }
    rec_len = sizeof(wl_node_hdr_t) + hdr.value_len + sizeof(uint32_t);
    if(offset + rec_len > end)
    {
      break;//记录超出扇区边界
    }
    if(!fw_kv_node_crc_ok(kv, sec_index, offset, &hdr))
    {
      break;//CRC校验失败，可能是半写入，截断
    }
    offset += rec_len;
  }
  return offset;
}

/**
* @brief  判断扇区有效数据之后的尾部是否仍是擦除态(可以继续追加)
* @param  kv         KV实例指针
* @param  sec_index  扇区索引
* @param  offset     fw_kv_sector_scan_used()返回的有效数据结束偏移
* @return 1=可以继续追加，0=尾部残留半写入数据
* @note   用于掉电保护：若上次追加被掉电打断，尾部会留下已编程的字，
*         此时不能再往该扇区追加(同一字在一次擦除周期内只能编程一次)
*/
static int fw_kv_sector_tail_appendable(fw_kv_t *kv, uint32_t sec_index, uint32_t offset)
{
  uint32_t word = FW_ERASED_WORD;

  if(offset + sizeof(uint32_t) > kv->sector_size)
  {
    return 0;//没有空间可追加
  }
  if(kv->ops.read(fw_kv_sector_addr(kv, sec_index) + (offset & ~(uint32_t)3U),
          (uint8_t *)&word, sizeof(word)) != 0)
  {
    return 0;
  }
  return (word == FW_ERASED_WORD) ? 1 : 0;
}

/**
* @brief  单次扫描全部分区，建立key->最新记录位置的映射
* @param  kv  KV实例指针
* @note   用于GC快速路径
*/
static void fw_kv_key_map_build(fw_kv_t *kv)
{
  fw_kv_key_map_reset(kv);
  for(uint32_t s = 0; s < kv->sector_count; s++)
  {
    wl_sector_hdr_t sh;
    uint32_t off;

    if(fw_kv_sector_read_hdr(kv, s, &sh) != FW_KV_OK) continue;
    if(!fw_kv_sector_is_valid(&sh)) continue;

    off = sizeof(wl_sector_hdr_t);
    while(off + sizeof(wl_node_hdr_t) <= kv->sector_size)
    {
      wl_node_hdr_t hdr;
      uint32_t rec_len;

      if(fw_kv_node_read_hdr(kv, s, off, &hdr) != FW_KV_OK) break;
      if(hdr.magic == FW_ERASED_WORD) break;
      if(hdr.magic != FW_KV_NODE_MAGIC) break;
      rec_len = sizeof(wl_node_hdr_t) + hdr.value_len + sizeof(uint32_t);
      if(off + rec_len > kv->sector_size) break;
      if(!fw_kv_node_crc_ok(kv, s, off, &hdr)) break;

      //记录该key的位置，相同key只保留最新的
      fw_kv_key_map_put(kv, hdr.key, sh.seq, s, off);
      off += rec_len;
    }
  }
}

/**
* @brief  判断扇区内是否存在某个key（任何状态都算存在）
* @param  kv         KV实例指针
* @param  sec_index  扇区索引
* @param  key        键值
* @return 1=存在，0=不存在
*/
static int fw_kv_sector_has_key(fw_kv_t *kv, uint32_t sec_index, uint32_t key)
{
  uint32_t offset = sizeof(wl_sector_hdr_t);
  uint32_t end = kv->sector_size;

  while(offset + sizeof(wl_node_hdr_t) <= end)
  {
    wl_node_hdr_t hdr;
    uint32_t rec_len;

    if(fw_kv_node_read_hdr(kv, sec_index, offset, &hdr) != FW_KV_OK) break;
    if(hdr.magic == FW_ERASED_WORD) break;
    if(hdr.magic != FW_KV_NODE_MAGIC) break;
    rec_len = sizeof(wl_node_hdr_t) + hdr.value_len + sizeof(uint32_t);
    if(offset + rec_len > end) break;
    if(!fw_kv_node_crc_ok(kv, sec_index, offset, &hdr)) break;
    if(hdr.key == key) return 1;
    offset += rec_len;
  }
  return 0;
}

/**
* @brief  判断扇区start偏移之后是否还存在该key
* @param  kv         KV实例指针
* @param  sec_index  扇区索引
* @param  start      起始偏移
* @param  key        键值
* @return 1=存在，0=不存在
* @note   用于判断某条记录是否为该key在本扇区中的最后一条
*/
static int fw_kv_sector_key_has_later(fw_kv_t *kv, uint32_t sec_index, uint32_t start, uint32_t key)
{
  uint32_t offset = start;
  uint32_t end = kv->sector_size;

  while(offset + sizeof(wl_node_hdr_t) <= end)
  {
    wl_node_hdr_t hdr;
    uint32_t rec_len;

    if(fw_kv_node_read_hdr(kv, sec_index, offset, &hdr) != FW_KV_OK) break;
    if(hdr.magic == FW_ERASED_WORD) break;
    if(hdr.magic != FW_KV_NODE_MAGIC) break;
    rec_len = sizeof(wl_node_hdr_t) + hdr.value_len + sizeof(uint32_t);
    if(offset + rec_len > end) break;
    if(!fw_kv_node_crc_ok(kv, sec_index, offset, &hdr)) break;
    if(hdr.key == key) return 1;
    offset += rec_len;
  }
  return 0;
}

/**
* @brief  在指定扇区内查找key的最新一条记录
* @param  kv          KV实例指针
* @param  sec_index   扇区索引
* @param  key         键值
* @param  out_offset  输出：记录在扇区内的偏移
* @param  out_hdr     输出：节点头
* @return 1=找到，0=未找到
*/
static int fw_kv_sector_find_newest(fw_kv_t *kv, uint32_t sec_index, uint32_t key,
              uint32_t *out_offset, wl_node_hdr_t *out_hdr)
{
  int found = 0;
  uint32_t offset = sizeof(wl_sector_hdr_t);
  uint32_t end = kv->sector_size;

  while(offset + sizeof(wl_node_hdr_t) <= end)
  {
    wl_node_hdr_t hdr;
    uint32_t rec_len;

    if(fw_kv_node_read_hdr(kv, sec_index, offset, &hdr) != FW_KV_OK) break;
    if(hdr.magic == FW_ERASED_WORD) break;
    if(hdr.magic != FW_KV_NODE_MAGIC) break;
    rec_len = sizeof(wl_node_hdr_t) + hdr.value_len + sizeof(uint32_t);
    if(offset + rec_len > end) break;
    if(!fw_kv_node_crc_ok(kv, sec_index, offset, &hdr)) break;
    if(hdr.key == key)
    {
      found = 1;
      *out_offset = offset;
      *out_hdr = hdr;
    }
    offset += rec_len;
  }
  return found;
}

/* ==================== 扇区管理与 GC ==================== */

/**
* @brief  判断比older_seq更新的扇区里是否还有该key
* @param  kv        KV实例指针
* @param  older_seq  参考扇区序号
* @param  key        键值
* @return 1=存在更新记录，0=不存在
*/
static int fw_kv_key_has_newer(fw_kv_t *kv, uint32_t older_seq, uint32_t key)
{
  for(uint32_t i = 0; i < kv->sector_count; i++)
  {
    wl_sector_hdr_t sh;
    if(fw_kv_sector_read_hdr(kv, i, &sh) != FW_KV_OK) continue;
    if(!fw_kv_sector_is_valid(&sh)) continue;
    if(sh.seq <= older_seq) continue;  //只检查序号更新的扇区
    if(fw_kv_sector_has_key(kv, i, key)) return 1;
  }
  return 0;
}

/**
* @brief  统计扇区中已用空间、垃圾字节与存活字节
* @param  kv          KV实例指针
* @param  sec_index   扇区索引
* @param  sec_seq     扇区序号
* @param  fast_path   1=使用key映射的快速路径，0=慢速路径(key数量超限时)
* @param  out_garbage 输出：可回收的垃圾字节数(可传NULL)
* @param  out_live    输出：仍是全局最新的有效数据字节数(可传NULL)
* @return 扇区有效数据结束偏移(含扇区头，遇坏记录/空尾即停止扫描)
* @note   扫描规则与读取路径完全一致，保证统计结果与实际可见数据一致
*/
static uint32_t fw_kv_sector_stats(fw_kv_t *kv, uint32_t sec_index, uint32_t sec_seq,
              int fast_path, uint32_t *out_garbage, uint32_t *out_live)
{
  uint32_t garbage = 0;
  uint32_t live = 0;
  uint32_t offset = sizeof(wl_sector_hdr_t);
  uint32_t end = kv->sector_size;

  while(offset + sizeof(wl_node_hdr_t) <= end)
  {
    wl_node_hdr_t hdr;
    uint32_t rec_len;
    int newest;

    if(fw_kv_node_read_hdr(kv, sec_index, offset, &hdr) != FW_KV_OK) break;
    if(hdr.magic == FW_ERASED_WORD) break;
    if(hdr.magic != FW_KV_NODE_MAGIC) break;
    rec_len = sizeof(wl_node_hdr_t) + hdr.value_len + sizeof(uint32_t);
    if(offset + rec_len > end) break;
    if(!fw_kv_node_crc_ok(kv, sec_index, offset, &hdr)) break;

    if(hdr.status == FW_KV_NODE_DELETED)
    {
      garbage += rec_len;  //DELETED节点直接算作垃圾
    }
    else if(hdr.status == FW_KV_NODE_VALID)
    {
      if(fast_path)
      {
        //快速路径：仅最新一条VALID记录为存活数据
        newest = fw_kv_key_is_newest(kv, hdr.key, sec_index, offset);
      }
      else
      {
        //慢速路径：检查是否有更新扇区里的同名key，或本扇区后面还有同名key
        newest = !fw_kv_key_has_newer(kv, sec_seq, hdr.key) &&
          !fw_kv_sector_key_has_later(kv, sec_index, offset + rec_len, hdr.key);
      }
      if(newest) live += rec_len;
      else       garbage += rec_len;
    }
    offset += rec_len;
  }

  if(out_garbage) *out_garbage = garbage;
  if(out_live)    *out_live = live;
  return offset;
}

/**
* @brief  判断扇区是否为空扇区(可作为GC搬迁目标)
* @param  kv         KV实例指针
* @param  sec_index  扇区索引
* @return 1=空扇区，0=非空
* @note   两种都算空扇区：
*         1) 扇区头无效(已擦除，从未写过或擦除后掉电)
*         2) 扇区头有效但一条记录都没有(GC结束时会给刚擦除的原扇区
*            立刻补写带擦除次数的头，用来保住擦除次数统计)
*/
static int fw_kv_sector_is_spare(fw_kv_t *kv, uint32_t sec_index)
{
  wl_sector_hdr_t sh;

  if(fw_kv_sector_read_hdr(kv, sec_index, &sh) != FW_KV_OK)
  {
    return 0;
  }
  if(!fw_kv_sector_is_valid(&sh))
  {
    return 1;
  }
  return (fw_kv_sector_scan_used(kv, sec_index) == sizeof(wl_sector_hdr_t)) ? 1 : 0;
}

/**
* @brief  查找一个空/无效扇区(掉电安全GC的搬迁目标)
* @param  kv  KV实例指针
* @return 扇区索引，-1表示没有空扇区
*/
static int fw_kv_gc_find_free_sector(fw_kv_t *kv)
{
  for(uint32_t i = 0; i < kv->sector_count; i++)
  {
    if((int)i == kv->active_sector) continue;
    if(fw_kv_sector_is_spare(kv, i)) return (int)i;
  }
  return -1;
}

/**
* @brief  统计分区内空/无效扇区数量(不含活动扇区)
* @param  kv  KV实例指针
* @return 空扇区数量
*/
static uint32_t fw_kv_free_sector_count(fw_kv_t *kv)
{
  uint32_t count = 0;

  for(uint32_t i = 0; i < kv->sector_count; i++)
  {
    if((int)i == kv->active_sector) continue;
    if(fw_kv_sector_is_spare(kv, i)) count++;
  }
  return count;
}

/**
* @brief  选择垃圾最多的有效非活动扇区作为回收对象
* @param  kv          KV实例指针
* @param  exclude     需要排除的扇区索引(搬迁目标)，<0表示不排除
* @param  fast_path   1=key映射快速路径
* @param  out_seq     输出：回收扇区序号(可传NULL)
* @param  out_garbage 输出：回收扇区垃圾字节数(可传NULL)
* @return 扇区索引，-1表示没有可回收扇区
* @note   先取垃圾最多者；垃圾相同时取序号最小(最早写入)的扇区，
*         保证回收顺序按写入先后轮转，不会总盯着低编号扇区擦写
*/
static int fw_kv_gc_pick_victim(fw_kv_t *kv, int exclude, int fast_path,
              uint32_t *out_seq, uint32_t *out_garbage)
{
  int victim = -1;
  uint32_t victim_seq = 0;
  uint32_t victim_garbage = 0;

  for(uint32_t i = 0; i < kv->sector_count; i++)
  {
    wl_sector_hdr_t sh;
    uint32_t garbage;

    if((int)i == kv->active_sector) continue;
    if((int)i == exclude) continue;
    if(fw_kv_sector_read_hdr(kv, i, &sh) != FW_KV_OK) continue;
    if(!fw_kv_sector_is_valid(&sh)) continue;

    (void)fw_kv_sector_stats(kv, i, sh.seq, fast_path, &garbage, NULL);
    if(victim < 0 || garbage > victim_garbage ||
       (garbage == victim_garbage && sh.seq < victim_seq))
    {
      victim = (int)i;
      victim_seq = sh.seq;
      victim_garbage = garbage;
    }
  }

  if(out_seq)     *out_seq = victim_seq;
  if(out_garbage) *out_garbage = victim_garbage;
  return victim;
}

/**
* @brief  选择存活数据最少的有效非活动扇区作为搬迁目标
* @param  kv        KV实例指针
* @param  exclude   需要排除的扇区索引，<0表示不排除
* @param  fast_path 1=key映射快速路径
* @return 扇区索引，-1表示没有可用扇区
* @note   仅在没有空扇区(老固件遗留状态)时使用：把两个扇区的存活数据合并到
*         一个扇区，腾出空扇区后即恢复始终预留一个备用扇区的策略
*/
static int fw_kv_gc_pick_donor(fw_kv_t *kv, int exclude, int fast_path)
{
  int donor = -1;
  uint32_t best_live = 0;
  uint32_t best_garbage = 0;
  uint32_t best_seq = 0;

  for(uint32_t i = 0; i < kv->sector_count; i++)
  {
    wl_sector_hdr_t sh;
    uint32_t garbage;
    uint32_t live;

    if((int)i == kv->active_sector) continue;
    if((int)i == exclude) continue;
    if(fw_kv_sector_read_hdr(kv, i, &sh) != FW_KV_OK) continue;
    if(!fw_kv_sector_is_valid(&sh)) continue;

    (void)fw_kv_sector_stats(kv, i, sh.seq, fast_path, &garbage, &live);
    if(donor < 0 || live < best_live ||
       (live == best_live && garbage > best_garbage) ||
       (live == best_live && garbage == best_garbage && sh.seq < best_seq))
    {
      donor = (int)i;
      best_live = live;
      best_garbage = garbage;
      best_seq = sh.seq;
    }
  }
  return donor;
}

/**
* @brief  把扇区中仍然有效的记录暂存到GC缓冲区
* @param  kv         KV实例指针
* @param  sec_index  扇区索引
* @param  sec_seq    扇区序号
* @param  fast_path  1=key映射快速路径
* @param  staged     输入输出：缓冲区中已暂存的字节数
* @return 0成功，非0失败
* @note   扫描规则与读取路径一致；暂存区不足时返回FW_KV_ERR_NOSPC
*/
static int fw_kv_gc_stage_sector(fw_kv_t *kv, uint32_t sec_index, uint32_t sec_seq,
              int fast_path, uint32_t *staged)
{
  uint32_t offset = sizeof(wl_sector_hdr_t);
  uint32_t end = kv->sector_size;

  while(offset + sizeof(wl_node_hdr_t) <= end)
  {
    wl_node_hdr_t hdr;
    uint32_t rec_len;
    int keep = 0;

    if(fw_kv_node_read_hdr(kv, sec_index, offset, &hdr) != FW_KV_OK) break;
    if(hdr.magic == FW_ERASED_WORD) break;
    if(hdr.magic != FW_KV_NODE_MAGIC) break;
    rec_len = sizeof(wl_node_hdr_t) + hdr.value_len + sizeof(uint32_t);
    if(offset + rec_len > end) break;
    if(!fw_kv_node_crc_ok(kv, sec_index, offset, &hdr)) break;

    if(hdr.status == FW_KV_NODE_VALID)
    {
      if(fast_path)
      {
        /* 快速路径：仅最新一条VALID记录为存活数据 */
        keep = fw_kv_key_is_newest(kv, hdr.key, sec_index, offset);
      }
      else
      {
        /* 慢速路径：检查是否有更新扇区里的同名key，或本扇区后面还有同名key */
        keep = !fw_kv_key_has_newer(kv, sec_seq, hdr.key) &&
          !fw_kv_sector_key_has_later(kv, sec_index, offset + rec_len, hdr.key);
      }
    }

    if(keep)
    {
      if(*staged + rec_len > sizeof(gc_buf))
      {
        return FW_KV_ERR_NOSPC;//暂存区装不下，调用方需回退到其它GC策略
      }
      if(kv->ops.read(fw_kv_sector_addr(kv, sec_index) + offset, gc_buf + *staged, rec_len) != 0)
      {
        return FW_KV_ERR_FLASH;
      }
      *staged += rec_len;
    }
    offset += rec_len;
  }
  return FW_KV_OK;
}

/**
* @brief  把源扇区中的存活记录逐条流式搬迁到目标扇区(不占大缓冲)
* @param  kv         KV实例指针
* @param  src        源扇区索引(victim)
* @param  src_seq    源扇区序号
* @param  fast_path  1=key映射快速路径
* @param  dest       目标扇区索引(扇区头已写好)
* @param  dest_used  输入输出：目标扇区已用字节数
* @return 0成功，非0失败
* @note   一次只经过gc_buf搬一条记录(记录长度一定小于等于扇区容量)，
*         因此不受暂存缓冲总大小限制，写一条即回读校验一条
*/
static int fw_kv_gc_stream_sector(fw_kv_t *kv, uint32_t src, uint32_t src_seq, int fast_path,
              uint32_t dest, uint32_t *dest_used)
{
  uint32_t offset = sizeof(wl_sector_hdr_t);
  uint32_t used = *dest_used;
  uint32_t end = kv->sector_size;

  while(offset + sizeof(wl_node_hdr_t) <= end)
  {
    wl_node_hdr_t hdr;
    uint32_t rec_len;
    int keep = 0;

    if(fw_kv_node_read_hdr(kv, src, offset, &hdr) != FW_KV_OK) break;
    if(hdr.magic == FW_ERASED_WORD) break;
    if(hdr.magic != FW_KV_NODE_MAGIC) break;
    rec_len = sizeof(wl_node_hdr_t) + hdr.value_len + sizeof(uint32_t);
    if(offset + rec_len > end) break;
    if(!fw_kv_node_crc_ok(kv, src, offset, &hdr)) break;

    if(hdr.status == FW_KV_NODE_VALID)
    {
      if(fast_path)
      {
        /* 快速路径：仅最新一条VALID记录为存活数据 */
        keep = fw_kv_key_is_newest(kv, hdr.key, src, offset);
      }
      else
      {
        /* 慢速路径：检查是否有更新扇区里的同名key，或本扇区后面还有同名key */
        keep = !fw_kv_key_has_newer(kv, src_seq, hdr.key) &&
          !fw_kv_sector_key_has_later(kv, src, offset + rec_len, hdr.key);
      }
    }

    if(keep)
    {
      if(rec_len > sizeof(gc_buf))
      {
        return FW_KV_ERR_NOSPC;//单条记录都放不下(记录不会超过扇区容量，正常不会发生)
      }
      if(used + rec_len > end)
      {
        return FW_KV_ERR_NOSPC;//目标扇区装不下了
      }
      if(kv->ops.read(fw_kv_sector_addr(kv, src) + offset, gc_buf, rec_len) != 0)
      {
        return FW_KV_ERR_FLASH;
      }
      if(kv->ops.write(fw_kv_sector_addr(kv, dest) + used, gc_buf, rec_len) != 0)
      {
        return FW_KV_ERR_FLASH;
      }
      //刚写进去的这条回读校验，通过后才继续下一条
      if(!fw_kv_node_crc_ok(kv, dest, used, &hdr))
      {
        return FW_KV_ERR_FLASH;
      }
      used += rec_len;
    }
    offset += rec_len;
  }

  *dest_used = used;
  return FW_KV_OK;
}

/**
* @brief  回读校验搬迁目标扇区中写入的存活记录
* @param  kv         KV实例指针
* @param  sec_index  目标扇区索引
* @param  staged     已写入的字节数
* @return 0成功，非0失败
* @note   逐条CRC校验，确认搬迁数据完整后才允许擦除原扇区
*/
static int fw_kv_gc_verify_records(fw_kv_t *kv, uint32_t sec_index, uint32_t staged)
{
  uint32_t offset = sizeof(wl_sector_hdr_t);
  uint32_t end = sizeof(wl_sector_hdr_t) + staged;

  while(offset < end)
  {
    wl_node_hdr_t hdr;
    uint32_t rec_len;

    if(fw_kv_node_read_hdr(kv, sec_index, offset, &hdr) != FW_KV_OK) return FW_KV_ERR_FLASH;
    if(hdr.magic != FW_KV_NODE_MAGIC) return FW_KV_ERR_FLASH;
    rec_len = sizeof(wl_node_hdr_t) + hdr.value_len + sizeof(uint32_t);
    if(offset + rec_len > end) return FW_KV_ERR_FLASH;
    if(!fw_kv_node_crc_ok(kv, sec_index, offset, &hdr)) return FW_KV_ERR_FLASH;
    offset += rec_len;
  }
  return (offset == end) ? FW_KV_OK : FW_KV_ERR_FLASH;
}

/**
* @brief  GC：回收一个扇区，腾出可继续追加的空间
* @param  kv  KV实例指针
* @return 0成功，非0失败
* @note   掉电安全策略：先把待回收扇区(victim)中仍有效的记录写入另一个扇区
*         (dest)并逐条回读校验，确认无误后才擦除victim。任一步骤掉电，
*         victim都保持完整，不会丢数据。
*         为保证始终存在备用扇区，扇区轮转时预留至少一个空扇区
*         (见fw_kv_sector_activate_next)；只有当分区内一个空扇区都没有时(例如从
*         旧固件升级上来)，才退化为把两个扇区的存活数据压缩进一个扇区
*         腾出空扇区。
*/
static int fw_kv_gc_collect(fw_kv_t *kv)
{
  int dest;
  int victim;
  uint32_t victim_seq = 0;
  uint32_t victim_garbage = 0;
  uint32_t new_seq;
  uint32_t dest_used = sizeof(wl_sector_hdr_t);
  int fast_path;
  int donor_path = 0;   //1=没有空扇区，搬迁目标本身是有效扇区(旧分区状态的回退路径)

  /* 映射由init/set/del维护(搬迁结束后会重建)，这里只判断它是否可用；
   * 键数超限导致映射不可用时回退到慢速路径 */
  fast_path = kv->key_map_ok;

  /* 1. 选搬迁目标：优先空扇区；没有空扇区时选存活数据最少的有效扇区
   *    (此时目标扇区自己的存活数据也要一起搬走，等价于压缩两个扇区) */
  dest = fw_kv_gc_find_free_sector(kv);
  if(dest < 0)
  {
    dest = fw_kv_gc_pick_donor(kv, -1, fast_path);
    if(dest < 0)
    {
      return FW_KV_ERR_NOSPC;//没有任何扇区可搬迁，空间不足
    }
    donor_path = 1;
  }

  /* 2. 选回收对象：垃圾最多的有效非活动扇区(不能是搬迁目标) */
  victim = fw_kv_gc_pick_victim(kv, dest, fast_path, &victim_seq, &victim_garbage);
  if(victim < 0 || victim_garbage == 0)
  {
    return FW_KV_ERR_NOSPC;//没有可回收的垃圾
  }

  new_seq = kv->next_seq;

  if(donor_path)
  {
    /* ---- 回退路径：目标扇区自己还存着有效数据，必须先把两边数据都搬到RAM ---- */
    uint32_t staged = 0;
    wl_sector_hdr_t sh;

    if(fw_kv_sector_read_hdr(kv, (uint32_t)dest, &sh) == FW_KV_OK && fw_kv_sector_is_valid(&sh))
    {
      if(fw_kv_gc_stage_sector(kv, (uint32_t)dest, sh.seq, fast_path, &staged) != FW_KV_OK)
      {
        return FW_KV_ERR_NOSPC;
      }
    }
    if(fw_kv_gc_stage_sector(kv, (uint32_t)victim, victim_seq, fast_path, &staged) != FW_KV_OK)
    {
      return FW_KV_ERR_NOSPC;
    }
    if(sizeof(wl_sector_hdr_t) + staged > kv->sector_size)
    {
      return FW_KV_ERR_NOSPC;//存活数据装不进一个扇区
    }
    if(fw_kv_sector_erase(kv, (uint32_t)dest) != FW_KV_OK)
    {
      return FW_KV_ERR_FLASH;
    }
    if(fw_kv_sector_write_hdr(kv, (uint32_t)dest, new_seq, FW_KV_SECTOR_ACTIVE) != FW_KV_OK)
    {
      return FW_KV_ERR_FLASH;
    }
    if(staged > 0)
    {
      if(kv->ops.write(fw_kv_sector_addr(kv, (uint32_t)dest) + sizeof(wl_sector_hdr_t), gc_buf, staged) != 0)
      {
        return FW_KV_ERR_FLASH;
      }
      if(fw_kv_gc_verify_records(kv, (uint32_t)dest, staged) != FW_KV_OK)
      {
        return FW_KV_ERR_FLASH;
      }
    }
    dest_used = sizeof(wl_sector_hdr_t) + staged;
  }
  else
  {
    /* ---- 安全路径：目标是空扇区(通常是上次GC擦干净的备用扇区)，
     *      可以直接逐条流式搬迁，不受暂存缓冲大小限制 ---- */
    if(!fw_kv_sector_is_erased(kv, (uint32_t)dest))
    {
      if(fw_kv_sector_erase(kv, (uint32_t)dest) != FW_KV_OK)
      {
        return FW_KV_ERR_FLASH;
      }
    }
    if(fw_kv_sector_write_hdr(kv, (uint32_t)dest, new_seq, FW_KV_SECTOR_ACTIVE) != FW_KV_OK)
    {
      return FW_KV_ERR_FLASH;
    }
    if(fw_kv_gc_stream_sector(kv, (uint32_t)victim, victim_seq, fast_path, (uint32_t)dest, &dest_used) != FW_KV_OK)
    {
      return FW_KV_ERR_NOSPC;
    }
  }

  /* 3. 确认目标扇区数据完整后才擦除原扇区(擦除失败也不丢数据：目标已有完整副本)。
   *    擦除后不补写扇区头，使其保持整体擦除的空扇区状态作为备用扇区：
   *    下次GC直接以其为搬迁目标，无需再擦除(省一半擦除次数) */
  kv->next_seq = new_seq + 1;
  if(victim != dest)
  {
    (void)fw_kv_sector_erase(kv, (uint32_t)victim);
  }

  kv->active_sector = dest;
  kv->used = dest_used;
  FW_KV_LOG(FW_KV_EVT_GC, kv, victim, dest, dest_used);
  //搬迁改变了存活记录的位置，重建常驻映射
  fw_kv_key_map_build(kv);
  return FW_KV_OK;
}

/**
* @brief  激活下一个扇区
* @param  kv  KV实例指针
* @return 0成功，非0失败
* @note   掉电安全GC必须有空扇区做搬迁目标，所以这里始终预留至少一个空扇区：
*         空闲扇区只剩1个时先做GC(GC用完一个空扇区做目标、回收victim后又
*         还回一个空扇区，数量不变)，GC成功就直接以搬迁目标作为活动扇区。
*/
static int fw_kv_sector_activate_next(fw_kv_t *kv)
{
  int start;
  uint32_t free_count;

  /* 不把旧活动扇区原地位翻转成FULL：0x00擦除器件单字只能写一次，
   * 非零word无法再写新值，且ACTIVE/FULL对正确性并无区别，活动扇区由seq确定 */
  start = (kv->active_sector < 0) ? 0 : (kv->active_sector + 1);

  /* 空闲扇区不足2个：先GC腾空间，避免把最后一个空扇区也用掉 */
  free_count = fw_kv_free_sector_count(kv);
  if(free_count <= 1U)
  {
    if(fw_kv_gc_collect(kv) == FW_KV_OK)
    {
      return FW_KV_OK;//GC成功后active已指向搬迁目标扇区
    }
    if(free_count == 0U)
    {
      return FW_KV_ERR_NOSPC;//一个空扇区都没有，无法做掉电安全GC
    }
    /* 只剩1个空扇区且GC没成功(例如没有垃圾可回收)：退化为直接占用该空扇区 */
  }

  /* 先找空扇区(无效扇区头，或有效但无记录的备用扇区) */
  for(uint32_t n = 0; n < kv->sector_count; n++)
  {
    uint32_t idx = ((uint32_t)start + n) % kv->sector_count;
    if((int)idx == kv->active_sector) continue;
    if(fw_kv_sector_is_spare(kv, idx))
    {
      //找到一个空扇区：已是整体擦除态则直接写头以省一次擦除，否则先擦除
      if(!fw_kv_sector_is_erased(kv, idx))
      {
        if(fw_kv_sector_erase(kv, idx) != FW_KV_OK) return FW_KV_ERR_FLASH;
      }
      if(fw_kv_sector_write_hdr(kv, idx, kv->next_seq++, FW_KV_SECTOR_ACTIVE) != FW_KV_OK)
      {
        return FW_KV_ERR_FLASH;
      }
      kv->active_sector = (int)idx;
      kv->used = sizeof(wl_sector_hdr_t);
      FW_KV_LOG(FW_KV_EVT_ROTATE, kv, (int)idx, 0, kv->next_seq - 1u);
      return FW_KV_OK;
    }
  }

  /* 没有空扇区，执行GC；GC成功后active已指向新扇区 */
  return fw_kv_gc_collect(kv);
}

/* ==================== 节点追加 ==================== */

/**
* @brief  追加一个KV节点到当前活动扇区
* @param  kv      KV实例指针
* @param  key     键值
* @param  value   值数据指针
* @param  len     值长度
* @param  status  节点状态（VALID/DELETED）
* @return 0成功，非0失败
*/
static int fw_kv_node_append(fw_kv_t *kv, uint32_t key, const uint8_t *value,
          uint32_t len, uint32_t status)
{
  uint32_t rec_len = sizeof(wl_node_hdr_t) + len + sizeof(uint32_t);
  uint32_t addr;
  wl_node_hdr_t hdr;
  uint32_t h;
  uint32_t crc;

  /* 追加前须满足两个条件：存在活动扇区，且本记录不跨扇区边界。
   * 跨扇区写会覆盖尚未擦除的下一扇区头，导致数据损坏 */
  FW_ASSERT(kv->active_sector >= 0);
  FW_ASSERT(kv->used + rec_len <= kv->sector_size);

  addr = fw_kv_sector_addr(kv, (uint32_t)kv->active_sector) + kv->used;

  hdr.magic = FW_KV_NODE_MAGIC;
  hdr.key = key;
  hdr.value_len = len;
  hdr.status = status;

  /* CRC计算范围：key + value_len + status + value */
  h = 0x811C9DC5u;
  h = fw_kv_fnv1a_update(h, (const uint8_t *)&hdr.key, sizeof(hdr.key));
  h = fw_kv_fnv1a_update(h, (const uint8_t *)&hdr.value_len, sizeof(hdr.value_len));
  h = fw_kv_fnv1a_update(h, (const uint8_t *)&hdr.status, sizeof(hdr.status));
  h = fw_kv_fnv1a_update(h, value, len);
  crc = h;

  //写入节点头
  if(kv->ops.write(addr, (const uint8_t *)&hdr, sizeof(hdr)) != 0)
  {
    return FW_KV_ERR_FLASH;
  }
  //写入value
  if(len > 0 && kv->ops.write(addr + sizeof(hdr), value, len) != 0)
  {
    return FW_KV_ERR_FLASH;
  }
  //写入CRC
  if(kv->ops.write(addr + sizeof(hdr) + len, (const uint8_t *)&crc, sizeof(crc)) != 0)
  {
    return FW_KV_ERR_FLASH;
  }

  kv->used += rec_len;
  return FW_KV_OK;
}

/* ==================== 分区表（fw_cfg.h单宏登记 + 按名查找） ==================== */
/**
* @brief  按名称查找KV分区
* @param  name  分区名
* @return 分区描述指针，未找到返回NULL
*/
const fw_kv_part_t *fw_kv_part_find(const char *name)
{
  if(name == NULL)
  {
    return NULL;
  }
  for(uint32_t i = 0; i < fw_kv_part_count; i++)
  {
    if(fw_kv_part_table[i].name != NULL && strcmp(fw_kv_part_table[i].name, name) == 0)
    {
      return &fw_kv_part_table[i];
    }
  }
  return NULL;
}

/**
* @brief  按分区名初始化KV实例
* @param  kv    KV实例指针
* @param  name  分区名
* @return 0成功，非0失败
*/
int fw_kv_init_by_name(fw_kv_t *kv, const char *name)
{
  const fw_kv_part_t *part = fw_kv_part_find(name);
  if(part == NULL)
  {
    return FW_KV_ERR_PARAM;
  }
  return fw_kv_init(kv, part);
}

/* ==================== 对外接口 ==================== */

/**
* @brief  初始化KV实例
* @param  kv    KV实例指针
* @param  part  分区描述指针
* @return 0成功，非0失败
*/
int fw_kv_init(fw_kv_t *kv, const fw_kv_part_t *part)
{
  uint32_t max_seq = 0;
  int active = -1;
  //参数合法性检查
  if(!kv || !part || part->page_size == 0U)
  {
    return FW_KV_ERR_PARAM;
  }
  //分区表必须给出ops，即用FW_KV_OPS在分区表里直接写函数名，或给一个&xxx_ops实例
  if(part->ops == NULL ||
     !part->ops->read || !part->ops->erase || !part->ops->write)
  {
    return FW_KV_ERR_PARAM;
  }
  /* 分区几何约束(见README)：大小为扇区整数倍、扇区为2的幂且能被页整除。
   * 分区表属外部输入，配置错误返回错误码，不使用断言 */
  if(part->sector_size == 0U || (part->size % part->sector_size) != 0U)
  {
    return FW_KV_ERR_PARAM;
  }
  if((part->sector_size & (part->sector_size - 1u)) != 0u)
  {
    return FW_KV_ERR_PARAM;//扇区大小必须是2的幂
  }
  if((part->sector_size % part->page_size) != 0u)
  {
    return FW_KV_ERR_PARAM;//扇区大小必须是页大小的整数倍
  }
  //初始化实例字段(同时保存分区名，供日志回调使用)
  kv->ops = *part->ops;
  kv->name = (part->name != NULL) ? part->name : "?";
  kv->base = part->base;
  kv->size = part->size;
  kv->sector_size = part->sector_size;
  kv->page_size = part->page_size;
  kv->sector_count = part->size / part->sector_size;
  kv->active_sector = -1;
  kv->used = sizeof(wl_sector_hdr_t);
  kv->next_seq = 1;
  kv->key_count = 0;
  kv->key_map_ok = 1;
  if(kv->sector_count < 2U || kv->sector_count > FW_KV_MAX_SECTORS)
  {
    return FW_KV_ERR_PARAM;//至少需要2个扇区才能实现磨损均衡
  }
  /* 以下为配置检查通过后的内部不变量，后续扇区号换算均依赖它们 */
  FW_ASSERT(kv->sector_count >= 2U);
  FW_ASSERT((kv->sector_size % kv->page_size) == 0U);
  //扫描所有扇区，找到seq最大的有效扇区作为活动扇区
  for(uint32_t i = 0; i < kv->sector_count; i++)
  {
    wl_sector_hdr_t sh;
		if(fw_kv_sector_read_hdr(kv, i, &sh) != FW_KV_OK)
		{
			continue;
		}
    if(fw_kv_sector_is_valid(&sh) && sh.seq > max_seq)
    {
      max_seq = sh.seq;
      active = (int)i;
    }
  }
  if(active >= 0)
  {
    kv->active_sector = active;
    kv->next_seq = max_seq + 1;
    kv->used = fw_kv_sector_scan_used(kv, (uint32_t)active);
    /* 掉电保护：若上次追加被掉电打断，扇区尾部会残留已编程的字，
     * 该扇区不能再继续追加(同一字在一个擦除周期内只能编程一次)，
     * 因此视为已满，交给后续的扇区轮转/GC回收 */
    if(!fw_kv_sector_tail_appendable(kv, (uint32_t)active, kv->used))
    {
      kv->used = kv->sector_size;
    }
  }
  //建立常驻key映射：get与统计均依赖它，无需每次扫描全分区
  fw_kv_key_map_build(kv);
  return FW_KV_OK;
}

/**
* @brief  在常驻映射里定位key，并读回节点头校验
* @param  kv       KV实例指针
* @param  key      键值
* @param  out_sec  输出：扇区索引
* @param  out_off  输出：记录在扇区内的偏移
* @param  out_hdr  输出：节点头
* @return 1=命中且校验通过，0=未命中/映射不可用/校验不过
* @note   命中后仍读一次节点头：万一Flash被外部改动过，位置处的key对不上
*         就判为不可信，由调用方回退到全扫描兜底
*/
static int fw_kv_key_map_locate(fw_kv_t *kv, uint32_t key, uint32_t *out_sec,
              uint32_t *out_off, wl_node_hdr_t *out_hdr)
{
  if(!kv->key_map_ok)
  {
    return 0;
  }
  for(uint32_t i = 0; i < kv->key_count; i++)
  {
    if(kv->key_map[i].key != key)
    {
      continue;
    }
    if(fw_kv_node_read_hdr(kv, kv->key_map[i].sector, kv->key_map[i].offset, out_hdr) != FW_KV_OK)
    {
      return 0;
    }
    if(out_hdr->magic != FW_KV_NODE_MAGIC || out_hdr->key != key)
    {
      return 0;
    }
    *out_sec = kv->key_map[i].sector;
    *out_off = kv->key_map[i].offset;
    return 1;
  }
  return 0;
}

/**
* @brief  全分区扫描查找key的最新记录(映射不可用时的兜底路径)
* @param  kv       KV实例指针
* @param  key      键值
* @param  out_sec  输出：扇区索引
* @param  out_off  输出：记录在扇区内的偏移
* @param  out_hdr  输出：节点头
* @return 1=找到，0=未找到
*/
static int fw_kv_get_scan(fw_kv_t *kv, uint32_t key, uint32_t *out_sec,
             uint32_t *out_off, wl_node_hdr_t *out_hdr)
{
  uint32_t best_seq = 0;
  int found = 0;

  //遍历所有扇区，找seq最大的有效记录
  for(uint32_t i = 0; i < kv->sector_count; i++)
  {
    wl_sector_hdr_t sh;
    uint32_t off;
    wl_node_hdr_t nh;

    if(fw_kv_sector_read_hdr(kv, i, &sh) != FW_KV_OK) continue;
    if(!fw_kv_sector_is_valid(&sh)) continue;
    if(fw_kv_sector_find_newest(kv, i, key, &off, &nh))
    {
      if(sh.seq > best_seq)  //选择序号最大的扇区中的记录
      {
        best_seq = sh.seq;
        *out_sec = i;
        *out_off = off;
        *out_hdr = nh;
        found = 1;
      }
    }
  }
  return found;
}

/**
* @brief  获取key对应的值（内部实现，不加锁）
* @param  kv     KV实例指针
* @param  key    键值
* @param  value  输出缓冲区
* @param  len    输入：缓冲区大小；输出：实际值长度
* @return 0成功，非0失败
* @note   先查常驻映射直接定位；映射不可用或校验不过时回退全分区扫描
*/
static int fw_kv_get_impl(fw_kv_t *kv, uint32_t key, uint8_t *value, uint32_t *len)
{
  uint32_t sec = 0;
  uint32_t offset = 0;
  wl_node_hdr_t hdr;

  if(!kv || !len) return FW_KV_ERR_PARAM;
  if(key == 0U) return FW_KV_ERR_PARAM;
  if(value == NULL && *len != 0U) return FW_KV_ERR_PARAM;

  if(!fw_kv_key_map_locate(kv, key, &sec, &offset, &hdr))
  {
    if(!fw_kv_get_scan(kv, key, &sec, &offset, &hdr))
    {
      *len = 0;
      return FW_KV_ERR_NOT_FOUND;
    }
  }

  if(hdr.status == FW_KV_NODE_DELETED)
  {
    *len = 0;
    return FW_KV_ERR_NOT_FOUND;
  }

  //检查缓冲区是否足够
  if(*len < hdr.value_len)
  {
    *len = hdr.value_len;
    return FW_KV_ERR_BUF_SMALL;
  }

  //读取value
  if(hdr.value_len > 0U)
  {
    uint32_t addr = fw_kv_sector_addr(kv, sec) + offset + sizeof(wl_node_hdr_t);
    if(kv->ops.read(addr, value, hdr.value_len) != 0)
    {
      FW_KV_LOG(FW_KV_EVT_ERROR, kv, FW_KV_ERR_FLASH, 0, 0);
      return FW_KV_ERR_FLASH;
    }
  }

  *len = hdr.value_len;
  return FW_KV_OK;
}

/**
* @brief  确保活动扇区有足够空间追加一条记录
* @param  kv       KV实例指针
* @param  rec_len  记录长度
* @return 0成功，非0失败
*/
static int fw_kv_ensure_space(fw_kv_t *kv, uint32_t rec_len)
{
  uint32_t gc_watermark;

  //如果没有活动扇区，激活一个
  if(kv->active_sector < 0)
  {
    if(fw_kv_sector_activate_next(kv) != FW_KV_OK) return FW_KV_ERR_NOSPC;
  }

  gc_watermark = (uint32_t)FW_KV_GC_THRESHOLD * kv->page_size;

  /* 放不下：必须切换/GC */
  if(kv->used + rec_len > kv->sector_size)
  {
    if(fw_kv_sector_activate_next(kv) != FW_KV_OK) return FW_KV_ERR_NOSPC;
    if(kv->used + rec_len > kv->sector_size) return FW_KV_ERR_NOSPC;
    return FW_KV_OK;
  }

  /* 放得下但剩余空间低于GC阈值：主动提前GC（尽力而为，失败也继续追加） */
  if((kv->sector_size - kv->used) <= gc_watermark)
  {
    (void)fw_kv_sector_activate_next(kv);  //提前触发GC，忽略失败
  }

  return FW_KV_OK;
}

/**
* @brief  设置key的值（内部实现，不加锁）
* @param  kv     KV实例指针
* @param  key    键值
* @param  value  值数据指针
* @param  len    值长度
* @return 0成功，非0失败
* @note   追加一条新记录，并同步更新常驻映射
*/
static int fw_kv_set_impl(fw_kv_t *kv, uint32_t key, const uint8_t *value, uint32_t len)
{
  uint32_t rec_len;
  uint32_t offset;
  uint32_t seq;
  uint32_t sector;

  if(!kv || key == 0U || (value == NULL && len > 0U))
  {
    return FW_KV_ERR_PARAM;
  }

  rec_len = sizeof(wl_node_hdr_t) + len + sizeof(uint32_t);
  if(rec_len > kv->sector_size - sizeof(wl_sector_hdr_t))
  {
    return FW_KV_ERR_NOSPC;  //记录太大，单个扇区放不下
  }

  if(fw_kv_ensure_space(kv, rec_len) != FW_KV_OK)
  {
    FW_KV_LOG(FW_KV_EVT_ERROR, kv, FW_KV_ERR_NOSPC, 0, 0);
    return FW_KV_ERR_NOSPC;
  }

  //空间确保后(可能已轮转或GC)再取本条记录的位置：就是当前已用位置
  FW_ASSERT(kv->active_sector >= 0);   //ensure_space成功时必定留下一个活动扇区
  FW_ASSERT(kv->next_seq >= 1u);       //序号从1起单调递增，减1不会下溢
  offset = kv->used;
  seq = kv->next_seq - 1u;
  sector = (uint32_t)kv->active_sector;

  if(fw_kv_node_append(kv, key, value, len, FW_KV_NODE_VALID) != FW_KV_OK)
  {
    FW_KV_LOG(FW_KV_EVT_ERROR, kv, FW_KV_ERR_FLASH, 0, 0);
    return FW_KV_ERR_FLASH;
  }

  //更新常驻映射：该key的最新记录在新位置
  fw_kv_key_map_put(kv, key, seq, sector, offset);
  return FW_KV_OK;
}

/**
* @brief  删除key（内部实现，不加锁）
* @param  kv   KV实例指针
* @param  key  键值
* @return 0成功，非0失败
* @note   追加一条DELETED墓碑记录实现删除，并同步更新常驻映射
*/
static int fw_kv_del_impl(fw_kv_t *kv, uint32_t key)
{
  uint32_t rec_len;
  uint32_t offset;
  uint32_t seq;
  uint32_t sector;

  if(!kv || key == 0U)
  {
    return FW_KV_ERR_PARAM;
  }

  /* 删除通过追加一个DELETED墓碑记录实现 */
  rec_len = sizeof(wl_node_hdr_t) + sizeof(uint32_t);
  if(rec_len > kv->sector_size - sizeof(wl_sector_hdr_t))
  {
    return FW_KV_ERR_NOSPC;
  }

  if(fw_kv_ensure_space(kv, rec_len) != FW_KV_OK)
  {
    return FW_KV_ERR_NOSPC;
  }

  //空间确保后(可能已轮转或GC)再取本条墓碑的位置
  FW_ASSERT(kv->active_sector >= 0);   //ensure_space成功时必定留下一个活动扇区
  FW_ASSERT(kv->next_seq >= 1u);       //序号从1起单调递增，减1不会下溢
  offset = kv->used;
  seq = kv->next_seq - 1u;
  sector = (uint32_t)kv->active_sector;

  if(fw_kv_node_append(kv, key, NULL, 0, FW_KV_NODE_DELETED) != FW_KV_OK)
  {
    return FW_KV_ERR_FLASH;
  }

  //映射指向墓碑记录，get时据此返回不存在
  fw_kv_key_map_put(kv, key, seq, sector, offset);
  return FW_KV_OK;
}

//以下是对外接口，带并发保护
int fw_kv_get(fw_kv_t *kv, uint32_t key, uint8_t *value, uint32_t *len)
{
  int ret;

  FW_KV_LOCK();
  ret = fw_kv_get_impl(kv, key, value, len);
  FW_KV_UNLOCK();
  return ret;
}

int fw_kv_set(fw_kv_t *kv, uint32_t key, const uint8_t *value, uint32_t len)
{
  int ret;

  FW_KV_LOCK();
  ret = fw_kv_set_impl(kv, key, value, len);
  FW_KV_UNLOCK();
  return ret;
}

int fw_kv_del(fw_kv_t *kv, uint32_t key)
{
  int ret;

  FW_KV_LOCK();
  ret = fw_kv_del_impl(kv, key);
  FW_KV_UNLOCK();
  return ret;
}

/**
* @brief  获取KV分区统计信息
* @param  kv     KV实例指针
* @param  stats  统计信息输出指针
* @return 0成功，非0失败
*/
int fw_kv_get_stats(fw_kv_t *kv, fw_kv_stats_t *stats)
{
  if(!kv || !stats)
  {
    return FW_KV_ERR_PARAM;
  }

  FW_KV_LOCK();

  memset(stats, 0, sizeof(*stats));
  stats->sector_size = kv->sector_size;
  stats->sector_count = kv->sector_count;
  stats->active_sector = (kv->active_sector >= 0) ? (uint32_t)kv->active_sector : 0xFFFFFFFFu;
  stats->used_bytes = kv->used;

  /* 建立key映射，用于区分存活记录与垃圾记录 */
  fw_kv_key_map_build(kv);

  for(uint32_t s = 0; s < kv->sector_count; s++)
  {
    wl_sector_hdr_t sh;
    uint32_t off;
    uint32_t sector_garbage = 0;
    uint32_t sector_live = 0;

    if(fw_kv_sector_read_hdr(kv, s, &sh) != FW_KV_OK)
    {
      stats->empty_sectors++;
      continue;
    }
    if(!fw_kv_sector_is_valid(&sh))
    {
      stats->empty_sectors++;
      continue;
    }

    off = sizeof(wl_sector_hdr_t);
    while(off + sizeof(wl_node_hdr_t) <= kv->sector_size)
    {
      wl_node_hdr_t hdr;
      uint32_t rec_len;

      if(fw_kv_node_read_hdr(kv, s, off, &hdr) != FW_KV_OK) break;
      if(hdr.magic == FW_ERASED_WORD) break;
      if(hdr.magic != FW_KV_NODE_MAGIC) break;
      rec_len = sizeof(wl_node_hdr_t) + hdr.value_len + sizeof(uint32_t);
      if(off + rec_len > kv->sector_size) break;
      if(!fw_kv_node_crc_ok(kv, s, off, &hdr)) break;

      if(hdr.status == FW_KV_NODE_DELETED)
      {
        sector_garbage += rec_len;
      }
      else if(hdr.status == FW_KV_NODE_VALID)
      {
        if(fw_kv_key_is_newest(kv, hdr.key, s, off))
        {
          sector_live += rec_len;  //最新记录算作有效数据
        }
        else
        {
          sector_garbage += rec_len;  //旧版本记录算作垃圾
        }
      }
      off += rec_len;
    }

    if(off == sizeof(wl_sector_hdr_t))
    {
      stats->empty_sectors++;//有效但没有记录的备用扇区
    }
    //非活动有效扇区视为已满扇区：0x00擦除器件无法原位翻转status，活动扇区由seq确定
    else if((int)s != kv->active_sector)
    {
      stats->full_sectors++;
    }

    if(sector_garbage > 0)
    {
      stats->dirty_sectors++;
    }
    stats->garbage_bytes += sector_garbage;
    stats->live_bytes += sector_live;
  }

  FW_KV_UNLOCK();
  return FW_KV_OK;
}

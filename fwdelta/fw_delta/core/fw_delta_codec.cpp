#include "fw_delta_codec.h"                         //差分编码接口声明

#include <algorithm>                                //std::min
#include <array>                                    //std::array
#include <cstdio>                                   //std::snprintf
#include <cstring>                                  //std::memcmp
#include <random>                                   //std::mt19937
#include <unordered_map>                            //std::unordered_map

namespace fw                                        //固件镜像与差分处理命名空间
{

namespace
{

//差分记录类型，与设备端 fw_delta.h 一致
constexpr uint8_t kOpCopy = 1u;                     //COPY记录：从旧镜像按偏移拷贝
constexpr uint8_t kOpLiteral = 2u;                  //LITERAL记录：直接携带新数据
constexpr uint8_t kOpDiff = 3u;                     //DIFF记录：以旧镜像为基只发差异点
//DIFF块长上限，与设备端FW_DELTA_BUF_SIZE一致
constexpr uint32_t kDiffMax = 128u;                 //DIFF块最大长度，受设备端缓冲区限制

//一条差分记录
struct Op
{
  uint8_t type = 0;                                 //记录类型，取kOpCopy、kOpLiteral或kOpDiff
  uint32_t src = 0;                                 //COPY与DIFF的源偏移，指向旧镜像代码区
  uint32_t len = 0;                                 //记录覆盖的字节数
  std::vector<uint8_t> data;   //LITERAL的数据
  std::vector<uint16_t> pos;   //DIFF的差异位置
  std::vector<uint8_t> val;    //DIFF的差异值
};

//切块结果：每块的[起始,结束)
using Chunks = std::vector<std::pair<uint32_t, uint32_t>>;

/**
 * @brief   取gear哈希表(固定种子，保证结果可复现)
 */
const std::array<uint32_t, 256> &gear_table()
{
  static const std::array<uint32_t, 256> table = []  //首次调用时生成并缓存
  {
    std::array<uint32_t, 256> t{};                   //全部元素初始化为0
    std::mt19937 rng(20261020u);                     //固定种子，保证每次运行表内容一致
    for(auto &v : t)                                 //逐项填充
    {
      v = rng();                                     //取一个32位随机数作为该字节的gear权重
    }
    return t;                                        //返回构造完成的表
  }();                                               //立即执行该lambda完成初始化
  return table;                                      //返回静态表引用
}

/**
 * @brief   固定大小分块
 */
Chunks chunk_fixed(const std::vector<uint8_t> &data, uint32_t size)
{
  Chunks chunks;  //本次切块结果
  if(size == 0u)                                     //块长为0会让步进无效并死循环
  {
    return chunks;                                   //返回空切块，调用方按失败处理
  }
  for(uint32_t s = 0; s < data.size(); s += size)    //按固定步长从头到尾切块
  {
    chunks.emplace_back(s, static_cast<uint32_t>(std::min<size_t>(data.size(), s + size)));  //末块按实际剩余长度截断
  }
  return chunks;                                     //返回切块列表
}

/**
 * @brief   CDC内容定义分块(gear滚动哈希)
 * @note    切点由局部内容决定，插入/删除后能重新对齐。
 */
Chunks chunk_cdc(const std::vector<uint8_t> &data, uint32_t min_sz, uint32_t avg_bits, uint32_t max_sz)
{
  const auto &gear = gear_table();                   //取固定种子的gear权重表
  const uint32_t mask = (avg_bits >= 32u) ? 0xFFFFFFFFu : ((1u << avg_bits) - 1u);  //低avg_bits位掩码，位宽超限时取全1
  Chunks chunks;  //本次切块结果
  uint32_t start = 0;                                //当前块起始偏移
  uint32_t h = 0;                                    //滚动哈希当前值

  for(uint32_t i = 0; i < data.size(); i++)          //逐字节推进切点判定
  {
    h = (h << 1) + gear[data[i]];                    //gear滚动哈希：左移一位后叠加当前字节权重
    uint32_t len = i - start + 1;                    //当前块已累计长度
    if(len >= max_sz || (len >= min_sz && (h & mask) == 0u))  //达到块长上限或满足最小块且哈希选中
    {
      chunks.emplace_back(start, i + 1);             //在当前位置切出一块
      start = i + 1;                                 //下一块从下一字节开始
      h = 0;                                         //重置哈希，让下一块的切点只由块内内容决定
    }
  }
  if(start < data.size())                            //末尾不足一块的剩余数据
  {
    chunks.emplace_back(start, static_cast<uint32_t>(data.size()));  //作为独立尾块收尾
  }
  return chunks;                                     //返回切块列表
}

/**
 * @brief   按切块生成COPY/LITERAL记录
 */
std::vector<Op> diff_chunks(const std::vector<uint8_t> &old_code,
                            const std::vector<uint8_t> &new_code,
                            const Chunks &old_chunks,
                            const Chunks &new_chunks)
{
  std::unordered_map<uint64_t, std::vector<uint32_t>> index;  //"块CRC+块长"到旧镜像偏移的索引
  //32字节窗口索引：精确块未命中时，用块首窗口在旧镜像里找"内容相同、起点不同"的位置
  constexpr uint32_t kWin = 32u;                     //窗口哈希的采样长度
  std::unordered_map<uint32_t, std::vector<uint32_t>> win_index;  //窗口CRC到旧镜像偏移的索引

  //旧镜像建"块哈希 -> 偏移"索引
  for(const auto &c : old_chunks)  //遍历旧镜像每个切块建立精确索引
  {
    uint32_t len = c.second - c.first;               //该块长度
    uint64_t key = (static_cast<uint64_t>(crc32(old_code.data() + c.first, len)) << 32) | len;  //高32位为CRC、低32位为长度
    index[key].push_back(c.first);                   //登记该块在旧镜像中的起始偏移
  }
  //旧镜像建"窗口哈希 -> 偏移"索引
  for(uint32_t off = 0u; off + kWin <= old_code.size(); off++)  //窗口在旧镜像内逐字节滑动
  {
    win_index[crc32(old_code.data() + off, kWin)].push_back(off);  //按窗口CRC登记起始偏移
  }

  std::vector<Op> ops;  //新镜像的记录列表
  for(const auto &c : new_chunks)                    //逐个新块生成记录
  {
    uint32_t len = c.second - c.first;               //该新块长度
    uint64_t key = (static_cast<uint64_t>(crc32(new_code.data() + c.first, len)) << 32) | len;  //同构的新块键值
    bool found = false;                              //本块是否已生成COPY或DIFF记录

    //哈希命中后必须逐字节确认，避免碰撞
    for(uint32_t off : index[key])                   //遍历同键的全部候选偏移
    {
      if(std::memcmp(old_code.data() + off, new_code.data() + c.first, len) == 0)  //逐字节确认内容一致
      {
        Op op;  //待填充的COPY记录
        op.type = kOpCopy;                           //记录类型为COPY
        op.src = off;                                //源偏移指向旧镜像
        op.len = len;                                //拷贝长度
        ops.push_back(std::move(op));                //加入记录列表
        found = true;                                //标记本块已编码
        break;                                       //命中一个候选即可停止
      }
    }
    //精确块没命中：用窗口哈希做偏移搜索，捞回整体移位但内容相同的块
    if(!found && len >= kWin)                        //块长不小于窗口长度才有搜索意义
    {
      auto it = win_index.find(crc32(new_code.data() + c.first, kWin));  //用新块首窗口查索引
      if(it != win_index.end())                      //旧镜像存在同内容窗口
      {
        for(uint32_t off : it->second)               //遍历候选起始偏移
        {
          if(static_cast<uint64_t>(off) + len > old_code.size())  //候选位置放不下整块
          {
            continue;                                //跳过该候选
          }
          if(std::memcmp(old_code.data() + off, new_code.data() + c.first, len) == 0)  //整块逐字节比对
          {
            Op op;  //待填充的COPY记录
            op.type = kOpCopy;                       //记录类型为COPY
            op.src = off;                            //源偏移指向旧镜像
            op.len = len;                            //拷贝长度
            ops.push_back(std::move(op));            //加入记录列表
            found = true;                            //标记本块已编码
            break;                                   //命中即可停止
          }
        }
      }
    }
    //3)DIFF：在旧镜像同偏移±4KB内找差异最少的块，块内只发差异点
    if(!found && len <= kDiffMax && old_code.size() >= len)    //旧镜像至少能容纳一整块才允许DIFF编码
    {
      uint32_t best_src = c.first;                   //最优源偏移，默认取与新区块相同的偏移
      uint32_t best_count = 0xFFFFFFFFu;             //最优块的差异字节数，初值取最大
      uint64_t lo = (c.first > 4096u) ? (static_cast<uint64_t>(c.first) - 4096u) : 0u;  //搜索下界，最多回退4KB
      uint64_t hi = static_cast<uint64_t>(c.first) + 4096u;  //搜索上界，最多前推4KB
      uint64_t max_off = (old_code.size() >= len) ? (old_code.size() - len) : 0u;  //旧镜像允许的最大起始偏移

      if(hi > max_off)                               //上界不能越过旧镜像可用范围
      {
        hi = max_off;                                //收敛到有效范围
      }
      for(uint64_t off = lo; off <= hi; off++)       //在窗口内逐个候选偏移比较
      {
        uint32_t cnt = 0;                            //该候选偏移下的差异字节数
        for(uint32_t i = 0u; i < len; i++)           //整块逐字节比较
        {
          if(old_code[static_cast<std::size_t>(off) + i] != new_code[c.first + i])  //对应字节不同
          {
            cnt++;                                   //差异计数加一
          }
        }
        if(cnt < best_count)                         //找到差异更少的候选
        {
          best_count = cnt;                          //更新最优差异数
          best_src = static_cast<uint32_t>(off);     //更新最优源偏移
          if(cnt == 0u)                              //已完全一致
          {
            break;                                   //无需继续搜索
          }
        }
      }
      if(best_count == 0u)                           //存在完全一致的块
      {
        Op op;  //待填充的COPY记录
        op.type = kOpCopy;                           //按COPY记录编码
        op.src = best_src;                           //该一致块在旧镜像中的偏移
        op.len = len;                                //覆盖长度
        ops.push_back(std::move(op));                //加入记录列表
        found = true;                                //标记本块已编码
      }
      else if(best_count != 0xFFFFFFFFu && (7u + 3u * best_count) < (3u + len))  //DIFF编码长度必须小于LITERAL
      {
        Op op;  //待填充的DIFF记录
        op.type = kOpDiff;                           //按DIFF记录编码
        op.src = best_src;                           //基块在旧镜像中的偏移
        op.len = len;                                //覆盖长度
        for(uint32_t i = 0u; i < len; i++)           //逐字节求差异
        {
          uint8_t old_b = old_code[static_cast<std::size_t>(best_src) + i];  //旧镜像对应字节
          uint8_t new_b = new_code[c.first + i];     //新镜像对应字节
          if(old_b != new_b)                         //只记录不同的字节
          {
            op.pos.push_back(static_cast<uint16_t>(i));             //差异位置，块内偏移
            op.val.push_back(static_cast<uint8_t>(new_b - old_b));  //差异值，按无符号回绕存差值
          }
        }
        ops.push_back(std::move(op));                //加入记录列表
        found = true;                                //标记本块已编码
      }
    }
    if(!found)                                       //COPY与DIFF都不适用
    {
      Op op;  //待填充的LITERAL记录
      op.type = kOpLiteral;                          //按LITERAL记录原样携带数据
      op.len = len;                                  //数据长度
      op.data.assign(new_code.begin() + c.first, new_code.begin() + c.second);  //拷贝新块内容
      ops.push_back(std::move(op));                  //加入记录列表
    }
  }
  return ops;                                        //返回新镜像的全部记录
}

/**
 * @brief   合并相邻记录，减少记录头开销
 */
std::vector<Op> merge_ops(std::vector<Op> ops, uint32_t literal_merge_max)
{
  std::vector<Op> out;  //合并后的记录列表
  const uint32_t merge_max = std::min<uint32_t>(literal_merge_max, 0xFFFFu);  //LITERAL长度字段只有16位

  for(auto &op : ops)                                //按顺序处理每条记录
  {
    if(op.type == kOpCopy)                           //当前是COPY记录
    {
      if(!out.empty() && out.back().type == kOpCopy  //与上一条同为COPY才可能合并
         && out.back().src + out.back().len == op.src
         && out.back().len + op.len <= 0xFFFFu)      //与上一条首尾相接且长度不超16位上限
      {
        out.back().len += op.len;                    //并入上一条，节省一条记录头
        continue;                                    //本记录已处理完
      }
      out.push_back(std::move(op));                  //无法合并则单独输出
    }
    else if(op.type == kOpLiteral)                   //当前是LITERAL记录
    {
      if(!out.empty() && out.back().type == kOpLiteral
         && out.back().data.size() + op.data.size() <= merge_max)  //合并后不超16位长度上限
      {
        out.back().data.insert(out.back().data.end(), op.data.begin(), op.data.end());  //追加数据
        out.back().len = static_cast<uint32_t>(out.back().data.size());  //同步更新长度字段
        continue;                                    //本记录已处理完
      }
      out.push_back(std::move(op));                  //无法合并则单独输出
    }
    else
    {
      //DIFF不合并
      out.push_back(std::move(op));                  //DIFF记录原样输出
    }
  }
  return out;                                        //返回合并后的记录列表
}

/**
 * @brief   序列化为记录流
 */
std::vector<uint8_t> serialize_ops(const std::vector<Op> &ops)
{
  std::vector<uint8_t> out;  //序列化得到的字节流

  for(const auto &op : ops)                          //逐条序列化记录
  {
    if(op.type == kOpCopy)                           //COPY：type + src(2) + len(2)
    {
      out.push_back(kOpCopy);                        //记录类型
      out.push_back(static_cast<uint8_t>(op.src & 0xFFu));          //源偏移低字节
      out.push_back(static_cast<uint8_t>((op.src >> 8) & 0xFFu));   //源偏移高字节
      out.push_back(static_cast<uint8_t>(op.len & 0xFFu));          //长度低字节
      out.push_back(static_cast<uint8_t>((op.len >> 8) & 0xFFu));   //长度高字节
    }
    else if(op.type == kOpLiteral)                   //LITERAL：type + len(2) + 数据
    {
      out.push_back(kOpLiteral);                     //记录类型
      out.push_back(static_cast<uint8_t>(op.len & 0xFFu));          //长度低字节
      out.push_back(static_cast<uint8_t>((op.len >> 8) & 0xFFu));   //长度高字节
      out.insert(out.end(), op.data.begin(), op.data.end());        //原样数据
    }
    else
    {
      //DIFF：src + len + count + (pos,val)*count
      uint16_t count = static_cast<uint16_t>(op.pos.size());  //差异点个数
      out.push_back(kOpDiff);                        //记录类型
      out.push_back(static_cast<uint8_t>(op.src & 0xFFu));          //基块偏移低字节
      out.push_back(static_cast<uint8_t>((op.src >> 8) & 0xFFu));   //基块偏移高字节
      out.push_back(static_cast<uint8_t>(op.len & 0xFFu));          //覆盖长度低字节
      out.push_back(static_cast<uint8_t>((op.len >> 8) & 0xFFu));   //覆盖长度高字节
      out.push_back(static_cast<uint8_t>(count & 0xFFu));           //差异点个数低字节
      out.push_back(static_cast<uint8_t>((count >> 8) & 0xFFu));    //差异点个数高字节
      for(std::size_t i = 0; i < op.pos.size(); i++)                //逐个输出差异点
      {
        out.push_back(static_cast<uint8_t>(op.pos[i] & 0xFFu));     //块内偏移低字节
        out.push_back(static_cast<uint8_t>((op.pos[i] >> 8) & 0xFFu));  //块内偏移高字节
        out.push_back(op.val[i]);                                   //差异值
      }
    }
  }
  return out;                                        //返回完整记录流
}

/**
 * @brief   统计记录流
 */
DeltaStats collect_stats(const std::vector<Op> &ops, const std::vector<uint8_t> &payload,
                         const Chunks &new_chunks, const char *strategy)
{
  DeltaStats s;  //统计结果，各字段从0开始累计

  s.delta_size = payload.size();                     //记录流总字节数
  s.chunks = static_cast<uint32_t>(new_chunks.size());  //新镜像切块总数
  s.strategy = strategy;                             //生成策略名
  for(const auto &op : ops)                          //逐条累计统计
  {
    if(op.type == kOpCopy)                           //COPY记录
    {
      s.copy_bytes += op.len;                        //累计拷贝字节数
      s.copy_ops++;                                  //拷贝记录条数加一
    }
    else if(op.type == kOpLiteral)                   //LITERAL记录
    {
      s.literal_bytes += op.len;                     //累计字面数据字节数
      s.literal_ops++;                               //字面记录条数加一
    }
    else                                             //DIFF记录
    {
      s.diff_bytes += op.len;                        //累计差异块覆盖字节数
      s.diff_ops++;                                  //差异记录条数加一
    }
  }
  return s;                                          //返回统计结果
}

/**
 * @brief   用一套切块参数生成差分包
 */
bool build_with_chunks(const std::vector<uint8_t> &old_code,
                       const std::vector<uint8_t> &new_code,
                       const Chunks &old_chunks,
                       const Chunks &new_chunks,
                       uint32_t literal_merge_max,
                       const char *strategy,
                       DeltaPack &pack)
{
  auto ops = merge_ops(diff_chunks(old_code, new_code, old_chunks, new_chunks), literal_merge_max);  //生成并合并记录
  auto payload = serialize_ops(ops);                 //序列化成记录流
  DeltaStats stats = collect_stats(ops, payload, new_chunks, strategy);  //统计本组参数的效果

  //本地回放，确认记录流能还原出新代码
  std::vector<uint8_t> check;  //回放得到的新代码区
  if(!apply_delta_stream(old_code, payload, static_cast<uint32_t>(new_code.size()), check))  //按设备端同样的规则回放
  {
    return false;                                    //回放失败，这组记录流不可用
  }
  if(check != new_code)                              //回放结果必须与新代码逐字节一致
  {
    return false;                                    //内容不符，说明记录流有误
  }
  pack.payload = std::move(payload);                 //输出记录流
  pack.stats = stats;                                //输出统计
  return true;                                       //本组参数生成成功
}

} // namespace

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
                        std::vector<uint8_t> &out)
{
  out.clear();                                       //清空输出，从空代码区开始拼装
  out.reserve(code_len);                             //按期望长度预留空间

  std::size_t pos = 0;                               //记录流读取游标
  while(pos < payload.size())                        //顺序解析每条记录
  {
    uint8_t type = payload[pos++];                   //先取出记录类型并前移游标

    if(type == kOpCopy)                              //COPY：从旧镜像按偏移拷贝
    {
      if(pos + 4 > payload.size())                   //剩余数据不足src与len字段
      {
        return false;                                //记录流截断
      }
      uint32_t src = static_cast<uint32_t>(payload[pos])              //源偏移低字节
                   | (static_cast<uint32_t>(payload[pos + 1]) << 8);  //源偏移高字节
      uint32_t len = static_cast<uint32_t>(payload[pos + 2])          //长度低字节
                   | (static_cast<uint32_t>(payload[pos + 3]) << 8);  //长度高字节
      pos += 4;                                      //跳过src与len字段
      if(len == 0 || static_cast<uint64_t>(src) + len > old_code.size())  //长度非法或源区间越界
      {
        return false;                                //拒绝越界拷贝
      }
      if(static_cast<uint64_t>(out.size()) + len > code_len)  //拼装后超出期望长度
      {
        return false;                                //拒绝超长输出
      }
      out.insert(out.end(), old_code.begin() + src, old_code.begin() + src + len);  //追加旧镜像对应区间
    }
    else if(type == kOpLiteral)                      //LITERAL：直接追加携带的数据
    {
      if(pos + 2 > payload.size())                   //剩余数据不足len字段
      {
        return false;                                //记录流截断
      }
      uint32_t len = static_cast<uint32_t>(payload[pos])              //长度低字节
                   | (static_cast<uint32_t>(payload[pos + 1]) << 8);  //长度高字节
      pos += 2;                                      //跳过len字段
      if(len == 0 || pos + len > payload.size())     //长度为0或数据段越界
      {
        return false;                                //拒绝非法长度
      }
      if(static_cast<uint64_t>(out.size()) + len > code_len)  //拼装后超出期望长度
      {
        return false;                                //拒绝超长输出
      }
      out.insert(out.end(), payload.begin() + pos, payload.begin() + pos + len);  //追加字面数据
      pos += len;                                    //游标跳过该数据段
    }
    else if(type == kOpDiff)                         //DIFF：拷贝基块后按差异点修正
    {
      if(pos + 6 > payload.size())                   //剩余数据不足源偏移、长度与差异点个数
      {
        return false;                                //记录流截断
      }
      uint32_t src = static_cast<uint32_t>(payload[pos])              //基块偏移低字节
                   | (static_cast<uint32_t>(payload[pos + 1]) << 8);  //基块偏移高字节
      uint32_t len = static_cast<uint32_t>(payload[pos + 2])          //覆盖长度低字节
                   | (static_cast<uint32_t>(payload[pos + 3]) << 8);  //覆盖长度高字节
      uint32_t count = static_cast<uint32_t>(payload[pos + 4])        //差异点个数低字节
                     | (static_cast<uint32_t>(payload[pos + 5]) << 8);  //差异点个数高字节
      pos += 6;                                      //跳过上述字段
      if(len == 0 || len > kDiffMax || count > len)  //长度越界或差异点数量超过块长
      {
        return false;                                //拒绝非法字段
      }
      if(static_cast<uint64_t>(src) + len > old_code.size())  //基块区间超出旧镜像
      {
        return false;                                //拒绝越界读取
      }
      if(static_cast<uint64_t>(out.size()) + len > code_len)  //拼装后超出期望长度
      {
        return false;                                //拒绝超长输出
      }
      std::size_t base = out.size();                 //记录本块在输出中的起始位置
      out.insert(out.end(), old_code.begin() + src, old_code.begin() + src + len);  //先整块拷贝基块
      for(uint32_t k = 0; k < count; k++)            //逐个应用差异点
      {
        if(pos + 3 > payload.size())                 //剩余数据不足一个差异点
        {
          return false;                              //记录流截断
        }
        uint32_t p = static_cast<uint32_t>(payload[pos])              //块内偏移低字节
                   | (static_cast<uint32_t>(payload[pos + 1]) << 8);  //块内偏移高字节
        uint8_t v = payload[pos + 2];                //差异值
        pos += 3;                                    //跳过该差异点
        if(p >= len)                                 //块内偏移超出本块范围
        {
          return false;                              //拒绝越界修正
        }
        out[base + p] = static_cast<uint8_t>(out[base + p] + v);  //按无符号回绕加回差异值
      }
    }
    else
    {
      return false;                                  //未知记录类型
    }
  }
  return out.size() == code_len;                     //输出长度必须与期望长度一致
}

/**
 * @brief   生成差分记录流
 * @param   old_code  旧镜像代码区
 * @param   new_code  新镜像代码区
 * @param   meta    新镜像元数据
 * @param   opt       参数
 * @return  差分包；payload为空表示失败
 */
DeltaPack make_delta(const std::vector<uint8_t> &old_code,
                     const std::vector<uint8_t> &new_code,
                     const ImageMeta &meta,
                     const DeltaOptions &opt)
{
  DeltaPack pack;  //返回值容器，先填公共字段
  pack.meta = meta;                              //以调用方给出的镜像元数据为基准
  pack.meta.code_crc32 = crc32(new_code.data(), new_code.size());  //补全新代码区CRC32
  pack.meta.image_len = static_cast<uint32_t>(new_code.size()) + kImgMetaLen;  //补全镜像总长度
  pack.base_crc32 = crc32(old_code.data(), old_code.size());  //记录旧代码区CRC32，应用前用于校验基版本

  if(old_code.empty() || new_code.empty())           //任一镜像为空都无法做差分
  {
    return pack;                                     //返回空payload表示失败
  }
  //COPY的src/len与LITERAL的len都只有16位，记录流里能引用的旧代码最长64KB；
  //镜像能否装进槽位由设备端按自己的Flash划分判定，这里不做假设
  const uint32_t max_code_len = 0x10000u;
  if(new_code.size() > max_code_len || old_code.size() > max_code_len
     || new_code.size() < 8u || old_code.size() < 8u)      //设备端同样要求代码长度不小于8字节
  {
    return pack;                                     //超出可编码范围或低于设备端下限，按失败返回
  }

  //多组CDC参数与固定分块各算一遍，取记录流最小的
  DeltaPack best;  //记录流最小的一组结果
  auto consider = [&](DeltaPack &&p)  //比较并保留记录流更小的结果
  {
    p.meta = pack.meta;                          //统一使用调用方给出的新镜像元数据
    p.base_crc32 = pack.base_crc32;                  //统一使用同一个基版本CRC
    if(best.payload.empty() || p.payload.size() < best.payload.size())  //记录流更小才保留
    {
      best = std::move(p);                           //替换当前最优结果
    }
  };
  auto try_cdc = [&](uint32_t min_sz, uint32_t bits, uint32_t max_sz, const char *name)  //按一组CDC参数生成
  {
    DeltaPack p;  //该组参数的生成结果
    if(build_with_chunks(old_code, new_code,
                         chunk_cdc(old_code, min_sz, bits, max_sz),
                         chunk_cdc(new_code, min_sz, bits, max_sz),
                         opt.literal_merge_max, name, p))  //按该组CDC参数生成并自校验
    {
      consider(std::move(p));                        //参与最优结果比较
    }
  };
  auto try_fixed = [&](uint32_t size)  //按固定块长生成
  {
    DeltaPack p;  //该块长的生成结果
    char name[32];  //策略名缓冲
    std::snprintf(name, sizeof(name), "fixed %u", size);  //策略名，用于统计区分块长
    if(build_with_chunks(old_code, new_code,
                         chunk_fixed(old_code, size),
                         chunk_fixed(new_code, size),
                         opt.literal_merge_max, name, p))  //按该固定块长生成并自校验
    {
      consider(std::move(p));                        //参与最优结果比较
    }
  };

  try_cdc(opt.cdc_min, opt.cdc_avg_bits, opt.cdc_max, "cdc 配置值");  //调用方指定的CDC参数
  try_cdc(64u, 7u, 512u, "cdc 64/128/512");          //最小64、平均128、最大512
  try_cdc(128u, 8u, 1024u, "cdc 128/256/1024");      //最小128、平均256、最大1024
  try_cdc(256u, 9u, 2048u, "cdc 256/512/2048");      //最小256、平均512、最大2048
  try_cdc(512u, 10u, 4096u, "cdc 512/1024/4096");    //最小512、平均1024、最大4096
  try_fixed(opt.fixed_size);                         //调用方指定的固定块长
  try_fixed(128u);                                   //固定128字节分块
  try_fixed(256u);                                   //固定256字节分块
  try_fixed(512u);                                   //固定512字节分块
  return best;                                       //返回记录流最小的一组
}

/**
 * @brief   应用差分包
 * @param   old_code  旧镜像代码区
 * @param   pack      差分包
 * @param   out       输出：新代码区
 * @return  true成功，false失败
 */
bool apply_delta(const std::vector<uint8_t> &old_code,
                 const DeltaPack &pack,
                 std::vector<uint8_t> &out)
{
  if(pack.meta.image_len <= kImgMetaLen)            //镜像长度必须大于头部本身
  {
    return false;                                    //长度字段非法
  }
  if(pack.base_crc32 != 0u && crc32(old_code.data(), old_code.size()) != pack.base_crc32)  //基版本必须匹配
  {
    return false;                                    //旧镜像不是该差分包对应的基版本
  }
  uint32_t code_len = pack.meta.image_len - kImgMetaLen;  //由镜像总长推出代码区长度
  if(code_len > 0xFFFFu)                             //设备端记录长度字段只有16位
  {
    return false;                                    //超出协议可还原范围
  }
  if(!apply_delta_stream(old_code, pack.payload, code_len, out))  //按记录流拼装新代码区
  {
    return false;                                    //记录流非法或长度不符
  }
  return crc32(out.data(), out.size()) == pack.meta.code_crc32;  //整体校验新代码区CRC32
}

} // namespace fw

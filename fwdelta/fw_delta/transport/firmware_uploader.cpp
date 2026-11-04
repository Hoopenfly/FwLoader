#include "firmware_uploader.h"    //引入上传器声明

#include <QElapsedTimer>    //引入计时器，用于响应超时判断
#include <QThread>    //引入线程休眠，重试发送前短暂退避

#include <algorithm>    //引入std::min，用于计算末帧长度

namespace
{

//单帧数据上限，与固件FW_IAP_HEX_DATA_MAX一致
constexpr uint32_t kChunkSize = 256u;    //每帧最多携带256字节代码
//单帧等待RESP的超时时间
constexpr int kRespTimeoutMs = 3000;    //3秒未收到RESP即按超时重试
//每帧重试次数
constexpr int kRetryMax = 3;    //同一帧最多发送3次
//差分记录流末帧的等待时间
//末帧写完后设备要解析全部记录并逐页写Flash，耗时随镜像大小增长
//因此这里按分钟级等待，不能沿用单帧的3秒超时
constexpr int kDeltaFinishTimeoutSec = 60;    //最多等1分钟
//差分记录流末帧只发一次
//重发会让设备重复解析同一段记录流并重复写Flash，必须等它把上一帧处理完
constexpr int kDeltaFinishRetry = 1;    //末帧不作重试

} // namespace

/**
 * @brief   构造上传器
 * @param   transport  已打开的传输通道
 */
FirmwareUploader::FirmwareUploader(IFirmwareTransport *transport)
  : transport(transport)    //保存字节流通道，不持有其生命周期
{
}

/**
 * @brief   设置取消检查回调，每帧发送前调用
 * @param   check  返回true表示请求取消
 */
void FirmwareUploader::setCancelCheck(const std::function<bool()> &check)
{
  cancelCheck = check;    //保存回调副本，空回调表示不做取消检查
}

/**
 * @brief   设置日志回调，用于向界面报告等待设备处理等关键节点
 * @param   sink  接收日志文字的回调，空表示不输出
 */
void FirmwareUploader::setLogSink(const std::function<void(const QString &)> &sink)
{
  logSink = sink;    //保存回调副本，空回调表示不输出
}

/**
 * @brief   发送整包固件
 * @param   code      纯代码(不含镜像元数据)
 * @param   meta    镜像元数据参数，函数内补image_len与code_crc32
 * @param   progress  进度回调，参数为百分比、当前帧号、总帧数
 * @return  升级结果
 */
FirmwareUploader::Result FirmwareUploader::sendFull(const std::vector<uint8_t> &code,
                                                    const fw::ImageMeta &meta,
                                                    const std::function<void(int, uint32_t, uint32_t)> &progress)
{
  Result result;    //默认字段表示未成功、未取消

  //每帧发送前检查取消请求
  auto cancelled = [this, &result]()    //返回true表示调用方已请求取消
  {
    if(cancelCheck && cancelCheck())    //回调存在且返回true时判定为取消
    {
      result.cancelled = true;    //标记本次升级被取消
      return true;
    }
    return false;    //没有取消请求
  };

  if(code.empty())    //空固件没有可发送的代码
  {
    result.error = QStringLiteral("固件代码为空");    //交给调用方提示
    return result;    //不进入发送流程
  }

  //镜像元数据中写入代码长度与CRC32
  fw::ImageMeta h = meta;    //复制一份，避免改动调用方传入的镜像元数据
  h.image_len = static_cast<uint32_t>(code.size()) + fw::kImgMetaLen;    //镜像总长=代码长度+64字节头
  h.code_crc32 = fw::crc32(code.data(), code.size());    //设备据此校验收到的代码区
  const std::vector<uint8_t> metaBytes = fw::build_meta(h);    //生成含CRC16的64字节镜像元数据

  uint32_t respOffset = 0;    //设备在RESP里回报的确认位置
  uint8_t status = 0;    //设备在RESP里回报的状态码
  bool fatal = false;    //链路级错误，重试没有意义
  bool wasCancelled = false;    //调用方取消标记
  if(!transact(0, 0, metaBytes, respOffset, status, fatal, wasCancelled))    //类型0为START帧，载荷是镜像元数据
  {
    if(wasCancelled)    //用户主动取消时按取消返回
    {
      result.cancelled = true;    //交给界面显示已取消
    }
    else    //其余原因区分链路失败与应答超时
    {
      result.fatal = fatal;    //把链路状态带给上层，避免靠文案判断
      result.error = fatal ? QStringLiteral("传输链路失败") : QStringLiteral("START 无应答");    //重试耗尽仍没有有效RESP
    }
    return result;
  }
  if(status != 0)    //设备拒绝启动时提前结束，由上层按状态码处理
  {
    result.ok = true;    //已收到设备的有效应答
    result.status = status;    //带回设备状态码
    result.offset = respOffset;    //带回设备确认位置
    return result;
  }

  //按256字节分帧，最后一帧可能更短
  for(uint32_t offset = 0; offset < code.size(); offset += kChunkSize)    //按偏移逐帧推进
  {
    if(cancelled())    //每帧发送前响应取消请求
    {
      return result;
    }
    const uint32_t n = std::min<uint32_t>(kChunkSize,
                                          static_cast<uint32_t>(code.size() - offset));    //本帧实际字节数
    const std::vector<uint8_t> part(code.begin() + offset, code.begin() + offset + n);    //拷出本帧数据

    if(!transact(1, offset, part, respOffset, status, fatal, wasCancelled))    //类型1为数据帧，偏移随帧推进
    {
      if(wasCancelled)    //用户主动取消时按取消返回
      {
        result.cancelled = true;    //交给界面显示已取消
      }
      else    //其余原因区分链路失败与应答超时
      {
        result.fatal = fatal;    //把链路状态带给上层，避免靠文案判断
        result.error = fatal ? QStringLiteral("传输链路失败，偏移 %1").arg(offset)
                             : QStringLiteral("数据帧无应答，偏移 %1").arg(offset);    //注明失败帧位置
      }
      return result;
    }
    if(status != 0)    //设备报错时停止发送，交由上层处理
    {
      result.ok = true;    //已收到设备的有效应答
      result.status = status;    //带回设备状态码
      result.offset = respOffset;    //带回设备确认位置
      return result;
    }
    if(progress)    //有进度回调时按已发送比例上报
    {
      //帧号按256字节一帧折算，总帧数向上取整，供界面显示"帧 x/y"
      const uint32_t totalFrames = static_cast<uint32_t>((code.size() + kChunkSize - 1u) / kChunkSize);
      const uint32_t currentFrame = static_cast<uint32_t>((offset + n + kChunkSize - 1u) / kChunkSize);
      progress(static_cast<int>((static_cast<uint64_t>(offset) + n) * 100u / code.size()),
               currentFrame, totalFrames);    //百分比与帧号一起上报
    }
  }

  result.ok = true;    //全部帧发送完毕
  result.status = 0;    //状态0表示设备全程无报错
  result.offset = static_cast<uint32_t>(code.size());    //设备确认位置即代码长度
  return result;
}

/**
 * @brief   发送差分包
 * @param   pack      差分包
 * @param   progress  进度回调，参数为百分比、当前帧号、总帧数
 * @return  升级结果；status为0x0E/0x10时由调用方决定是否回退整包
 */
FirmwareUploader::Result FirmwareUploader::sendDelta(const fw::DeltaPack &pack,
                                                     const std::function<void(int, uint32_t, uint32_t)> &progress)
{
  Result result;    //默认字段表示未成功、未取消

  //每帧发送前检查取消请求
  auto cancelled = [this, &result]()    //返回true表示调用方已请求取消
  {
    if(cancelCheck && cancelCheck())    //回调存在且返回true时判定为取消
    {
      result.cancelled = true;    //标记本次升级被取消
      return true;
    }
    return false;    //没有取消请求
  };

  const std::vector<uint8_t> meta = fw::build_meta(pack.meta);    //生成新的64字节镜像元数据
  if(pack.payload.empty())    //空记录流无法组成有效的升级请求
  {
    result.error = QStringLiteral("差分记录流为空");    //交给调用方提示
    return result;    //不发送任何帧
  }
  std::vector<uint8_t> start;    //START_DELTA帧载荷=镜像元数据+基版本CRC32+记录流长度
  start.reserve(meta.size() + 8u);    //预先留出两个32位字段的空间
  start.insert(start.end(), meta.begin(), meta.end());    //先放入镜像元数据
  putU32(start, pack.base_crc32);    //再放基版本代码区CRC32，设备据此校验旧镜像
  putU32(start, static_cast<uint32_t>(pack.payload.size()));    //最后放记录流字节数

  uint32_t respOffset = 0;    //设备在RESP里回报的确认位置
  uint8_t status = 0;    //设备在RESP里回报的状态码
  bool fatal = false;    //链路级错误，重试没有意义
  bool wasCancelled = false;    //调用方取消标记
  if(!transact(3, 0, start, respOffset, status, fatal, wasCancelled))    //类型3为START_DELTA帧
  {
    if(wasCancelled)    //用户主动取消时按取消返回
    {
      result.cancelled = true;    //交给界面显示已取消
    }
    else    //其余原因区分链路失败与应答超时
    {
      result.fatal = fatal;    //把链路状态带给上层，避免靠文案判断
      result.error = fatal ? QStringLiteral("传输链路失败") : QStringLiteral("START_DELTA 无应答");    //重试耗尽仍没有有效RESP
    }
    return result;
  }
  //0x0E/0x10由调用方决定是否回退整包
  if(status != 0)    //设备拒绝差分时提前结束，由上层决定是否回退整包
  {
    result.ok = true;    //已收到设备的有效应答
    result.status = status;    //带回设备状态码，可能是0x0E或0x10
    result.offset = respOffset;    //带回设备确认位置
    return result;
  }

  for(uint32_t offset = 0; offset < pack.payload.size(); offset += kChunkSize)    //按偏移逐帧推进
  {
    if(cancelled())    //每帧发送前响应取消请求
    {
      return result;
    }
    const uint32_t n = std::min<uint32_t>(kChunkSize,
                                          static_cast<uint32_t>(pack.payload.size() - offset));    //本帧实际字节数
    const std::vector<uint8_t> part(pack.payload.begin() + offset,
                                    pack.payload.begin() + offset + n);    //拷出本帧记录流

    //末帧写完后设备要解析全部记录并逐页写Flash，必须按分钟级等待，且不能重发
    const bool lastFrame = (offset + n >= pack.payload.size());
    if(lastFrame && logSink)    //先告诉操作者设备端开始重建镜像，避免界面看起来卡住
    {
      logSink(QStringLiteral("差分包已全部发出，设备端正在重建镜像，最长等待 %1 秒")
              .arg(kDeltaFinishTimeoutSec));
    }

    if(!transact(1, offset, part, respOffset, status, fatal, wasCancelled,
                 lastFrame ? kDeltaFinishTimeoutSec * 1000 : 0,
                 lastFrame ? kDeltaFinishRetry : 0))    //差分记录流同样用类型1数据帧
    {
      if(wasCancelled)    //用户主动取消时按取消返回
      {
        result.cancelled = true;    //交给界面显示已取消
      }
      else    //其余原因区分链路失败与应答超时
      {
        result.fatal = fatal;    //把链路状态带给上层，避免靠文案判断
        if(fatal)    //链路已断
        {
          result.error = QStringLiteral("传输链路失败，偏移 %1").arg(offset);
        }
        else if(lastFrame)    //末帧超时说明设备没能在限定时间内重建完镜像
        {
          result.error = QStringLiteral("设备端重建镜像超时，偏移 %1").arg(offset);
        }
        else    //普通数据帧超时
        {
          result.error = QStringLiteral("差分数据帧无应答，偏移 %1").arg(offset);
        }
      }
      return result;
    }
    if(status != 0)    //设备报错时停止发送，交由上层处理
    {
      result.ok = true;    //已收到设备的有效应答
      result.status = status;    //带回设备状态码
      result.offset = respOffset;    //带回设备确认位置
      return result;
    }
    if(progress)    //有进度回调时按已发送比例上报
    {
      //差分记录流同样按256字节一帧折算帧号
      const uint32_t totalFrames =
          static_cast<uint32_t>((pack.payload.size() + kChunkSize - 1u) / kChunkSize);
      const uint32_t currentFrame =
          static_cast<uint32_t>((offset + n + kChunkSize - 1u) / kChunkSize);
      progress(static_cast<int>((static_cast<uint64_t>(offset) + n) * 100u
                                / pack.payload.size()),
               currentFrame, totalFrames);    //百分比与帧号一起上报
    }
  }

  result.ok = true;    //全部帧发送完毕
  result.status = 0;    //状态0表示设备全程无报错
  result.offset = static_cast<uint32_t>(pack.payload.size());    //确认位置即记录流长度
  return result;
}

/**
 * @brief   发送一帧并等待RESP，失败自动重试
 * @param   type       帧类型，写入类型字节的低4位
 * @param   offset     本帧在代码区或记录流中的偏移
 * @param   payload    本帧载荷
 * @param   respOffset 输出：设备回报的确认位置
 * @param   status     输出：设备回报的状态码
 * @return  true表示收到有效RESP
 */
bool FirmwareUploader::transact(uint8_t type, uint32_t offset,
                                const std::vector<uint8_t> &payload,
                                uint32_t &respOffset, uint8_t &status,
                                bool &fatal, bool &cancelled,
                                int timeoutMs, int retryMax)
{
  fatal = false;    //默认没有链路级错误
  cancelled = false;    //默认没有取消

  const int waitMs = (timeoutMs > 0) ? timeoutMs : kRespTimeoutMs;    //0表示取默认超时
  const int attempts = (retryMax > 0) ? retryMax : kRetryMax;    //0表示取默认尝试次数

  for(int retry = 0; retry < attempts; retry++)    //同一帧最多尝试attempts次
  {
    if(cancelCheck && cancelCheck())    //重试期间也要响应取消，避免长时间无反应
    {
      cancelled = true;    //向上层标记取消
      return false;    //停止本次帧的发送
    }

    if(!sendFrame(type, offset, payload))    //写入失败说明链路已不可用
    {
      fatal = true;    //链路级错误，重试没有意义
      return false;    //交由上层按“传输链路失败”上报
    }

    uint32_t gotOffset = 0;    //本次RESP回报的确认位置
    uint8_t gotStatus = 0;    //本次RESP回报的状态码
    bool gotCancelled = false;    //等待期间用户是否取消
    if(!readResp(gotOffset, gotStatus, waitMs, fatal, gotCancelled))    //等待并解析RESP
    {
      if(gotCancelled)    //等待期间被取消，不必重试
      {
        cancelled = true;
        return false;
      }
      if(fatal)    //链路已断开，重试没有意义
      {
        return false;    //交由上层按链路失败处理
      }
      continue;    //超时则重试
    }

    //数据帧的成功应答，偏移必须等于本帧写完后的下一个期望位置，否则视为上一帧的旧应答
    //失败应答携带的是设备期望位置（不连续时为received_size，fail()后为0），不能按同一规则比对，
    //必须原样上报，否则设备返回的错误码会被全部丢弃，界面只能显示“无应答”
    if(type == 1u && gotStatus == 0u
       && gotOffset != offset + static_cast<uint32_t>(payload.size()))    //类型1为数据帧
    {
      continue;    //旧的成功应答，丢弃后重发本帧
    }

    respOffset = gotOffset;    //回填设备确认位置
    status = gotStatus;    //回填设备状态码
    return true;    //收到有效RESP
  }
  return false;    //重试耗尽仍无有效RESP
}

/**
 * @brief   组帧并写入传输通道
 * @param   type     帧类型
 * @param   offset   本帧偏移
 * @param   payload  本帧载荷
 * @return  true表示整帧已写入
 */
bool FirmwareUploader::sendFrame(uint8_t type, uint32_t offset,
                                 const std::vector<uint8_t> &payload)
{
  std::vector<uint8_t> frame;    //待发送的完整帧
  if(payload.size() > 0xFFFFu)    //载荷长度字段只有16位，超长帧无法表示
  {
    return false;    //交给调用方按发送失败处理
  }
  frame.reserve(9u + payload.size() + 2u);    //帧头9字节+载荷+CRC16共2字节

  //帧头：55 AA + 版本/类型 + 偏移 + 长度
  frame.push_back(0x55u);    //帧起始字节1
  frame.push_back(0xAAu);    //帧起始字节2，与前一字节组成SOF
  frame.push_back(static_cast<uint8_t>(0x10u | type));    //高4位为版本1，低4位为帧类型
  putU32(frame, offset);    //本帧在代码区或记录流中的偏移，小端
  putU16(frame, static_cast<uint16_t>(payload.size()));    //载荷字节数，小端
  frame.insert(frame.end(), payload.begin(), payload.end());    //追加本帧数据

  //CRC16覆盖SOF到DATA
  putU16(frame, fw::crc16(frame.data(), frame.size()));    //校验范围为帧头到载荷

  return transport->write(frame.data(), frame.size());    //整帧交给传输层发送
}

/**
 * @brief   等待并解析一帧RESP
 * @param   offset    输出：设备确认位置
 * @param   status    输出：设备状态码
 * @param   timeoutMs 等待超时时间
 * @return  true表示解析到一帧校验正确的RESP
 */
bool FirmwareUploader::readResp(uint32_t &offset, uint8_t &status, int timeoutMs, bool &fatal, bool &cancelled)
{
  fatal = false;    //默认没有链路级错误
  cancelled = false;    //默认没有被取消
  std::vector<uint8_t> buffer;    //累积收到的字节，便于处理半包与粘包
  uint8_t chunk[256];    //单次读取的临时缓冲，放在栈上避免频繁分配
  QElapsedTimer timer;    //使用单调计时器统计已等待时间
  std::size_t scanned = 0;    //已确认不可能构成帧起始的前缀长度，避免每轮从头重扫

  timer.start();    //开始计时
  while(timer.elapsed() < timeoutMs)    //在总超时时间内反复尝试
  {
    if(cancelCheck && cancelCheck())    //用户取消时立即退出，不必等满超时
    {
      cancelled = true;
      return false;
    }
    const int n = transport->read(chunk, sizeof(chunk), 10);    //最多等10毫秒读一段数据
    if(n < 0)    //传输层报告链路错误
    {
      fatal = true;    //标记链路失败，上层立即终止
      return false;    //不再等待剩余超时
    }
    if(n > 0)    //读到数据就追加到累积缓冲
    {
      buffer.insert(buffer.end(), chunk, chunk + n);    //把新字节拼到已有数据之后
    }
    //从上次确认过的位置往后找完整的RESP帧
    std::size_t i = scanned;
    while(i + 12 <= buffer.size())    //帧最短12字节，保证下标不越界
    {
      if(buffer[i] != 0x55u || buffer[i + 1] != 0xAAu)    //跳过不是帧起始的位置
      {
        i++;
        continue;
      }
      if((buffer[i + 2] & 0x0Fu) != 0x02u)    //高4位为版本，低4位为2表示RESP帧
      {
        i++;
        continue;
      }
      const std::size_t len = static_cast<std::size_t>(buffer[i + 7])
                            | (static_cast<std::size_t>(buffer[i + 8]) << 8);    //载荷长度，小端16位
      if(len < 1u)    //RESP至少带1字节状态码，空载荷帧按非法帧跳过
      {
        i++;
        continue;
      }
      const std::size_t end = i + 9 + len + 2;    //帧尾=帧头9字节+载荷+CRC16
      if(buffer.size() < end)    //数据还没收全，等下一轮再试
      {
        break;    //保留当前位置，下次仍有数据时从帧头继续
      }
      const uint16_t crcCalc = fw::crc16(buffer.data() + i, 9 + len);    //按收到的内容重算CRC16
      const uint16_t crcGot = static_cast<uint16_t>(buffer[i + 9 + len])
                            | (static_cast<uint16_t>(buffer[i + 9 + len + 1]) << 8);    //取出帧尾CRC16
      if(crcCalc != crcGot)    //校验不符说明数据有误，继续向后找下一帧
      {
        i++;
        continue;
      }
      offset = static_cast<uint32_t>(buffer[i + 3])
             | (static_cast<uint32_t>(buffer[i + 4]) << 8)
             | (static_cast<uint32_t>(buffer[i + 5]) << 16)
             | (static_cast<uint32_t>(buffer[i + 6]) << 24);    //拼出小端32位确认位置
      status = buffer[i + 9];    //载荷首字节为状态码
      return true;
    }
    scanned = i;    //记住扫描进度，下一轮从这里继续
    //设备持续输出文本时缓冲会一直变长，把已确认无用的前缀丢掉
    if(scanned >= 4096u)
    {
      buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(scanned));
      scanned = 0;
    }
  }
  return false;    //超时仍未解析到有效RESP
}

/**
 * @brief   写小端16位
 * @param   out    输出缓冲
 * @param   value  待写入的值
 */
void FirmwareUploader::putU16(std::vector<uint8_t> &out, uint16_t value)
{
  out.push_back(static_cast<uint8_t>(value & 0xFFu));    //先写低字节
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));    //再写高字节
}

/**
 * @brief   写小端32位
 * @param   out    输出缓冲
 * @param   value  待写入的值
 */
void FirmwareUploader::putU32(std::vector<uint8_t> &out, uint32_t value)
{
  out.push_back(static_cast<uint8_t>(value & 0xFFu));    //第0字节：最低8位
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));    //第1字节：次低8位
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));    //第2字节：次高8位
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));    //第3字节：最高8位
}

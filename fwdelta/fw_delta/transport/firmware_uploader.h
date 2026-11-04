#ifndef FIRMWAREUPLOADER_H      //防止头文件被重复包含
#define FIRMWAREUPLOADER_H      //定义头文件保护宏

#include <cstdint>      //uint8_t、uint32_t等定长整型
#include <functional>   //std::function，进度与取消回调
#include <vector>       //std::vector，帧数据与固件代码

#include "fw_delta_codec.h"  //差分格式定义
#include "fw_image.h"        //镜像元数据定义
#include "transport.h"       //传输接口定义

/**
 * @brief   固件升级协议层
 * @note    负责HEX帧、整包/差分发送、重试与进度；
 *          字节流由IFirmwareTransport提供，换传输方式不用改这里。
 */
class FirmwareUploader
{
public:
  //一次升级的结果
  struct Result
  {
    bool ok = false;          //是否收到有效响应
    bool cancelled = false;   //是否被调用方主动取消
    bool fatal = false;       //传输链路已断开，重试没有意义
    uint8_t status = 0xFF;    //设备状态码
    uint32_t offset = 0;      //设备确认位置
    QString error;            //错误描述
  };

  /**
   * @brief   构造上传器
   * @param   transport  已打开的传输通道
   */
  explicit FirmwareUploader(IFirmwareTransport *transport);

  /**
   * @brief   设置取消检查回调，每帧发送前调用
   * @param   check  返回true表示请求取消
   */
  void setCancelCheck(const std::function<bool()> &check);

  /**
   * @brief   设置日志回调，用于向界面报告等待设备处理等关键节点
   * @param   sink  接收日志文字的回调，空表示不输出
   */
  void setLogSink(const std::function<void(const QString &)> &sink);

  /**
   * @brief   发送整包固件
   * @param   code      纯代码(不含镜像元数据)
   * @param   meta      镜像元数据参数，函数内补image_len与code_crc32
   * @param   progress  进度回调，参数为百分比、当前帧号、总帧数
   * @return  升级结果
   */
  Result sendFull(const std::vector<uint8_t> &code,
                  const fw::ImageMeta &meta,
                  const std::function<void(int, uint32_t, uint32_t)> &progress);

  /**
   * @brief   发送差分包
   * @param   pack      差分包
   * @param   progress  进度回调，参数为百分比、当前帧号、总帧数
   * @return  升级结果；status为0x0E/0x10时由调用方决定是否回退整包
   */
  Result sendDelta(const fw::DeltaPack &pack,
                   const std::function<void(int, uint32_t, uint32_t)> &progress);

private:
  /**
   * @brief   发送一帧并等待RESP，失败自动重试
   * @param   type       帧类型，写入类型字节的低4位
   * @param   offset     本帧在代码区或记录流中的偏移
   * @param   payload    本帧载荷
   * @param   respOffset 输出：设备回报的确认位置
   * @param   status     输出：设备回报的状态码
   * @param   fatal      输出：true表示传输链路已断开，不必重试
   * @param   cancelled  输出：true表示调用方已请求取消
   * @param   timeoutMs  单次等待RESP的超时时间，0表示取默认值
   * @param   retryMax   最大尝试次数，0表示取默认值
   * @return  true表示收到有效RESP
   */
  bool transact(uint8_t type, uint32_t offset,
                const std::vector<uint8_t> &payload,
                uint32_t &respOffset, uint8_t &status,
                bool &fatal, bool &cancelled,
                int timeoutMs = 0, int retryMax = 0);

  /**
   * @brief   组帧并写入传输通道
   */
  bool sendFrame(uint8_t type, uint32_t offset,
                 const std::vector<uint8_t> &payload);

  /**
   * @brief   等待并解析一帧RESP
   * @param   offset    输出：设备确认位置
   * @param   status    输出：设备状态码
   * @param   timeoutMs 等待超时时间
   * @param   fatal     输出：true表示链路已断开
   * @param   cancelled 输出：true表示调用方已请求取消
   * @return  true表示解析到一帧校验正确的RESP
   */
  bool readResp(uint32_t &offset, uint8_t &status, int timeoutMs, bool &fatal, bool &cancelled);

  /**
   * @brief   写小端16位
   */
  static void putU16(std::vector<uint8_t> &out, uint16_t value);

  /**
   * @brief   写小端32位
   */
  static void putU32(std::vector<uint8_t> &out, uint32_t value);

  IFirmwareTransport *transport;   //字节流通道
  std::function<bool()> cancelCheck;  //取消检查回调，空表示不检查
  std::function<void(const QString &)> logSink;  //界面日志回调，空表示不输出
};

#endif // FIRMWAREUPLOADER_H

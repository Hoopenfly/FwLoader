#ifndef TRANSPORT_H      //防止头文件被重复包含
#define TRANSPORT_H      //定义头文件保护宏

#include <QString>       //QString，用于错误信息与客户端标识
#include <QStringList>   //字符串列表，保存客户端标识
#include <QVariantList>  //通用类型列表，保存客户端明细
#include <QtGlobal>      //Qt全局定义，提供Q_UNUSED等宏

#include <cstddef>       //std::size_t，表示字节数
#include <cstdint>       //uint8_t等定长整型

/**
 * @brief   固件传输接口
 * @note    只负责字节流读写，不关心HEX帧、差分格式和升级流程；
 *          串口、TCP、HTTP等传输方式各自实现本接口。
 */
class IFirmwareTransport
{
public:
  virtual ~IFirmwareTransport() = default;  //虚析构，允许通过接口指针释放实现对象

  /**
   * @brief   打开传输通道
   * @param   error  输出：失败原因
   * @return  true成功
   */
  virtual bool open(QString &error) = 0;

  /**
   * @brief   关闭传输通道
   */
  virtual void close() = 0;

  /**
   * @brief   写入一段字节
   * @param   data  数据
   * @param   len   字节数
   * @return  true成功
   */
  virtual bool write(const uint8_t *data, std::size_t len) = 0;

  /**
   * @brief   读取一段字节
   * @param   data       输出缓冲
   * @param   len        期望读取的最大字节数
   * @param   timeoutMs  超时时间
   * @return  实际读取字节数，0表示超时，<0表示失败
   */
  virtual int read(uint8_t *data, std::size_t len, int timeoutMs) = 0;

  /**
   * @brief   轮询传输层状态
   * @note    服务器模式用于接受新客户端，其他传输方式无需处理
   */
  virtual void poll() {}

  /**
   * @brief   获取已连接的客户端列表
   * @return  客户端标识列表，没有客户端时为空
   */
  virtual QStringList clientList() const { return QStringList(); }

  /**
   * @brief   获取客户端明细列表
   * @return  每项包含序号、时间、IP、端口
   */
  virtual QVariantList clientRows() const { return QVariantList(); }

  /**
   * @brief   选择当前使用的客户端
   * @param   index  客户端索引
   */
  virtual void setClient(int index) { Q_UNUSED(index); }
};

#endif // TRANSPORT_H

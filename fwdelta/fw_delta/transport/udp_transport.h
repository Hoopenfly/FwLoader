#ifndef UDPTRANSPORT_H      //防止头文件被重复包含
#define UDPTRANSPORT_H      //定义头文件保护宏

#include <QHostAddress>  //IP地址与主机地址
#include <QString>       //QString，保存主机名与错误信息
#include <QUdpSocket>    //UDP套接字

#include "transport.h"   //传输接口定义

/**
 * @brief   UDP传输实现
 * @note    客户端发送到目标地址；服务器模式绑定端口接收并回复最近来源。
 */
class UdpTransport : public IFirmwareTransport
{
public:
  /**
   * @brief   构造UDP传输
   * @param   host    目标IP或主机名
   * @param   port    目标端口
   * @param   server  true为服务器模式，false为客户端模式
   */
  UdpTransport(const QString &host, quint16 port, bool server = false);

  ~UdpTransport() override;  //析构，关闭套接字

  /**
   * @brief   解析地址并绑定或连接套接字
   * @param   error  输出：失败原因
   * @return  true成功
   */
  bool open(QString &error) override;

  /**
   * @brief   关闭套接字
   */
  void close() override;

  /**
   * @brief   发送一段字节
   * @param   data  数据
   * @param   len   字节数
   * @return  true成功
   */
  bool write(const uint8_t *data, std::size_t len) override;

  /**
   * @brief   读取一段字节，服务器模式记录最近来源
   * @param   data       输出缓冲
   * @param   len        期望读取的最大字节数
   * @param   timeoutMs  超时时间
   * @return  实际读取字节数，0表示超时，<0表示失败
   */
  int read(uint8_t *data, std::size_t len, int timeoutMs) override;

private:
  QUdpSocket socket;      //UDP套接字
  QString host;           //原始目标文本，IP字面量或主机名
  QHostAddress address;   //目标地址
  QHostAddress lastFrom;  //最近一次来源地址
  quint16 lastPort;       //最近一次来源端口
  quint16 port;           //目标端口
  bool serverMode;        //是否服务器模式
};

#endif // UDPTRANSPORT_H

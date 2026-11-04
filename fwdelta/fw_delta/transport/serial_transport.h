#ifndef SERIALTRANSPORT_H      //防止头文件被重复包含
#define SERIALTRANSPORT_H      //定义头文件保护宏

#include <QSerialPort>  //串口设备类
#include <QString>      //QString，保存串口名

#include "transport.h"  //传输接口定义

/**
 * @brief   串口传输实现
 * @note    串口参数由界面传入，默认8N1、无流控。
 */
class SerialTransport : public IFirmwareTransport
{
public:
  /**
   * @brief   构造串口传输
   * @param   portName    串口名，例如COM5
   * @param   baud        波特率
   * @param   dataBits    数据位，5到8
   * @param   stopBits    停止位，1、15或2
   * @param   parity      校验位，0无、1奇、2偶
   * @param   flowControl 流控，0无、1硬件
   */
  SerialTransport(const QString &portName, int baud,
                  int dataBits = 8, int stopBits = 1,
                  int parity = 0, int flowControl = 0);

  ~SerialTransport() override;  //析构，关闭串口

  /**
   * @brief   打开串口并按构造参数配置
   * @param   error  输出：失败原因
   * @return  true成功
   */
  bool open(QString &error) override;

  /**
   * @brief   关闭串口
   */
  void close() override;

  /**
   * @brief   向串口写入一段字节
   * @param   data  数据
   * @param   len   字节数
   * @return  true成功
   */
  bool write(const uint8_t *data, std::size_t len) override;

  /**
   * @brief   从串口读取一段字节
   * @param   data       输出缓冲
   * @param   len        期望读取的最大字节数
   * @param   timeoutMs  超时时间
   * @return  实际读取字节数，0表示超时，<0表示失败
   */
  int read(uint8_t *data, std::size_t len, int timeoutMs) override;

private:
  QSerialPort port;      //串口对象
  QString portName;      //串口名
  int baud;              //波特率
  int dataBits;          //数据位
  int stopBits;          //停止位
  int parity;            //校验位
  int flowControl;       //流控
  bool linkBroken = false;  //驱动已报告设备被移除，读写都按链路失效处理
  int probeTickPoll = 0;    //非阻塞读的调用计数，用于周期确认端口是否还在
  int probeTickWait = 0;    //带超时读的等待轮次计数，与非阻塞路径分开统计
};

#endif // SERIALTRANSPORT_H

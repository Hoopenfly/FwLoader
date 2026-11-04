#include "serial_transport.h"    //引入串口传输类的声明

#include <QElapsedTimer>    //引入计时器，用于实现读超时
#include <QSerialPortInfo>  //引入串口枚举，用于确认设备是否仍插在系统上

#include <cstring>    //引入memcpy，把读到的字节拷入调用方缓冲区

namespace
{

/**
 * @brief   把界面数据位映射为Qt枚举
 * @param   value  数据位
 * @return  Qt数据位
 */
QSerialPort::DataBits toDataBits(int value)
{
  switch(value)    //按界面选择的数据位逐项匹配
  {
    case 5:    //界面选择5位数据位
      return QSerialPort::Data5;    //返回Qt的5位数据位枚举
    case 6:    //界面选择6位数据位
      return QSerialPort::Data6;    //返回Qt的6位数据位枚举
    case 7:    //界面选择7位数据位
      return QSerialPort::Data7;    //返回Qt的7位数据位枚举
    default:    //未列举的取值统一按8位处理，也是串口常用配置
      return QSerialPort::Data8;    //返回Qt的8位数据位枚举
  }
}

/**
 * @brief   把界面停止位映射为Qt枚举
 * @param   value  停止位，1、15或2
 * @return  Qt停止位
 */
QSerialPort::StopBits toStopBits(int value)
{
  if(value == 2)    //2表示2位停止位
  {
    return QSerialPort::TwoStop;    //返回Qt的2位停止位枚举
  }
  if(value == 15)    //15表示1.5位停止位，界面上用整数15表示
  {
    return QSerialPort::OneAndHalfStop;    //返回Qt的1.5位停止位枚举
  }
  return QSerialPort::OneStop;    //默认按1位停止位处理
}

/**
 * @brief   把界面校验位映射为Qt枚举
 * @param   value  校验位，0无、1奇、2偶
 * @return  Qt校验位
 */
QSerialPort::Parity toParity(int value)
{
  if(value == 1)    //1表示奇校验
  {
    return QSerialPort::OddParity;    //返回Qt的奇校验枚举
  }
  if(value == 2)    //2表示偶校验
  {
    return QSerialPort::EvenParity;    //返回Qt的偶校验枚举
  }
  return QSerialPort::NoParity;    //0或未列举取值按无校验处理
}

/**
 * @brief   把界面流控映射为Qt枚举
 * @param   value  流控，0无、1硬件
 * @return  Qt流控
 */
QSerialPort::FlowControl toFlowControl(int value)
{
  if(value == 1)    //1表示硬件流控(RTS/CTS)
  {
    return QSerialPort::HardwareControl;    //返回Qt的硬件流控枚举
  }
  return QSerialPort::NoFlowControl;    //0或未列举取值按无流控处理
}

} // namespace

/**
 * @brief   构造串口传输
 * @param   portName    串口名，例如COM5
 * @param   baud        波特率
 * @param   dataBits    数据位，5到8
 * @param   stopBits    停止位，1、15或2
 * @param   parity      校验位，0无、1奇、2偶
 * @param   flowControl 流控，0无、1硬件
 */
SerialTransport::SerialTransport(const QString &portName, int baud,
                                 int dataBits, int stopBits,
                                 int parity, int flowControl)
  : portName(portName), baud(baud), dataBits(dataBits),    //保存串口名、波特率与数据位
    stopBits(stopBits), parity(parity), flowControl(flowControl)    //保存停止位、校验位与流控
{
}

/**
 * @brief   析构时关闭串口，避免句柄泄漏
 */
SerialTransport::~SerialTransport()
{
  SerialTransport::close();    //析构时显式调用本类close，不走虚分派
}

/**
 * @brief   按构造参数打开串口
 * @param   error  输出：失败原因
 * @return  true成功
 */
bool SerialTransport::open(QString &error)
{
  //按界面参数配置串口
  port.setPortName(portName);    //设置串口名，例如COM5
  port.setBaudRate(baud);    //设置波特率
  port.setDataBits(toDataBits(dataBits));    //设置数据位，界面整数映射为Qt枚举
  port.setParity(toParity(parity));    //设置校验方式
  port.setStopBits(toStopBits(stopBits));    //设置停止位
  port.setFlowControl(toFlowControl(flowControl));    //设置流控方式

  if(!port.open(QIODevice::ReadWrite))    //以读写方式打开串口，失败时返回原因
  {
    error = port.errorString();    //把Qt的错误描述交给调用方展示
    return false;
  }

  linkBroken = false;    //新打开的句柄重新开始
  probeTickPoll = 0;    //计数从零开始
  probeTickWait = 0;
  //重复打开时必须先断开上一次的连接，否则信号会重复触发
  QObject::disconnect(&port, &QSerialPort::errorOccurred, nullptr, nullptr);
  //USB转串口被拔出时Qt只通过该信号报告，主动记下来供读写路径判断
  QObject::connect(&port, &QSerialPort::errorOccurred, [this](QSerialPort::SerialPortError code)
  {
    if(code == QSerialPort::ResourceError)    //设备被移除或端口失效
    {
      linkBroken = true;    //后续读写一律按链路失败返回
    }
  });
  return true;    //打开成功
}

/**
 * @brief   关闭串口，未打开时不做处理
 */
void SerialTransport::close()
{
  if(port.isOpen())    //仅在串口处于打开状态时才关闭
  {
    port.close();    //关闭串口并释放系统句柄
  }
  linkBroken = false;    //关闭后不再保留失效标记
}

/**
 * @brief   向串口写入一段字节
 * @param   data  数据
 * @param   len   字节数
 * @return  true表示字节已全部写入
 */
bool SerialTransport::write(const uint8_t *data, std::size_t len)
{
  if(linkBroken || !port.isOpen())    //链路已失效或串口未打开时无法发送
  {
    return false;
  }
  const qint64 written = port.write(reinterpret_cast<const char *>(data),
                                    static_cast<qint64>(len));    //按Qt接口的长度类型写入原始字节
  if(written != static_cast<qint64>(len))    //写入长度不足说明发送失败
  {
    return false;
  }
  //等待数据交给驱动；个别USB转串口驱动会误报超时，因此只在仍有数据没发出去时才判定失败
  if(!port.waitForBytesWritten(1000) && port.bytesToWrite() > 0)    //等待失败且发送缓冲仍未清空
  {
    return false;    //端口已失效，交给上层按链路故障处理
  }
  return true;
}

/**
 * @brief   在超时时间内读取一段字节
 * @param   data       输出缓冲
 * @param   len        期望读取的最大字节数
 * @param   timeoutMs  超时时间
 * @return  实际读取字节数，0表示超时
 */
int SerialTransport::read(uint8_t *data, std::size_t len, int timeoutMs)
{
  if(len == 0)    //请求长度为0时直接返回
  {
    return 0;
  }
  if(linkBroken || !port.isOpen())    //驱动报告设备已移除或串口已关闭
  {
    return -1;    //交给上层按链路失败处理
  }

  //非阻塞模式：只取走缓冲里已有的数据，一字节也不等
  if(timeoutMs <= 0)
  {
    //USB转串口被拔出后isOpen()与error()都可能维持原状，定期确认端口是否还在系统里
    if(++probeTickPoll >= 10)    //界面每200毫秒轮询一次，约2秒确认一次
    {
      probeTickPoll = 0;    //重新计数
      bool present = false;    //端口是否仍在可用列表里
      const QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
      for(const QSerialPortInfo &info : ports)    //逐个比对端口名
      {
        if(info.portName().compare(portName, Qt::CaseInsensitive) == 0)
        {
          present = true;    //找到同名端口
          break;
        }
      }
      if(!present)    //系统里已经没有该端口，说明设备被移除
      {
        linkBroken = true;    //记下失效状态，后续调用直接返回失败
        return -1;    //交给上层按链路失败处理
      }
    }

    const qint64 avail = port.bytesAvailable();    //当前已到达的字节数
    if(avail <= 0)
    {
      return 0;    //没有数据即返回，不进入等待循环
    }
    const qint64 want = qMin<qint64>(avail, static_cast<qint64>(len));    //最多取请求长度
    return static_cast<int>(port.read(reinterpret_cast<char *>(data), want));
  }

  //先查缓冲再等待：部分USB转串口驱动会返回超时，但数据其实已经到达
  QElapsedTimer timer;    //使用单调计时器统计已等待时间
  timer.start();    //开始计时
  while(timer.elapsed() < timeoutMs)    //在总超时时间内反复尝试
  {
    if(port.bytesAvailable() > 0)    //缓冲里已有数据就先取走
    {
      const QByteArray bytes = port.read(static_cast<qint64>(len));    //最多读len字节，不阻塞
      const int count = static_cast<int>(bytes.size());    //本次实际读到的字节数

      if(count > 0)    //读到数据才拷贝并返回
      {
        std::memcpy(data, bytes.constData(), static_cast<std::size_t>(count));    //拷入调用方缓冲
        return count;
      }
    }
    if(!port.waitForReadyRead(10))    //最多等10毫秒新数据
    {
      if(!port.isOpen() || port.error() == QSerialPort::ResourceError)    //设备被拔出或端口异常
      {
        linkBroken = true;    //记下失效状态，后续调用直接返回失败
        return -1;    //按链路失败处理
      }
      //升级期间拔线时端口既不关闭也不报错，定期确认它是否还在系统里
      if(++probeTickWait >= 20)    //每次等待10毫秒，约200毫秒确认一次
      {
        probeTickWait = 0;    //重新计数
        bool present = false;    //端口是否仍在可用列表里
        const QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
        for(const QSerialPortInfo &info : ports)    //逐个比对端口名
        {
          if(info.portName().compare(portName, Qt::CaseInsensitive) == 0)
          {
            present = true;    //找到同名端口
            break;
          }
        }
        if(!present)    //系统里已经没有该端口，说明设备被移除
        {
          linkBroken = true;    //记下失效状态
          return -1;    //交给上层按链路失败处理
        }
      }
    }
  }
  return 0;    //超时未读到任何数据
}

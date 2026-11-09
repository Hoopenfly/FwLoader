#include "udp_transport.h"    //引入UDP传输类的声明

#include <QElapsedTimer>    //引入计时器，用于实现读超时
#include <QHostInfo>    //引入主机名解析，支持界面输入域名

#include <cstring>    //引入memcpy，把读到的字节拷入调用方缓冲区

/**
 * @brief   构造UDP传输
 * @param   host    目标IP或主机名
 * @param   port    目标端口
 * @param   server  true为服务器模式，false为客户端模式
 */
UdpTransport::UdpTransport(const QString &host, quint16 port, bool server)
  : host(host), address(host), lastPort(0), port(port), serverMode(server)    //保存目标文本、地址与端口，尚无来源记录
{
}

/**
 * @brief   析构时关闭套接字，释放端口
 */
UdpTransport::~UdpTransport()
{
  UdpTransport::close();    //析构时显式调用本类close，不走虚分派
}

/**
 * @brief   按模式绑定本地端口
 * @param   error  输出：失败原因
 * @return  true成功
 */
bool UdpTransport::open(QString &error)
{
  lastFrom = QHostAddress();    //清掉上一次会话记住的来源，避免重连后把首帧发往旧设备
  lastPort = 0;

  if(serverMode)    //服务器模式绑定固定端口等待设备上报
  {
    //服务器模式：绑定目标端口等待设备上报
    if(!socket.bind(QHostAddress(QHostAddress::AnyIPv4), port))    //在所有IPv4网卡的指定端口上绑定
    {
      error = socket.errorString();    //把绑定失败原因交给调用方
      return false;
    }
    return true;    //绑定成功即可开始收包
  }

  //客户端模式先确认目标地址可解析
  QHostAddress target(host);    //先尝试按IP字面量解析
  if(target.isNull())    //不是IP字面量时按主机名解析
  {
    const QHostInfo info = QHostInfo::fromName(host);    //同步解析主机名
    if(info.addresses().isEmpty())    //解析不到任何地址
    {
      error = QStringLiteral("无法解析目标地址：%1").arg(host);    //交给界面提示
      return false;    //地址无效时不绑定端口
    }
    target = info.addresses().first();    //取第一个解析结果
  }
  address = target;    //保存解析后的目标地址

  //客户端模式：绑定随机端口后发往目标
  if(!socket.bind(QHostAddress(QHostAddress::AnyIPv4), static_cast<quint16>(0)))    //端口填0由系统分配
  {
    error = socket.errorString();    //把绑定失败原因交给调用方
    return false;
  }
  return true;    //绑定成功即可开始发包
}

/**
 * @brief   关闭套接字，未绑定时不做处理
 */
void UdpTransport::close()
{
  if(socket.state() != QAbstractSocket::UnconnectedState)    //已绑定或已连接时才需要关闭
  {
    socket.close();    //关闭套接字并释放端口
  }
}

/**
 * @brief   发送一个UDP数据报
 * @param   data  数据
 * @param   len   字节数
 * @return  true表示数据报已整个交给系统
 */
bool UdpTransport::write(const uint8_t *data, std::size_t len)
{
  //服务器模式优先回复最近来源，否则发往配置地址
  const QHostAddress target = (serverMode && !lastFrom.isNull()) ? lastFrom : address;    //确定目标地址
  const quint16 targetPort = (serverMode && lastPort != 0) ? lastPort : port;    //确定目标端口

  if(target.isNull())    //服务器模式还没收到过设备数据报，没有可用的目标地址
  {
    return false;    //交给上层按发送失败上报，而不是让协议栈返回地址错误
  }

  const qint64 written = socket.writeDatagram(reinterpret_cast<const char *>(data),
                                              static_cast<qint64>(len), target, targetPort);    //一次写入即一个数据报
  if(written != static_cast<qint64>(len))    //写入长度不足说明发送失败
  {
    return false;
  }
  //等待数据报交给系统协议栈；与串口同理，只在仍有数据没发出去时才判定失败
  if(!socket.waitForBytesWritten(1000) && socket.bytesToWrite() > 0)
  {
    return false;    //套接字已失效，交给上层按链路故障处理
  }
  return true;
}

/**
 * @brief   在超时时间内接收一个数据报
 * @param   data       输出缓冲
 * @param   len        缓冲容量，超长数据报按此截断
 * @param   timeoutMs  超时时间
 * @return  实际读取字节数，0表示超时
 */
int UdpTransport::read(uint8_t *data, std::size_t len, int timeoutMs)
{
  if(len == 0)    //请求长度为0时直接返回
  {
    return 0;
  }
  if(socket.state() != QAbstractSocket::BoundState)    //套接字未绑定，链路不可用
  {
    return -1;    //与串口、TCP 保持一致：负值表示链路失败
  }

  if(timeoutMs <= 0)    //非阻塞模式：只取走内核队列里已有的数据报
  {
    if(!socket.hasPendingDatagrams())
    {
      return 0;    //没有数据即返回
    }
    //先查数据报大小：读进小缓冲会被静默截断，这里按超长包处理
    const qint64 quickSize = socket.pendingDatagramSize();
    if(quickSize > static_cast<qint64>(len))
    {
      QByteArray discard;    //把整包取出来丢掉，避免残留半个数据报影响后续解析
      discard.resize(static_cast<int>(quickSize));
      QHostAddress dropFrom;
      quint16 dropPort = 0;
      socket.readDatagram(discard.data(), discard.size(), &dropFrom, &dropPort);
      return 0;    //超长数据报不是本协议的应答，丢弃后按本次无数据处理
    }
    QByteArray quick;    //临时接收缓冲
    quick.resize(static_cast<int>(len));
    QHostAddress quickFrom;
    quint16 quickPort = 0;
    const qint64 got = socket.readDatagram(quick.data(), quick.size(), &quickFrom, &quickPort);
    if(got < 0)
    {
      return -1;    //读取出错按链路异常上报
    }
    if(got == 0)
    {
      return 0;    //空数据报视为没有数据
    }
    lastFrom = quickFrom;    //记住来源，服务器模式回复时使用
    lastPort = quickPort;
    std::memcpy(data, quick.constData(), static_cast<std::size_t>(got));
    return static_cast<int>(got);
  }

  QElapsedTimer timer;    //使用单调计时器统计已等待时间
  timer.start();    //开始计时
  while(timer.elapsed() < timeoutMs)    //在总超时时间内反复尝试
  {
    if(socket.hasPendingDatagrams())    //内核接收队列里已有数据报
    {
      //先查数据报大小：读进小缓冲会被静默截断，这里按超长包处理
      const qint64 pending = socket.pendingDatagramSize();
      QHostAddress from;    //输出：来源地址
      quint16 fromPort = 0;    //输出：来源端口
      if(pending > static_cast<qint64>(len))
      {
        QByteArray discard;    //把整包取出来丢掉，避免残留半个数据报影响后续解析
        discard.resize(static_cast<int>(pending));
        socket.readDatagram(discard.data(), discard.size(), &from, &fromPort);
        continue;    //超长数据报不是本协议的应答，丢弃后继续等待
      }
      QByteArray bytes;    //接收缓冲，按调用方容量分配
      bytes.resize(static_cast<int>(len));    //已确认整包放得下
      const qint64 count = socket.readDatagram(bytes.data(), bytes.size(), &from, &fromPort);    //取出一个数据报

      if(count > 0)    //读到有效数据报才处理
      {
        lastFrom = from;    //记住来源地址，服务器模式回复时使用
        lastPort = fromPort;    //记住来源端口
        std::memcpy(data, bytes.constData(), static_cast<std::size_t>(count));    //拷入调用方缓冲
        return static_cast<int>(count);
      }
      if(count < 0)    //读取出错按链路异常上报
      {
        return -1;
      }
    }
    socket.waitForReadyRead(10);    //最多等10毫秒新数据，避免空转占满CPU
  }
  return 0;    //超时未读到任何数据
}

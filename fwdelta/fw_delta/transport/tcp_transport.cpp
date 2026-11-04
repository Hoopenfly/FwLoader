#include "tcp_transport.h"    //引入TCP传输类的声明

#include <QElapsedTimer>    //引入计时器，用于实现读超时
#include <QHostAddress>    //引入主机地址类型，用于还原IPv4映射的IPv6地址
#include <QThread>    //引入线程休眠，服务器模式等待客户端时避免空转
#include <QVariantMap>    //引入键值映射，用于组装界面表格的一行

#include <cstring>    //引入memcpy，把读到的字节拷入调用方缓冲区

namespace
{

/**
 * @brief   取对端的IPv4地址文本
 * @param   socket  已连接的套接字
 * @return  IPv4地址，无法转换时返回原地址
 */
QString peerIp(const QTcpSocket *socket)
{
  QHostAddress address = socket->peerAddress();    //取对端地址，可能是IPv4或IPv6

  //IPv4客户端经双栈监听时会被表示成IPv4映射的IPv6地址，这里还原成IPv4
  if(address.protocol() == QAbstractSocket::IPv6Protocol)    //只处理IPv6表示形式
  {
    bool ok = false;    //记录转换为IPv4是否成功
    const quint32 ipv4 = address.toIPv4Address(&ok);    //尝试取出内嵌的IPv4地址
    if(ok)    //确实是IPv4映射地址时才替换
    {
      address = QHostAddress(ipv4);    //用还原出的IPv4重建地址对象
    }
  }
  return address.toString();    //返回可直接显示的地址文本
}

} // namespace

/**
 * @brief   构造TCP传输
 * @param   host     目标IP或主机名
 * @param   port     目标端口
 * @param   server   true为服务器模式，false为客户端模式
 * @param   onClient 服务器模式接受客户端连接时的回调
 */
TcpTransport::TcpTransport(const QString &host, quint16 port, bool server,
                           std::function<void(const QString &)> onClient)
  : active(nullptr), host(host), port(port), serverMode(server),    //初始没有当前连接，保存目标与模式
    clientCallback(std::move(onClient))    //接管客户端连接回调的所有权
{
}

/**
 * @brief   析构时关闭监听与全部连接，避免句柄泄漏
 */
TcpTransport::~TcpTransport()
{
  TcpTransport::close();    //析构时显式调用本类close，不走虚分派
}

/**
 * @brief   按模式建立监听或发起连接
 * @param   error  输出：失败原因
 * @return  true成功
 */
bool TcpTransport::open(QString &error)
{
  if(serverMode)    //服务器模式只监听，不等具体客户端
  {
    //服务器模式：监听成功即就绪，客户端连上后再收发
    if(!listener.listen(QHostAddress::Any, port))    //在所有网卡的指定端口上监听
    {
      error = listener.errorString();    //把监听失败原因交给调用方
      return false;
    }
    return true;    //监听已就绪
  }

  //客户端模式：主动连接目标
  clientSocket.connectToHost(host, port);    //发起异步连接
  if(!clientSocket.waitForConnected(3000))    //最多等3秒完成TCP握手
  {
    error = clientSocket.errorString();    //把连接失败原因交给调用方
    return false;
  }
  active = &clientSocket;    //连接建立后作为当前通信目标
  return true;
}

/**
 * @brief   关闭监听器与全部客户端连接
 */
void TcpTransport::close()
{
  QList<ClientInfo> snapshot;    //先取出待关闭的连接，缩短持锁时间
  {
    QMutexLocker locker(&m_mutex);    //与轮询、发送线程串行访问共享成员
    snapshot = clients;    //复制列表，避免关闭过程中遍历被修改
    clients.clear();    //列表立即置空，避免重复处理
    active = nullptr;    //当前连接随之失效
    while(listener.hasPendingConnections())    //先回收已完成握手但还没被接纳的连接
    {
      QTcpSocket *pending = listener.nextPendingConnection();
      if(pending != nullptr)    //指针有效时才处理
      {
        pending->disconnect();    //断开信号，避免关闭过程中回调
        pending->close();    //立即关闭套接字
        pending->deleteLater();    //交给事件循环回收
      }
    }
    listener.close();    //关闭监听器，停止接受新连接
    clientSocket.close();    //关闭客户端模式下使用的套接字
  }

  for(const ClientInfo &info : snapshot)    //逐个关闭服务器模式下已连接的客户端
  {
    if(info.socket == nullptr)    //已失效的记录直接跳过
    {
      continue;
    }
    info.socket->disconnect();    //断开本对象信号，避免关闭过程中回调removeClient
    info.socket->disconnectFromHost();    //先请求断开，让对端收到FIN
    info.socket->close();    //立即关闭套接字
    info.socket->deleteLater();    //交给事件循环回收，避免在信号回调里直接销毁
  }
}

/**
 * @brief   从客户端列表移除指定套接字并切换当前连接
 * @param   socket  已断开的套接字
 */
void TcpTransport::removeClient(QTcpSocket *socket)
{
  bool changed = false;    //记录本次是否真的移除了客户端
  {
    QMutexLocker locker(&m_mutex);    //与读取客户端列表的GUI线程互斥
    for(int i = 0; i < clients.size(); ++i)    //按套接字指针查找对应记录
    {
      if(clients.at(i).socket == socket)    //找到断开的那个客户端
      {
        clients.removeAt(i);    //从列表中摘除该记录
        changed = true;    //标记集合有变化
        break;    //列表内套接字唯一，找到即可结束查找
      }
    }

    if(active == socket)    //断开的是当前通信目标时需要改选
    {
      active = clients.isEmpty() ? nullptr : clients.first().socket;    //有客户端则切换，否则置空
    }
  }

  socket->deleteLater();    //释放已断开的套接字，避免句柄泄漏

  if(changed && clientCallback)    //集合有变化时通知上层刷新列表
  {
    clientCallback(QString());    //空字符串表示只是集合变化，不是新客户端上线
  }
}

/**
 * @brief   服务器模式下接受所有排队的新客户端
 */
void TcpTransport::acceptIfNeeded()
{
  if(!serverMode)    //客户端模式没有待接受的连接
  {
    return;
  }

  QList<ClientInfo> accepted;    //本轮新接入的客户端，用于锁外回调
  {
    QMutexLocker locker(&m_mutex);    //保护监听器与客户端列表
    //接受所有排队的新客户端，并记录上线时间
    while(listener.hasPendingConnections())    //把队列里的连接一次处理完
    {
      QTcpSocket *socket = listener.nextPendingConnection();    //取出一个已完成握手的连接
      if(socket == nullptr)    //取不到连接时结束本轮
      {
        break;
      }

      ClientInfo info;    //组装一条客户端记录
      info.socket = socket;    //记录套接字，后续收发与关闭都用它
      info.ip = peerIp(socket);    //记录对端IP
      info.port = socket->peerPort();    //记录对端端口
      info.time = QDateTime::currentDateTime();    //记录上线时间，供界面显示
      clients.append(info);    //加入客户端列表
      if(active == nullptr)    //第一个连上的客户端默认作为通信目标
      {
        active = socket;
      }

      QObject::connect(socket, &QTcpSocket::disconnected, socket, [this, socket]()    //断开时移除该客户端
      {
        removeClient(socket);    //从列表摘除并切换当前连接
      });

      accepted.append(info);    //记录下来，稍后在锁外通知上层
    }
  }

  if(clientCallback)    //有回调时通知上层有新客户端
  {
    for(const ClientInfo &info : accepted)
    {
      clientCallback(info.ip);    //参数为新客户端的IP
    }
  }
}

/**
 * @brief   轮询传输层状态，只处理服务器模式的连接接纳
 */
void TcpTransport::poll()
{
  acceptIfNeeded();    //接受排队的新客户端，其他情况无需处理
}

/**
 * @brief   获取已连接客户端标识列表
 * @return  形如IP:端口的字符串列表
 */
QStringList TcpTransport::clientList() const
{
  QMutexLocker locker(&m_mutex);    //与接受连接、移除客户端互斥
  QStringList list;    //保存拼接结果
  for(const ClientInfo &info : clients)    //逐个拼接已连接的客户端
  {
    list.append(QStringLiteral("%1:%2").arg(info.ip).arg(info.port));    //格式为IP:端口
  }
  return list;
}

/**
 * @brief   获取客户端明细列表，供界面表格显示
 * @return  每项包含序号、时间、IP、端口
 */
QVariantList TcpTransport::clientRows() const
{
  QMutexLocker locker(&m_mutex);    //与接受连接、移除客户端互斥
  QVariantList rows;    //每行一个客户端

  for(int i = 0; i < clients.size(); ++i)    //按连接顺序生成表格行
  {
    const ClientInfo &info = clients.at(i);    //当前客户端的记录
    QVariantMap row;    //一行数据
    row.insert(QStringLiteral("index"), i + 1);    //序号从1开始，便于对照界面
    row.insert(QStringLiteral("time"), info.time.toString(QStringLiteral("HH:mm:ss")));    //只显示时分秒
    row.insert(QStringLiteral("ip"), info.ip);    //对端IP
    row.insert(QStringLiteral("port"), info.port);    //对端端口
    rows.append(row);    //加入结果
  }
  return rows;
}

/**
 * @brief   选择当前使用的客户端
 * @param   index  客户端索引，负值表示取消当前目标
 */
void TcpTransport::setClient(int index)
{
  QMutexLocker locker(&m_mutex);    //与接受连接、移除客户端互斥
  if(index < 0)    //负下标表示取消当前通信目标
  {
    //负索引表示取消当前通信目标
    active = nullptr;    //停止向任何客户端发送数据
    return;
  }
  if(index < clients.size())    //下标越界时忽略，保持当前目标不变
  {
    active = clients.at(index).socket;    //切换到界面选中的客户端
  }
}

/**
 * @brief   向当前连接写入一段字节
 * @param   data  数据
 * @param   len   字节数
 * @return  true表示字节已全部写入
 */
bool TcpTransport::write(const uint8_t *data, std::size_t len)
{
  acceptIfNeeded();    //写之前先接纳排队的新客户端

  QTcpSocket *socket = nullptr;    //本次发送使用的连接
  {
    QMutexLocker locker(&m_mutex);    //读取当前连接指针
    socket = active;
  }
  if(socket == nullptr || socket->state() != QAbstractSocket::ConnectedState)    //没有可用连接时不发送
  {
    return false;
  }
  const qint64 written = socket->write(reinterpret_cast<const char *>(data),
                                       static_cast<qint64>(len));    //按Qt接口的长度类型写入原始字节
  if(written != static_cast<qint64>(len))    //写入长度不足说明发送失败
  {
    return false;
  }
  //等待数据交给系统协议栈；个别平台会误报超时，因此只在仍有数据没发出去时才判定失败
  if(!socket->waitForBytesWritten(1000) && socket->bytesToWrite() > 0)
  {
    return false;    //等待失败且发送缓冲仍未清空，按发送失败处理
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
int TcpTransport::read(uint8_t *data, std::size_t len, int timeoutMs)
{
  if(len == 0)    //请求长度为0时直接返回
  {
    return 0;
  }

  if(timeoutMs <= 0)    //非阻塞模式：只取走已经到达的数据
  {
    QTcpSocket *quick = nullptr;
    {
      QMutexLocker locker(&m_mutex);    //读取当前连接指针
      quick = active;
    }
    if(serverMode)    //服务器模式先接纳新连接
    {
      acceptIfNeeded();
      QMutexLocker locker(&m_mutex);
      quick = active;
    }
    if(quick != nullptr && quick->state() != QAbstractSocket::ConnectedState)    //连接已断开
    {
      return -1;    //交给上层按链路失败处理
    }
    if(quick == nullptr || quick->bytesAvailable() <= 0)
    {
      return 0;    //没有连接或没有数据即返回
    }
    const qint64 want = qMin<qint64>(quick->bytesAvailable(), static_cast<qint64>(len));
    const qint64 got = quick->read(reinterpret_cast<char *>(data), want);
    if(got < 0)    //读取出错：按链路异常上报，不能当成"暂时没数据"
    {
      return -1;
    }
    return static_cast<int>(got);
  }

  QElapsedTimer timer;    //使用单调计时器统计已等待时间
  timer.start();    //开始计时
  while(timer.elapsed() < timeoutMs)    //在总超时时间内反复尝试
  {
    QTcpSocket *socket = nullptr;    //本轮使用的连接
    {
      QMutexLocker locker(&m_mutex);    //读取当前连接指针
      socket = active;
    }

    if(serverMode)    //服务器模式可能还没有任何客户端
    {
      acceptIfNeeded();    //先处理已经完成握手的连接
      {
        QMutexLocker locker(&m_mutex);    //接纳后重新读取当前连接
        socket = active;
      }
      if(socket == nullptr)    //仍然没有客户端时短暂等待
      {
        QThread::msleep(10);    //避免空转占满CPU
        continue;    //没有连接可读，进入下一轮
      }
    }

    if(socket != nullptr && socket->state() != QAbstractSocket::ConnectedState)    //连接已断开
    {
      return -1;    //交给上层按链路失败处理
    }

    if(socket != nullptr && socket->bytesAvailable() > 0)    //当前连接已有可读数据
    {
      const QByteArray bytes = socket->read(static_cast<qint64>(len));    //最多读len字节，不阻塞
      const int count = static_cast<int>(bytes.size());    //本次实际读到的字节数

      if(count > 0)    //读到数据才拷贝并返回
      {
        std::memcpy(data, bytes.constData(), static_cast<std::size_t>(count));    //拷入调用方缓冲
        return count;
      }
    }
    if(socket != nullptr)    //连接仍有效时等待新数据到达
    {
      socket->waitForReadyRead(10);    //最多等10毫秒，避免空转占满CPU
    }
    else    //客户端模式下连接对象缺失时同样要让出CPU
    {
      QThread::msleep(10);    //避免紧循环空转
    }
  }
  return 0;    //超时未读到任何数据
}

#ifndef TCPTRANSPORT_H      //防止头文件被重复包含
#define TCPTRANSPORT_H      //定义头文件保护宏

#include <QMutex>        //互斥锁，保护客户端列表与当前连接指针
#include <QTcpServer>    //TCP监听器
#include <QTcpSocket>    //TCP套接字
#include <QDateTime>     //QDateTime，记录客户端上线时间
#include <QList>         //QList，保存客户端列表
#include <QString>       //QString，保存地址与标识
#include <QStringList>   //客户端标识列表
#include <QVariantList>  //客户端明细列表

#include <functional>    //std::function，客户端连接回调

#include "transport.h"   //传输接口定义

/**
 * @brief   服务器模式下单个客户端的信息
 */
struct ClientInfo
{
  QTcpSocket *socket = nullptr; //客户端套接字
  QString ip;                   //客户端IP
  quint16 port = 0;             //客户端端口
  QDateTime time;               //上线时间
};

/**
 * @brief   TCP传输实现
 * @note    客户端主动连接；服务器模式监听成功后即视为就绪，客户端连上后再收发。
 */
class TcpTransport : public IFirmwareTransport
{
public:
  /**
   * @brief   构造TCP传输
   * @param   host     目标IP或主机名
   * @param   port     目标端口
   * @param   server   true为服务器模式，false为客户端模式
   * @param   onClient 服务器模式接受客户端连接时的回调
   */
  TcpTransport(const QString &host, quint16 port, bool server = false,
               std::function<void(const QString &)> onClient = nullptr);

  ~TcpTransport() override;  //析构，关闭监听与所有连接

  /**
   * @brief   打开连接；服务器模式只启动监听
   * @param   error  输出：失败原因
   * @return  true成功
   */
  bool open(QString &error) override;

  /**
   * @brief   关闭监听与所有客户端连接
   */
  void close() override;

  /**
   * @brief   向当前连接写入一段字节
   * @param   data  数据
   * @param   len   字节数
   * @return  true成功
   */
  bool write(const uint8_t *data, std::size_t len) override;

  /**
   * @brief   从当前连接读取一段字节
   * @param   data       输出缓冲
   * @param   len        期望读取的最大字节数
   * @param   timeoutMs  超时时间
   * @return  实际读取字节数，0表示超时，<0表示失败
   */
  int read(uint8_t *data, std::size_t len, int timeoutMs) override;

  /**
   * @brief   轮询：服务器模式接受新客户端并清理已断开的连接
   */
  void poll() override;

  /**
   * @brief   获取已连接的客户端标识列表
   * @return  客户端标识列表，没有客户端时为空
   */
  QStringList clientList() const override;

  /**
   * @brief   获取客户端明细列表
   * @return  每项包含序号、时间、IP、端口
   */
  QVariantList clientRows() const override;

  /**
   * @brief   选择当前使用的客户端
   * @param   index  客户端索引
   */
  void setClient(int index) override;

private:
  void acceptIfNeeded();                 //接受新客户端并记录其信息
  void removeClient(QTcpSocket *socket); //移除已断开的客户端

  QTcpServer listener;              //服务器监听器
  QTcpSocket clientSocket;          //客户端套接字
  QTcpSocket *active;               //当前使用的连接
  QList<ClientInfo> clients;        //服务器模式下已连接的客户端
  QString host;                     //目标地址
  quint16 port;                     //目标端口
  bool serverMode;                  //是否服务器模式
  std::function<void(const QString &)> clientCallback; //客户端连接回调
  mutable QMutex m_mutex;              //保护clients/active，GUI线程与升级线程都会访问
};

#endif // TCPTRANSPORT_H

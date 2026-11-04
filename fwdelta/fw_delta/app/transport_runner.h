#ifndef TRANSPORT_RUNNER_H        //防止头文件被重复包含
#define TRANSPORT_RUNNER_H        //定义头文件保护宏

#include <QObject>        //对象基类，提供信号槽机制
#include <QByteArray>     //原始字节缓冲，承载设备主动发来的数据
#include <QStringList>    //客户端名称列表
#include <QVariantList>   //客户端明细行数据

#include <atomic>         //原子布尔指针，读取升级取消标志
#include <cstdint>        //uint32_t
#include <memory>         //std::unique_ptr

class IFirmwareTransport; //固件传输接口的前置声明
class QTimer;             //轮询定时器的前置声明

/**
 * @brief   在独立工作线程中持有并操作传输通道
 * @note    串口与socket的创建、打开、读写、轮询全部在本对象所属线程执行，
 *          GUI线程只投递排队请求，避免QIODevice跨线程访问。
 */
class TransportRunner : public QObject
{
  Q_OBJECT //启用Qt元对象系统，支持排队调用与跨线程信号

public:
  /**
   * @brief   构造传输执行器
   * @param   cancelFlag  升级取消标志，由GUI线程原子置位
   * @param   parent      父对象
   */
  explicit TransportRunner(std::atomic<bool> *cancelFlag, QObject *parent = nullptr);

  /**
   * @brief   析构，释放传输对象与轮询定时器
   */
  ~TransportRunner() override;

signals:
  void openFinished(bool ok, const QString &info, const QString &error);  //通道打开结果
  void closedFinished();                                                  //通道关闭完成
  void clientsChanged(const QStringList &names, const QVariantList &rows);//客户端列表变化
  void progressChanged(int percent);                                      //升级进度变化
  void framesChanged(uint32_t frameCurrent, uint32_t frameTotal);         //已发送帧号与总帧数
  void statusText(const QString &text);                                   //状态文字变化
  void logText(const QString &text);                                      //新增一行日志
  void flashFinished(bool ok, bool linkBroken, const QString &message);    //升级流程结束，linkBroken表示链路已断
  void rawBytes(const QByteArray &bytes);                                 //收到设备主动发来的原始字节
  void linkLost(const QString &reason);                                   //链路已断开，需要界面复位连接状态

public slots:
  /**
   * @brief   非阻塞读取一次串口或网络数据
   * @note    仅在非升级阶段调用，把设备主动打印的内容交给界面显示
   */
  void pollText();
  /**
   * @brief   打开串口或网络通道
   * @param   transportType 0串口、1TCP、2UDP
   * @param   portName      串口设备名，例如COM5
   * @param   baud          波特率
   * @param   dataBits      数据位
   * @param   stopBits      停止位
   * @param   parity        校验位
   * @param   flowControl   流控
   * @param   host          目标IP或主机名
   * @param   port          目标端口
   * @param   role          0客户端、1服务器
   */
  void openTransport(int transportType, const QString &portName, int baud, int dataBits,
                     int stopBits, int parity, int flowControl, const QString &host,
                     int port, int role);

  /**
   * @brief   关闭当前通道并释放传输对象
   */
  void closeTransport();

  /**
   * @brief   切换TCP服务器模式下发送的目标客户端
   * @param   index  客户端索引，-1表示取消选择
   */
  void selectClient(int index);

  /**
   * @brief   执行一次完整升级流程
   * @param   newFile  新固件本地路径
   * @param   fwdFile  差分包本地路径，可为空
   * @note    走差分还是整包由本函数内部按差分包可用性与差分率自行决定，
   *          差分未成功时自动回退整包，界面不再提供手动选择；
   *          版本与水印一律取自新固件自带的镜像元数据，不接受外部传入。
   */
  void runFlash(const QString &newFile, const QString &fwdFile);

private:
  void startPollTimer();    //启动客户端轮询
  void stopPollTimer();     //停止客户端轮询
  void pollOnce();          //轮询一次并刷新客户端列表
  void emitClients();       //取当前客户端列表并发出变化信号
  bool clientCancelled() const;  //读取升级取消标志

  std::atomic<bool> *m_cancelFlag = nullptr;   //取消标志，指向GUIS线程持有的原子变量
  std::unique_ptr<IFirmwareTransport> m_transport;  //当前传输通道，仅在本线程访问
  QTimer *m_pollTimer = nullptr;               //客户端轮询定时器，在本线程创建
  QStringList m_lastNames;                     //上一次发出的客户端名称列表
  QVariantList m_lastRows;                     //上一次发出的客户端明细
  int m_transportType = -1;                    //当前通道类型
  int m_role = 0;                              //当前通道角色，0客户端、1服务器
};

#endif // TRANSPORT_RUNNER_H

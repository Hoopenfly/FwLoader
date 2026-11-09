#include "fw_bridge.h"    //本类的声明与属性定义

#include <QClipboard>    //把日志复制到系统剪贴板
#include <QDateTime>    //为每条日志生成时间戳
#include <QFile>    //读写固件文件与日志文件
#include <QFileInfo>    //判断文件是否存在、取所在目录
#include <QGuiApplication>    //访问GUI剪贴板对象
#include <QNetworkInterface>    //枚举本机网卡地址
#include <QSerialPortInfo>    //枚举当前可用串口
#include <QStringList>    //承载端口列表与客户端列表
#include <QThread>    //传输通道独占的工作线程
#include <QTimer>    //非升级阶段的被动读取定时器
#include <QUrl>    //把file://形式的路径还原为本地路径
#include <QtConcurrent/QtConcurrentRun>    //把差分生成放到线程池执行

#include <vector>    //固件字节缓冲

#include "fw_delta_codec.h"    //make_delta差分打包算法
#include "fw_fwd.h"    //fwd包结构、读写与版本号编码
#include "fw_image.h"    //镜像元数据结构与槽位长度常量
#include "transport_runner.h"    //传输通道工作线程执行器

namespace    //匿名命名空间，内部工具函数仅本文件可见
{

/**
 * @brief   把设备状态码翻成中文说明
 * @param   status  设备返回的状态码
 * @return  对应的中文说明
 */
QString iapStatusText(uint8_t status)
{
  switch(status)    //逐项匹配设备端定义的状态码
  {
    case 0x00u: return QStringLiteral("成功");
    case 0x01u: return QStringLiteral("帧格式错误");
    case 0x02u: return QStringLiteral("尚未开始升级");
    case 0x03u: return QStringLiteral("镜像长度不符");
    case 0x04u: return QStringLiteral("写Flash失败");
    case 0x06u: return QStringLiteral("写镜像元数据失败");
    case 0x07u: return QStringLiteral("镜像无效");
    case 0x08u: return QStringLiteral("水印不符");
    case 0x09u: return QStringLiteral("偏移不连续");
    case 0x0Au: return QStringLiteral("版本过低，本次版本低于设备当前运行的版本");
    case 0x0Du: return QStringLiteral("代码CRC32不符");
    case 0x0Eu: return QStringLiteral("基版本不符，设备当前固件与差分包不匹配");
    case 0x0Fu: return QStringLiteral("差分记录流格式错误");
    case 0x10u: return QStringLiteral("当前构建不支持差分");
    default:    return QStringLiteral("未知状态");
  }
}

/**
 * @brief   读整个固件文件
 * @param   path  固件文件路径
 * @param   data  输出缓冲区，存放文件全部字节
 * @return  true读取成功且文件非空
 */
bool readBinFile(const QString &path, std::vector<uint8_t> &data)    //把整个固件文件读入内存
{
  QFile file(path);    //以给定路径构造文件对象

  if(!file.open(QIODevice::ReadOnly))    //只读方式打开失败时直接返回
  {
    return false;    //路径不存在或无读取权限
  }
  const QByteArray bytes = file.readAll();    //一次性读入全部字节，固件只有几十KB
  data.assign(bytes.begin(), bytes.end());    //复制到调用方的字节容器
  return !data.empty();    //空文件按读取失败处理
}

/**
 * @brief   把32位版本号格式化成 主.次.修订
 * @param   ver  版本号，高字节为主版本，最低字节固定为0
 * @return  形如"1.0.3"的字符串
 */
QString version_text(uint32_t ver)    //版本号转可读文本，供日志显示
{
  return QStringLiteral("%1.%2.%3")
      .arg((ver >> 24) & 0xFFu)    //主版本号
      .arg((ver >> 16) & 0xFFu)    //次版本号
      .arg((ver >> 8) & 0xFFu);    //修订号
}

} // namespace

/**
 * @brief   构造桥接对象，启动传输工作线程
 * @param   parent  父对象，QML引擎负责生命周期
 */
FwBridge::FwBridge(QObject *parent) : QObject(parent)    //初始化基类，其余成员按声明顺序默认构造
{
  //串口列表由界面刷新按钮手动触发，启动时不自动枚举
  //日志只发信号，统一在GUI线程追加，避免工作线程直接改日志成员
  connect(this, &FwBridge::logAdded, this, &FwBridge::appendLog, Qt::QueuedConnection);    //队列连接，跨线程安全

  //传输通道独占一个工作线程，串口与socket全部在该线程打开和读写
  m_thread = new QThread(this);    //父对象为本类，随本类一起销毁
  m_thread->setObjectName(QStringLiteral("FwTransport"));    //线程名便于调试时识别
  m_runner = new TransportRunner(&m_cancelRequested);    //取消标志由界面线程原子置位
  m_runner->moveToThread(m_thread);    //执行器及其轮询定时器整体迁移到工作线程
  connect(m_runner, &TransportRunner::openFinished, this, &FwBridge::onOpenFinished);    //通道打开结果
  connect(m_runner, &TransportRunner::closedFinished, this, &FwBridge::onClosedFinished);    //通道关闭完成
  connect(m_runner, &TransportRunner::clientsChanged, this, &FwBridge::onClientsChanged);    //客户端列表变化
  connect(m_runner, &TransportRunner::progressChanged, this, &FwBridge::onRunnerProgress);    //升级进度
  //帧号由传输线程上报，自动跨线程排队到GUI线程
  connect(m_runner, &TransportRunner::framesChanged, this, &FwBridge::onFramesChanged);
  connect(m_runner, &TransportRunner::statusText, this, &FwBridge::onRunnerStatus);    //状态文字
  connect(m_runner, &TransportRunner::logText, this, [this](const QString &text)    //日志行
  {
    log(text);    //统一加时间戳后追加到GUI线程日志
  });
  connect(m_runner, &TransportRunner::flashFinished, this, &FwBridge::onFlashFinished);    //升级结束
  connect(m_runner, &TransportRunner::flashPhaseChanged, this, &FwBridge::onFlashPhaseChanged);    //上传阶段
  //设备主动打印的内容由传输线程读出后交给GUI线程解析
  connect(m_runner, &TransportRunner::rawBytes, this, &FwBridge::onRawBytes);
  connect(m_runner, &TransportRunner::linkLost, this, &FwBridge::onLinkLost);
  connect(m_thread, &QThread::finished, m_runner, &QObject::deleteLater);    //线程结束后释放执行器
  m_thread->start();    //启动工作线程

  //被动读取定时器：通道打开后每200ms取一次设备打印，升级期间自动跳过
  m_listenTimer = new QTimer(this);
  m_listenTimer->setInterval(200);
  connect(m_listenTimer, &QTimer::timeout, this, &FwBridge::pollPassive);
}

/**
 * @brief   析构前停止传输线程，避免工作线程访问已销毁的成员
 */
FwBridge::~FwBridge()
{
  m_cancelRequested = true;    //先置取消标志，让升级流程在下一帧前退出
  /* 任务可能已提交到线程池但还没抢到线程，此时isRunning()为false，
     只按isRunning判断会跳过等待，任务随后仍可能在this销毁后执行。
     waitForFinished()对未开始的任务同样会等到结束，对无任务的future立即返回 */
  m_deltaFuture.waitForFinished();    //阻塞到线程收尾，防止悬空访问
  if(m_thread)    //传输线程存在时先停事件循环再等待
  {
    m_thread->quit();    //请求事件循环退出
    m_thread->requestInterruption();    //通知正在阻塞的收发循环尽快返回
    if(!m_thread->wait(30000))    //最多等待30秒，覆盖取消后末帧的收尾
    {
      /* 线程仍未退出：强杀线程会打断Qt的串口I/O，属于未定义行为，
         只在等待超时后作为最后手段使用，避免~QThread在运行时析构触发qFatal */
      m_thread->terminate();    //强制结束线程，避免析构时进程被qFatal带走
      m_thread->wait(2000);    //给终止后的线程留出收尾时间
    }
  }
}

/**
 * @brief   当前是否正在升级
 * @return  true表示后台发送线程正在工作
 */
bool FwBridge::busy() const
{
  return m_busy;    //界面据此禁用开始升级按钮
}

/**
 * @brief   差分生成任务是否正在进行
 * @return  true表示正在比对新旧固件
 */
bool FwBridge::deltaBusy() const
{
  return m_deltaBusy;    //界面据此禁用固件选择与差分包保存
}

/**
 * @brief   取状态栏文字
 * @return  最近一次业务状态描述
 */
QString FwBridge::status() const
{
  return m_status;    //由setStatus统一维护
}

/**
 * @brief   当前通道是否已打开
 * @return  true表示可发送数据
 */
bool FwBridge::connected() const
{
  return m_connected;    //由setConnected统一维护
}

/**
 * @brief   是否正在建立连接
 * @return  true表示打开请求已投递、结果尚未返回
 */
bool FwBridge::connecting() const
{
  return m_connecting;    //由setConnecting统一维护
}

//连接状态变化统一走这里，保证属性值与信号成对更新
void FwBridge::setConnected(bool connected)    //更新连接状态并通知界面
{
  if(m_connected != connected)    //仅在状态真正变化时处理，避免无谓刷新
  {
    m_connected = connected;    //记录新状态
    emit connectedChanged();    //通知QML绑定刷新
  }
}

//连接建立中状态变化统一走这里，界面据此屏蔽重复点击
void FwBridge::setConnecting(bool connecting)
{
  if(m_connecting != connecting)
  {
    m_connecting = connecting;
    emit connectingChanged();
  }
}

/**
 * @brief   取升级进度百分比
 * @return  0到100之间的整数
 */
int FwBridge::progress() const
{
  return m_progress;    //由进度回调逐帧更新
}

/**
 * @brief   取当前上传阶段
 * @return  0仍在发送、1已把数据发完在等设备处理
 */
int FwBridge::flashPhase() const
{
  return m_flashPhase;    //由传输线程的阶段回调更新
}

/**
 * @brief   取全部日志文本
 * @return  带时间戳的多行日志，供复制与导出；发送阶段末尾追加两行实时进度
 */
QString FwBridge::logText() const
{
  //发送阶段把帧号行与进度行拼在末尾，界面上就是日志的最后两行原地刷新
  if(!m_flashActive)
  {
    return m_logText;    //非发送阶段只返回历史日志
  }
  return m_logText + QLatin1String("<br>") + progressLines(true);    //追加带颜色标记的两行，历史日志本身用<br>分隔
}

/**
 * @brief   取日志正文长度，不含末尾实时进度行
 * @return  已累积的历史日志字符数
 */
int FwBridge::logBodyLength() const
{
  return m_logText.length();    //进度行是渲染时临时拼接的，不属于正文
}

/**
 * @brief   取日志纯文本，不含富文本标记
 * @return  可导出到文件的纯文本
 */
QString FwBridge::plainLogText() const
{
  if(!m_flashActive)
  {
    return m_plainLog;    //非发送阶段只返回历史日志
  }
  return m_plainLog + QLatin1Char('\n') + progressLines(false);    //追加不带标记的两行
}

/**
 * @brief   生成日志末尾的实时两行
 * @param   rich  true表示返回带颜色标记的富文本，false表示纯文本
 * @return  倒数第二行帧号、倒数第一行进度
 */
QString FwBridge::progressLines(bool rich) const
{
  const QString stamp = m_progressStamp.isEmpty()
      ? QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"))
      : m_progressStamp;    //固定为发送开始时刻，不随进度刷新变化
  const int barWidth = 40;    //进度条总格数
  const int filled = barWidth * qBound(0, m_progress, 100) / 100;    //已完成格数
  const QString barOn = QString(filled, QChar(0x2588));    //已完成部分
  const QString barOff = QString(barWidth - filled, QChar(0x2588));    //未完成部分用同一字形，只靠颜色区分

  if(!rich)    //导出用纯文本，只保留一行
  {
    return QStringLiteral("[%1] %2%3 %4%").arg(stamp, barOn, barOff).arg(m_progress);
  }

  //界面用富文本：时间戳沿用历史日志的默认颜色，只有进度条本体着色
  return QStringLiteral("[%1] <font color='#22C55E'>%2</font><font color='#E5E7EB'>%3</font> "
                        "<font color='#111827'>%4%</font>")
      .arg(stamp, barOn, barOff).arg(m_progress);
}

/**
 * @brief   取最近一次差分的差分率
 * @return  补丁大小占新固件的百分比
 */
double FwBridge::deltaPercent() const
{
  return m_deltaPercent;    //makeDelta成功时写入
}

/**
 * @brief   取上次参与差分的旧固件大小
 * @return  字节数
 */
qint64 FwBridge::oldSize() const
{
  return m_oldSize;    //makeDelta成功时写入
}

/**
 * @brief   取上次参与差分的新固件大小
 * @return  字节数
 */
qint64 FwBridge::newSize() const
{
  return m_newSize;    //makeDelta成功时写入
}

/**
 * @brief   取差分算法选用的策略名
 * @return  例如COPY、DIFF等策略文字
 */
QString FwBridge::strategy() const
{
  return m_strategy;    //来自打包统计结果
}

/**
 * @brief   取最近一次生成的差分包实际落盘路径
 * @return  差分包路径，尚未生成过时为空
 */
QString FwBridge::deltaPath() const
{
  return m_deltaPath;    //由postDeltaPath在GUI线程写入
}

/**
 * @brief   取当前通道的显示信息
 * @return  串口名或对端地址，未连接时为"未连接"
 */
QString FwBridge::deviceInfo() const
{
  return m_deviceInfo;    //由setDeviceInfo统一维护
}

/**
 * @brief   取当前选中的客户端信息
 * @return  客户端标识，未选中时为空
 */
QString FwBridge::clientInfo() const
{
  return m_clientInfo;    //由setClientInfo统一维护
}

/**
 * @brief   取当前可选的客户端名称列表
 * @return  未打开连接时返回空列表
 */
QStringList FwBridge::clientList() const
{
  return m_clientList;    //由传输线程推送的缓存，界面线程不直接访问传输层
}

/**
 * @brief   取客户端列表的行数据
 * @return  供表格模型绑定的QVariantList
 */
QVariantList FwBridge::clientRows() const
{
  return m_clientRows;    //由传输线程推送的缓存，未连接时为空行集
}

/**
 * @brief   取当前选中的客户端索引
 * @return  下拉框索引，-1表示未选中
 */
int FwBridge::clientIndex() const
{
  return m_clientIndex;    //由selectClient维护
}

/**
 * @brief   选择TCP服务器模式下的目标客户端
 * @param   index  客户端下拉索引，-1表示取消选择
 */
void FwBridge::selectClient(int index)    //切换后续发送的目标客户端
{
  if(index < 0)    //负索引表示取消通信目标，但保留界面的视觉高亮
  {
    m_clientIndex = -1;    //界面记录为未选中
    setClientInfo(QString());    //清空显示的客户端信息
    TransportRunner *runner = m_runner;
    QMetaObject::invokeMethod(runner, [runner]() { runner->selectClient(-1); }, Qt::QueuedConnection);
    return;    //不发出clientIndexChanged，空行点击后高亮由界面自己维持
  }
  if(index >= m_clientList.size())    //越界索引直接忽略，客户端可能刚断开
  {
    return;    //保持原选择不变
  }
  m_clientIndex = index;    //缓存选择结果
  setClientInfo(m_clientList.at(index));    //显示选中的客户端标识
  emit clientIndexChanged();    //通知界面刷新选中项
  TransportRunner *runner = m_runner;
  QMetaObject::invokeMethod(runner, [runner, index]() { runner->selectClient(index); }, Qt::QueuedConnection);
}

//客户端信息变化时统一走这里，避免界面重复刷新
void FwBridge::setClientInfo(const QString &info)    //更新显示的客户端信息
{
  if(m_clientInfo != info)    //内容变化时才发信号
  {
    m_clientInfo = info;    //记录新内容
    emit clientInfoChanged();    //通知界面刷新
  }
}

/**
 * @brief   取串口显示列表
 * @return  形如"COM5 (USB-SERIAL CH340)"的字符串列表
 */
QStringList FwBridge::portLabels() const
{
  return m_portLabels;    //与m_portNames按下标一一对应
}

/**
 * @brief   判断当前是否有可用串口
 * @return  串口列表非空返回true
 */
bool FwBridge::hasPorts() const
{
  return !m_portNames.isEmpty();    //列表为空表示没有可打开的串口
}

/**
 * @brief   取串口短名列表
 * @return  形如"COM5"的字符串列表，与portLabels按下标一一对应
 */
QStringList FwBridge::portNames() const
{
  return m_portNames;    //下拉框直接绑定该列表，无需在QML里拆分显示文本
}

/**
 * @brief   获取本机IPv4地址列表
 * @return  监听地址候选，首项为0.0.0.0
 */
QStringList FwBridge::localIps() const    //列出界面可选的监听地址
{
  QStringList ips;    //结果列表，先放通配地址
  ips.append(QStringLiteral("0.0.0.0"));    //监听所有网卡
  ips.append(QStringLiteral("127.0.0.1"));    //仅本机回环，便于本机调试

  const auto addresses = QNetworkInterface::allAddresses();    //枚举全部网卡地址
  for(const QHostAddress &address : addresses)    //逐个筛出可用的IPv4地址
  {
    if(address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback())    //排除IPv6与回环
    {
      const QString ip = address.toString();    //转成点分十进制字符串
      if(!ips.contains(ip))    //同一地址可能出现在多张网卡上
      {
        ips.append(ip);    //去重后加入候选
      }
    }
  }
  return ips;    //界面下拉框直接使用该列表
}

/**
 * @brief   刷新串口列表
 */
void FwBridge::refreshPorts()    //重新枚举串口并重建两个平行列表
{
  m_portNames.clear();    //先清空旧名称，避免残留已拔出的串口
  m_portLabels.clear();    //清空对应的显示文本

  const auto ports = QSerialPortInfo::availablePorts();    //取系统当前可用串口
  for(const QSerialPortInfo &info : ports)    //逐个生成下拉项
  {
    QString label = info.portName();    //默认显示端口名，例如COM5
    if(!info.description().isEmpty())    //有描述时补上，便于识别具体设备
    {
      label += QStringLiteral(" (%1)").arg(info.description());    //形如COM5 (USB-SERIAL CH340)
    }
    m_portNames.append(info.portName());    //保存原始端口名，供打开串口使用
    m_portLabels.append(label);    //保存给人看的显示文本
  }
  emit portsChanged();    //通知界面重建下拉列表
}

/**
 * @brief   打开串口或网络连接
 * @param   transportType 0串口、1TCP、2UDP
 * @param   portIndex     串口下拉索引
 * @param   baud          波特率
 * @param   dataBits      数据位
 * @param   stopBits      停止位
 * @param   parity        校验位
 * @param   flowControl   流控
 * @param   host          目标地址
 * @param   port          目标端口
 * @param   role          0客户端、1服务器
 */
void FwBridge::openConnection(int transportType, int portIndex, int baud,
                              int dataBits, int stopBits, int parity,
                              int flowControl, const QString &host,
                              int port, int role)    //把打开请求投递给传输线程
{
  if(m_busy)    //升级线程正持有传输对象，切换连接会使其悬空
  {
    log(QStringLiteral("升级进行中，无法切换连接"));    //提示先等升级结束
    return;    //拒绝重建通道
  }
  if(m_connecting)    //上一次连接还在建立中，忽略重复点击
  {
    return;    //避免排队多个打开请求
  }
  if(m_connected || m_transportType != -1)    //已有通道时先释放旧连接
  {
    closeConnection();    //关闭请求排在本次打开之前执行
  }

  m_transportType = transportType;    //记录通道类型，升级结束后据此恢复轮询
  m_role = role;    //记录角色，0客户端、1服务器
  if(role == 1 && transportType != 0)    //服务器模式监听地址可能随网卡变化
  {
    emit localIpsChanged();    //让界面在下拉框绑定上重新取本机地址
  }
  setConnecting(true);    //界面进入连接中，期间禁止重复打开与刷新
  setStatus(QStringLiteral("正在连接..."));    //界面先提示连接中

  const QString serialName = portName(portIndex);    //串口名在GUI线程取，避免工作线程读界面列表
  TransportRunner *runner = m_runner;
  QMetaObject::invokeMethod(runner, [runner, transportType, serialName, baud, dataBits,
                                     stopBits, parity, flowControl, host, port, role]()
  {
    runner->openTransport(transportType, serialName, baud, dataBits, stopBits,
                          parity, flowControl, host, port, role);    //在工作线程创建并打开通道
  }, Qt::QueuedConnection);
}

/**
 * @brief   关闭当前连接
 */
void FwBridge::closeConnection()    //复位界面状态并把关闭请求投递给传输线程
{
  if(m_busy)    //升级线程正持有传输对象，先取消再关闭，避免对象悬空
  {
    m_closeAfterCancel = true;    //记下待关闭标志，等升级线程退出后补做
    log(QStringLiteral("正在取消升级，结束后自动关闭通道"));
    cancelFlash();    //请求中断当前发送
    return;
  }
  if(!m_connected && !m_connecting && m_transportType == -1)    //没有通道时不重复处理
  {
    return;
  }

  setConnected(false);    //界面上报未连接
  setConnecting(false);    //连接中状态一并清除
  setStatus(QStringLiteral("未连接"));    //状态文字复位
  setDeviceInfo(QStringLiteral("未连接"));    //设备描述复位
  setClientInfo(QString());    //清空客户端信息
  m_clientList.clear();    //客户端缓存同步清空
  m_clientRows.clear();
  if(m_clientIndex != -1)    //列表已清空，选中项一并复位
  {
    m_clientIndex = -1;    //取消选中
    emit clientIndexChanged();    //通知界面刷新选中行
  }
  m_transportType = -1;    //通道已释放，类型复位
  m_role = 0;    //角色复位
  emit clientListChanged();    //通知界面清空客户端列表

  TransportRunner *runner = m_runner;
  QMetaObject::invokeMethod(runner, [runner]() { runner->closeTransport(); }, Qt::QueuedConnection);
}

/**
 * @brief   按索引取串口名
 * @param   index  下拉框索引
 * @return  串口名，越界时返回空字符串
 */
QString FwBridge::portName(int index) const    //把界面索引换算成串口名
{
  return m_portNames.value(index);    //越界返回空串，由传输层报打开失败
}

//把界面给的路径转成本地文件系统路径，兼容拖拽得到的file://形式
QString FwBridge::localPath(const QString &path)    //必要时做URL还原
{
  if(path.startsWith(QStringLiteral("file:")))    //QML拖拽传入的是URL而非路径
  {
    return QUrl(path).toLocalFile();    //还原成Windows本地路径
  }
  return path;    //普通路径原样返回
}

//忙碌状态变化统一走这里，保证属性值与信号成对更新
void FwBridge::setBusy(bool busy)    //更新忙碌状态并通知界面
{
  if(m_busy != busy)    //仅在状态真正变化时处理
  {
    m_busy = busy;    //记录新状态
    emit busyChanged();    //通知界面启停按钮
  }
}

//差分生成状态变化统一走这里，保证属性值与信号成对更新
void FwBridge::setDeltaBusy(bool busy)    //更新差分生成状态并通知界面
{
  if(m_deltaBusy != busy)    //仅在状态真正变化时处理
  {
    m_deltaBusy = busy;    //记录新状态
    emit deltaBusyChanged();    //通知界面禁用或恢复固件选择
  }
}

//状态文字变化统一走这里，避免重复刷新界面
void FwBridge::setStatus(const QString &status)    //更新状态栏文字
{
  if(m_status != status)    //内容变化时才发信号
  {
    m_status = status;    //记录新文字
    emit statusChanged();    //通知界面刷新
  }
}

//升级进度统一走这里，避免重复刷新整条进度条
void FwBridge::setProgress(int percent)    //更新升级进度并通知界面
{
  if(m_progress != percent)    //数值变化时才发信号
  {
    m_progress = percent;    //记录新进度
    emit progressChanged(percent);    //通知界面刷新进度条
  }
}

//日志统一加时间戳后只发信号，由GUI线程落盘到成员文本
/**
 * @brief   追加一条上位机操作信息
 * @param   text  日志内容，可含换行
 * @note    时间戳之后按灰色显示，与设备上行打印区分。
 */
void FwBridge::log(const QString &text)
{
  emitLog(kLogHost, text);    //归类为上位机信息
}

/**
 * @brief   追加一条设备上行打印
 * @param   text  日志内容，可含换行
 */
void FwBridge::logDevice(const QString &text)
{
  emitLog(kLogDevice, text);    //归类为设备打印
}

/**
 * @brief   追加一条成功信息
 * @param   text  日志内容，可含换行
 */
void FwBridge::logSuccess(const QString &text)
{
  emitLog(kLogSuccess, text);    //归类为成功结果
}

/**
 * @brief   追加一条失败信息
 * @param   text  日志内容，可含换行
 */
void FwBridge::logError(const QString &text)
{
  emitLog(kLogError, text);    //归类为错误
}

/**
 * @brief   给日志加上时间戳后投递到GUI线程
 * @param   kind  日志来源分类
 * @param   text  日志内容，可含换行
 * @note    可在任意线程调用；多行内容共用同一个时间戳。
 */
void FwBridge::emitLog(int kind, const QString &text)
{
  //时间戳精确到毫秒，便于对齐设备应答与重试时机
  const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
  emit logAdded(kind, stamp, text);    //队列连接投递到GUI线程
}

//定时向传输线程索取被动数据，升级期间不读取以免抢数据
void FwBridge::pollPassive()
{
  if(!m_connected || m_busy || !m_runner)    //未连接、升级中、执行器缺失时跳过
  {
    return;
  }
  QMetaObject::invokeMethod(m_runner, "pollText", Qt::QueuedConnection);    //让工作线程读一次
}

//把接收到的原始字节拆成协议帧与文本
void FwBridge::onRawBytes(const QByteArray &bytes)
{
  m_rxBuffer.append(bytes);    //先累积到接收缓冲

  while(!m_rxBuffer.isEmpty())
  {
    const int idx = m_rxBuffer.indexOf(QByteArray("\x55\xAA", 2));    //查找协议帧头

    if(idx < 0)    //没有帧头说明全是文本
    {
      flushText(m_rxBuffer);
      m_rxBuffer.clear();
      break;
    }
    if(idx > 0)    //帧头之前的字节是设备打印
    {
      flushText(m_rxBuffer.left(idx));
      m_rxBuffer.remove(0, idx);
      continue;
    }
    if(m_rxBuffer.size() < 11)    //固定头还没收全
    {
      break;
    }

    const int len = static_cast<uint8_t>(m_rxBuffer[7])
                  | (static_cast<uint8_t>(m_rxBuffer[8]) << 8);    //载荷长度
    const int total = 11 + len;    //整帧长度

    if(total > 1024)    //长度异常，说明是文本里碰巧出现的字节，丢掉帧头继续找
    {
      flushText(m_rxBuffer.left(1));
      m_rxBuffer.remove(0, 1);
      continue;
    }
    if(m_rxBuffer.size() < total)    //整帧还没收全
    {
      break;
    }

    const QByteArray frame = m_rxBuffer.left(total);    //取出一整帧
    //CRC16覆盖帧头到载荷；不符说明是设备打印里凑巧出现的55AA，整段按文本处理
    const uint16_t crcCalc = fw::crc16(reinterpret_cast<const uint8_t *>(frame.constData()),
                                       static_cast<std::size_t>(9 + len));
    const uint16_t crcGot = static_cast<uint8_t>(frame[9 + len])
                          | (static_cast<uint8_t>(frame[9 + len + 1]) << 8);
    m_rxBuffer.remove(0, total);
    if(crcCalc != crcGot)
    {
      flushText(frame);    //校验失败不丢字节，交给文本通道输出
      continue;
    }

    if((static_cast<uint8_t>(frame[2]) & 0x0Fu) == 0x02u && len == 1)    //是设备响应帧
    {
      const uint8_t status = static_cast<uint8_t>(frame[9]);    //状态码
      if(status != 0u)    //失败才提示，成功静默
      {
        log(QStringLiteral("设备回复 0x%1：%2")
            .arg(QString::number(status, 16).toUpper().rightJustified(2, QChar('0')), iapStatusText(status)));
      }
    }
  }
}

//把一段原始字节按行输出为设备打印文本
void FwBridge::flushText(const QByteArray &raw)
{
  constexpr int kTextBufLimit = 8192;    //未成行缓冲上限，防止设备持续输出无换行数据把内存撑大

  //按原始字节累积，整行收齐后再解码：UTF-8多字节字符被拆到两次回调里也不会碎
  for(const char c : raw)    //逐字节检查
  {
    const int code = static_cast<uint8_t>(c);    //按无符号数判断取值范围
    //保留制表符、换行与可打印字节；串口打开瞬间常收到一串0x00噪声，其余控制字符丢弃
    if(code == 0x09 || code == 0x0A || code == 0x0D || code >= 0x20)
    {
      m_textBuffer.append(c);
    }
  }

  if(m_textBuffer.size() > kTextBufLimit)    //一直没等到换行，不能无限累积
  {
    if(m_textBuffer.indexOf('\n') < 0)    //整段都没有换行，直接作为一行输出
    {
      QString whole = QString::fromUtf8(m_textBuffer);    //整体解码
      whole.remove(QChar(0xFFFD));    //去掉解码失败的替换字符，避免行首出现乱码
      if(!whole.trimmed().isEmpty())
      {
        logDevice(whole);    //设备打印按深色显示
      }
      m_textBuffer.clear();
      return;
    }
  }

  int pos = -1;
  while((pos = m_textBuffer.indexOf('\n')) >= 0)    //逐行切分
  {
    QString line = QString::fromUtf8(m_textBuffer.left(pos));    //行尾才按UTF-8解码
    m_textBuffer.remove(0, pos + 1);    //从缓冲移除
    if(line.endsWith(QLatin1Char('\r')))    //去掉行尾回车
    {
      line.chop(1);
    }
    line.remove(QChar(0xFFFD));    //去掉解码失败的替换字符，避免行首出现乱码
    if(!line.trimmed().isEmpty())    //空行不输出
    {
      logDevice(line);    //设备打印按深色显示，与上位机提示区分
    }
  }
}

/**
 * @brief   把一段日志内容转成富文本安全形式
 * @param   text       原文
 * @param   keepSpace  为true时把空格转成不换行空格，保留设备打印原有的空格个数
 * @return  转义后的文本
 */
static QString escapeHtml(const QString &text, bool keepSpace)
{
  QString out = text;    //在副本上替换，不改调用方数据
  out.replace(QLatin1Char('&'), QStringLiteral("&amp;"));    //&必须先转义
  out.replace(QLatin1Char('<'), QStringLiteral("&lt;"));     //避免被当成标签
  out.replace(QLatin1Char('>'), QStringLiteral("&gt;"));     //避免被当成标签
  if(keepSpace)    //富文本会把连续空格折叠成一个，设备打印需要原样保留
  {
    out.replace(QLatin1Char(' '), QStringLiteral("&nbsp;"));    //转成不换行空格
  }
  return out;    //返回可直接拼进富文本的内容
}

/**
 * @brief   取日志来源对应的文字标签
 * @param   kind  日志来源分类
 * @return  两字标签，导出到日志文件后仍能区分来源
 */
static QString logTag(int kind)
{
  switch(kind)
  {
    case kLogDevice:
      return QStringLiteral("设备");    //设备上行打印
    case kLogSuccess:
      return QStringLiteral("成功");    //成功结果
    case kLogError:
      return QStringLiteral("失败");    //失败与错误
    default:
      return QStringLiteral("系统");    //上位机自身的操作与状态
  }
}

/**
 * @brief   取日志来源对应的内容颜色
 * @param   kind  日志来源分类
 * @return  十六进制颜色串
 */
QString FwBridge::logColor(int kind)
{
  switch(kind)
  {
    case kLogDevice:
      return QStringLiteral("#111827");    //设备打印用深色，与上位机提示拉开对比
    case kLogSuccess:
      return QStringLiteral("#16A34A");    //升级或生成成功用绿色
    case kLogError:
      return QStringLiteral("#DC2626");    //失败与错误用红色
    default:
      return QStringLiteral("#6B7280");    //上位机自身的操作与状态用灰色
  }
}

/**
 * @brief   把一条日志追加进富文本与纯文本缓冲
 * @param   kind   日志来源分类
 * @param   stamp  时间戳文本
 * @param   text   日志内容，可含换行
 * @note    时间戳不着色，只有其后的内容按来源取色。
 */
void FwBridge::appendLog(int kind, const QString &stamp, const QString &text)    //仅由logAdded信号触发
{
  const QString color = logColor(kind);    //本条日志的内容颜色
  const QString tag = logTag(kind);        //本条日志的来源标签
  const QStringList lines = text.split(QLatin1Char('\n'));    //逐行渲染，每行都带同一个时间戳
  QString segment;    //本次新增的富文本片段，供界面按增量追加

  for(int i = 0; i < lines.size(); ++i)    //逐行处理
  {
    const QString line = QStringLiteral("[%1] <span style=\"color:%2\">[%3] %4</span>")
                         .arg(stamp, color, tag, escapeHtml(lines.at(i), kind == kLogDevice));    //时间戳保持原色，标签与内容同色
    if(!m_logText.isEmpty())    //已有内容时先补分隔
    {
      m_logText += QStringLiteral("<br>");    //富文本里的换行
      m_plainLog += QLatin1Char('\n');        //纯文本里的换行
      segment += QStringLiteral("<br>");      //增量片段同样需要分隔符
    }
    m_logText += line;    //拼进全文
    segment += line;      //拼进增量片段
    m_plainLog += QStringLiteral("[%1] [%2] %3").arg(stamp, tag, lines.at(i));    //导出用的纯文本也带标签
    ++m_logCount;    //累计行数，用于控制总量
  }

  bool trimmed = false;    //本次是否发生了裁剪
  if(m_logCount > 2000)    //超过上限时裁掉最老的一批，避免长时间运行后界面越来越慢
  {
    trimLog();    //按行裁剪
    trimmed = true;    //裁剪后尾部增量失效，必须让界面整段重设
  }
  if(trimmed)    //裁剪后全文已变，界面只能整段重绘
  {
    emit logTextChanged();    //通知界面重设全文
  }
  else    //常规追加走增量信号，避免每帧整段复制富文本
  {
    emit logSegmentAppended(segment);    //界面按片段追加
    emit logTextChanged();    //其它消费者仍可依赖全文变化信号
  }
}

//日志超过上限时丢掉最老的一部分，保持两份文本仍然逐行对应
void FwBridge::trimLog()
{
  //富文本里的分隔符固定是<br>，纯文本里固定是换行，两者行数一一对应
  QStringList htmlLines = m_logText.split(QStringLiteral("<br>"));
  QStringList plainLines = m_plainLog.split(QLatin1Char('\n'));
  const int drop = qMax(1, htmlLines.size() / 5);    //一次丢掉两成，避免频繁重建
  m_logText = htmlLines.mid(drop).join(QStringLiteral("<br>"));
  m_plainLog = plainLines.mid(qMin(drop, plainLines.size())).join(QLatin1Char('\n'));
  m_logCount = htmlLines.size() - drop;    //同步剩余行数
}

//从工作线程投递状态文字，统一在GUI线程写属性并发信号
void FwBridge::postStatus(const QString &status)    //可在任意线程调用
{
  QMetaObject::invokeMethod(this, [this, status]() { setStatus(status); }, Qt::QueuedConnection);
}

//从工作线程投递升级进度，统一在GUI线程写属性并刷新界面
void FwBridge::postProgress(int percent)    //可在任意线程调用
{
  QMetaObject::invokeMethod(this, [this, percent]()
  {
    m_progress = percent;    //只在GUI线程写成员
    emit progressChanged(percent);    //通知界面刷新进度条
  }, Qt::QueuedConnection);
}

//从工作线程投递差分统计，四个属性一次性在GUI线程更新
void FwBridge::postDeltaStats(double percent, qint64 oldSize, qint64 newSize,
                              const QString &strategy)    //可在任意线程调用
{
  QMetaObject::invokeMethod(this, [this, percent, oldSize, newSize, strategy]()
  {
    m_deltaPercent = percent;    //差分率
    m_oldSize = oldSize;    //旧固件字节数
    m_newSize = newSize;    //新固件字节数
    m_strategy = strategy;    //所选策略
    emit deltaStatsChanged();    //一次性刷新四个统计属性
  }, Qt::QueuedConnection);
}

//从工作线程投递差分包实际路径，界面据此回显差分包输入框
void FwBridge::postDeltaPath(const QString &path)    //可在任意线程调用
{
  QMetaObject::invokeMethod(this, [this, path]()
  {
    m_deltaPath = path;    //只在GUI线程写成员
    emit deltaPathChanged();    //通知界面刷新路径
  }, Qt::QueuedConnection);
}

//通道显示信息变化统一走这里，避免重复刷新界面
void FwBridge::setDeviceInfo(const QString &info)    //更新通道描述文字
{
  if(m_deviceInfo != info)    //内容变化时才发信号
  {
    m_deviceInfo = info;    //记录新描述
    emit deviceInfoChanged();    //通知界面刷新
  }
}

/**
 * @brief   把日志写入文件
 * @param   path  目标文件
 * @param   text  日志内容
 * @return  true写入成功
 */
bool FwBridge::saveLog(const QString &path, const QString &text)    //把日志文本导出到文件
{
  const QString filePath = localPath(path);    //兼容file://形式的目标路径
  QFile file(filePath);    //目标文件

  if(!file.open(QIODevice::WriteOnly | QIODevice::Text))    //创建或截断目标文件
  {
    log(QStringLiteral("导出日志失败: %1").arg(file.errorString()));    //让操作者看到失败原因
    return false;    //无写权限或目录不存在
  }
  if(file.write(text.toUtf8()) < 0)    //写入失败通常意味着磁盘已满或文件被占用
  {
    log(QStringLiteral("写入日志失败: %1").arg(file.errorString()));
    return false;
  }
  log(QStringLiteral("日志已导出: %1").arg(filePath));    //成功后给一条可见反馈
  return true;    //文件句柄由析构关闭
}

/**
 * @brief   把全部日志复制到剪贴板
 */
void FwBridge::copyLog()    //便于把失败现场直接贴给别人
{
  QGuiApplication::clipboard()->setText(plainLogText());    //复制纯文本，避免带富文本标记
}

/**
 * @brief   清空日志
 */
void FwBridge::clearLog()    //只清文本，不影响后续追加
{
  m_logText.clear();    //丢弃全部历史日志
  m_plainLog.clear();   //纯文本同步清空
  m_logCount = 0;    //计数一并归零，否则下一条日志立刻触发裁剪把新内容裁掉
  emit logTextChanged();    //界面文本同步清空
}

/**
 * @brief   提交差分生成任务
 * @param   oldPath  旧固件
 * @param   newPath  新固件
 * @param   outPath  输出的.fwd
 * @param   major    主版本
 * @param   minor    次版本
 * @param   patch    构建号
 * @param   project  项目水印
 * @return  true表示任务已提交，false表示已有任务在跑
 * @note    比对在后台线程执行，界面在此期间保持响应
 */
/**
 * @brief   解析固件自带镜像元数据并把结果写进日志
 * @param   path  固件文件路径
 * @return  命中时返回"版本|水印"，没有镜像元数据或内容损坏时返回空串
 */
QString FwBridge::inspectFirmware(const QString &path)    //选完文件即解析，结果直接进日志
{
  const QString file = localPath(path);    //兼容拖拽进来的file://路径
  if(file.isEmpty())    //没有选中文件
  {
    return QString();    //不做任何提示
  }

  fw::ImageMeta meta;    //解析结果
  const int rc = fw::inspect_meta_file(QFile::encodeName(file).toStdString(), meta);    //只读文件末尾64字节
  if(rc < 0)    //元数据存在但损坏，或文件打不开
  {
    log(QStringLiteral("该固件的镜像元数据不完整，文件可能被截断或不是发布包"));
    return QString();    //界面不回填，烧录时还会再拦一次
  }
  if(rc == 0)    //纯代码bin没有可用的版本与水印
  {
    log(QStringLiteral("该文件不含镜像元数据，请选择发布包（Converter.pkg.bin）"));
    return QString();    //视为无效固件
  }

  log(QStringLiteral("已解析镜像元数据：版本 %1，水印 %2")
      .arg(version_text(meta.image_ver)).arg(meta.project_id));    //把结果显示给操作者确认
  return version_text(meta.image_ver) + QStringLiteral("|") + QString::number(meta.project_id);    //命中
}

bool FwBridge::makeDelta(const QString &oldPath, const QString &newPath,
                         const QString &outPath)    //提交差分生成，立即返回
{
  if(m_deltaBusy)    //已有任务在跑，忽略重复点击
  {
    log(QStringLiteral("差分包正在生成，请稍候"));    //提示不要重复提交
    return false;    //不重复启动线程
  }
  if(m_busy)    //升级期间不允许生成差分，避免占用通道与CPU
  {
    log(QStringLiteral("升级进行中，无法生成差分"));    //提示先等升级结束
    return false;    //不启动线程
  }

  setDeltaBusy(true);    //界面进入差分生成态
  m_deltaFuture = QtConcurrent::run([this, oldPath, newPath, outPath]()    //按值捕获参数
  {
    doMakeDelta(oldPath, newPath, outPath);    //在工作线程里完成比对与写盘
    QMetaObject::invokeMethod(this, [this]() { setDeltaBusy(false); }, Qt::QueuedConnection);    //回GUI线程复位生成态
  });
  return true;    //任务已提交
}

/**
 * @brief   在后台线程比对并生成差分包
 * @param   oldPath  旧固件
 * @param   newPath  新固件
 * @param   outPath  输出的.fwd
 * @param   major    主版本
 * @param   minor    次版本
 * @param   patch    构建号
 * @param   project  项目水印
 * @return  true生成成功并已写盘
 */
bool FwBridge::doMakeDelta(const QString &oldPath, const QString &newPath,
                           const QString &outPath)    //新旧固件比对后写出fwd差分包
{
  const QString oldFile = localPath(oldPath);    //旧固件本地路径
  const QString newFile = localPath(newPath);    //新固件本地路径
  QString outFile = localPath(outPath);    //差分包输出路径，可为空

  if(oldFile.isEmpty() || newFile.isEmpty())    //两个固件都必须选择
  {
    log(QStringLiteral("请先选择旧固件和新固件"));    //提示缺失输入
    postStatus(QStringLiteral("差分生成失败"));    //界面状态置失败
    return false;    //不产生任何文件
  }
  if(outFile.isEmpty())    //未指定输出时用默认文件名
  {
    outFile = QFileInfo(newFile).absolutePath() + QStringLiteral("/app.fwd");    //放在新固件同目录
    log(QStringLiteral("未指定差分包，写入 %1").arg(outFile));    //告知实际输出位置
  }

  std::vector<uint8_t> oldCode;    //旧固件原始字节
  std::vector<uint8_t> newCode;    //新固件原始字节
  if(!readBinFile(oldFile, oldCode))    //旧固件读取失败则终止
  {
    log(QStringLiteral("读取旧固件失败: %1").arg(oldFile));    //记录失败路径
    postStatus(QStringLiteral("差分生成失败"));    //状态置失败
    return false;    //无法继续比对
  }
  if(!readBinFile(newFile, newCode))    //新固件读取失败则终止
  {
    log(QStringLiteral("读取新固件失败: %1").arg(newFile));    //记录失败路径
    postStatus(QStringLiteral("差分生成失败"));    //状态置失败
    return false;    //无法继续比对
  }

  //两个固件末尾若自带镜像元数据则先剥离，保证比对的是纯代码区
  fw::ImageMeta oldEmbedded;    //旧固件自带的镜像元数据
  fw::ImageMeta newEmbedded;    //新固件自带的镜像元数据
  if(fw::strip_embedded_meta(oldCode, oldEmbedded) < 0)    //旧固件头存在但不完整
  {
    log(QStringLiteral("旧固件末尾的镜像元数据不完整，文件可能被截断"));
    postStatus(QStringLiteral("差分生成失败"));
    return false;
  }
  const int newStrip = fw::strip_embedded_meta(newCode, newEmbedded);    //新固件是否自带镜像元数据
  if(newStrip < 0)    //新固件元数据存在但不完整
  {
    log(QStringLiteral("该固件的镜像元数据不完整，文件可能被截断或不是发布包"));
    postStatus(QStringLiteral("差分生成失败"));
    return false;
  }
  if(newStrip == 0)    //纯代码bin没有镜像元数据，也就没有版本与水印来源
  {
    log(QStringLiteral("新固件不含镜像元数据，请选择发布包（Converter.pkg.bin）"));
    postStatus(QStringLiteral("差分生成失败"));
    return false;
  }

  fw::ImageMeta meta = newEmbedded;    //差分包内嵌的镜像元数据沿用新固件自带的那一份

  fw::DeltaOptions opt;    //差分算法参数，取默认值
  fw::DeltaPack pack = fw::make_delta(oldCode, newCode, meta, opt);    //生成补丁载荷与统计
  if(pack.payload.empty())    //空载荷说明比对没有产出
  {
    log(QStringLiteral("差分生成失败"));    //记录算法失败
    postStatus(QStringLiteral("差分生成失败"));    //状态置失败
    return false;    //不写文件
  }

  if(!fw::write_fwd(QFile::encodeName(outFile).toStdString(), pack))    //按本地编码打开输出文件
  {
    log(QStringLiteral("写入差分包失败: %1").arg(outFile));    //写盘失败多为权限或磁盘问题
    postStatus(QStringLiteral("差分生成失败"));    //状态置失败
    return false;    //文件未生成
  }

  const double rate = static_cast<double>(pack.payload.size()) * 100.0
                    / static_cast<double>(newCode.size());    //补丁占新固件的百分比
  log(QStringLiteral("新固件: 版本 %1，水印 %2")
      .arg(version_text(meta.image_ver)).arg(meta.project_id));    //版本与水印取自新固件自带镜像元数据
  log(QStringLiteral("旧代码: %1 字节  CRC32 0x%2")
      .arg(oldCode.size()).arg(QString::number(pack.base_crc32, 16).toUpper().rightJustified(8, QChar('0'))));    //旧固件的基准校验值
  log(QStringLiteral("新代码: %1 字节  CRC32 0x%2")
      .arg(newCode.size()).arg(QString::number(pack.meta.code_crc32, 16).toUpper().rightJustified(8, QChar('0'))));    //新固件校验值
  log(QStringLiteral("策略: %1").arg(QString::fromStdString(pack.stats.strategy)));    //算法选用的比对策略
  log(QStringLiteral("差分包: %1 字节 (差分率 %2%)")
      .arg(pack.payload.size()).arg(rate, 0, 'f', 2));    //补丁大小与差分率，保留两位小数
  log(QStringLiteral("记录: COPY %1 条 / LITERAL %2 条 / DIFF %3 条")
      .arg(pack.stats.copy_ops).arg(pack.stats.literal_ops).arg(pack.stats.diff_ops));    //三类操作条数
  log(QStringLiteral("已写入: %1").arg(outFile));    //确认落盘位置
  postDeltaPath(outFile);    //把实际落盘路径回报界面，差分包输入框据此回显

  postDeltaStats(rate, static_cast<qint64>(oldCode.size()),
                 static_cast<qint64>(newCode.size()),
                 QString::fromStdString(pack.stats.strategy));    //回GUI线程统一刷新统计属性
  postStatus(QStringLiteral("差分生成成功 (%1%)").arg(rate, 0, 'f', 2));    //状态里带差分率
  return true;    //文件已成功写出
}

/**
 * @brief   开始烧录
 * @param   newPath  新固件
 * @param   fwdPath  差分包，可为空
 */
void FwBridge::startFlash(const QString &newPath, const QString &fwdPath)    //校验前置条件后把升级请求投递给传输线程
{
  if(m_busy)    //已有任务在跑，忽略重复点击
  {
    log(QStringLiteral("上一次升级仍在进行，请稍候"));    //给出可见反馈，避免点击像没反应
    return;    //避免两条请求同时写同一个通道
  }
  if(m_deltaBusy)    //差分生成期间不允许开烧，差分包可能还未写完
  {
    log(QStringLiteral("差分包正在生成，无法开始烧录"));    //提示等待
    return;    //不进入发送流程
  }

  if(!m_connected)    //没有通道无法发送
  {
    log(QStringLiteral("请先打开通道"));    //提示先建立连接
    postStatus(QStringLiteral("升级失败"));    //状态置失败
    return;    //不进入发送流程
  }
  if(!m_runner)    //工作线程未就绪时拒绝
  {
    log(QStringLiteral("传输线程尚未就绪，请稍后重试"));    //给出可见反馈
    return;
  }
  if(newPath.isEmpty())    //没有目标固件就没有可写入的镜像
  {
    log(QStringLiteral("请先选择新固件"));    //带差分包时必须明确它对应哪份新固件
    postStatus(QStringLiteral("升级失败"));
    return;
  }
  if(QDateTime::currentMSecsSinceEpoch() < m_deviceBusyUntilMs)    //上一轮的设备可能还在写Flash
  {
    //设备重建期间不读串口，这时发新帧只会堆在缓冲里，还可能被当成上一轮的残留
    log(QStringLiteral("设备仍在处理上一轮升级，请先复位设备再开始新的烧录"));
    postStatus(QStringLiteral("设备处理中，请先复位"));
    return;
  }

  setBusy(true);    //界面进入忙碌态
  m_progress = 0;    //进度从0开始
  emit progressChanged(0);    //让界面立即回到0，而不是沿用上次的进度
  m_flashPhase = 0;    //新一轮从发送阶段开始
  emit flashPhaseChanged();
  m_cancelRequested = false;    //清除上一次的取消标记
  const QString newFile = localPath(newPath);    //QML拖拽路径在GUI线程还原
  const QString fwdFile = localPath(fwdPath);    //差分包路径同样还原

  TransportRunner *runner = m_runner;
  QMetaObject::invokeMethod(runner, [runner, newFile, fwdFile]()
  {
    runner->runFlash(newFile, fwdFile);    //在工作线程完成读取、选包与发送
  }, Qt::QueuedConnection);
}

/**
 * @brief   请求取消正在进行的升级
 */
void FwBridge::cancelFlash()    //置标志，发送线程在下一帧前退出
{
  m_cancelRequested = true;    //原子写，工作线程每帧读取一次
}

/**
 * @brief   清空已生成的差分包路径
 * @note    清空后开始烧录不再携带差分包，改走整包发送。
 */
void FwBridge::clearDeltaPath()
{
	if(m_deltaPath.isEmpty())    //没有路径时无需通知界面
	{
		return;
	}

	postDeltaPath(QString());    //清空路径并通知界面刷新
}

void FwBridge::onOpenFinished(bool ok, const QString &info, const QString &error)    //传输线程打开完成
{
  setConnecting(false);    //连接流程结束
  if(!ok)    //失败时复位界面
  {
    log(QStringLiteral("打开连接失败: %1").arg(error));    //记录失败原因
    setConnected(false);    //回到未连接
    setStatus(QStringLiteral("连接失败"));    //状态文字
    setDeviceInfo(QStringLiteral("未连接"));    //设备描述
    m_transportType = -1;    //复位通道类型
    m_role = 0;    //复位角色
    return;
  }
  setConnected(true);    //通道已就绪
  setStatus(info);    //状态文字由传输线程给出
  setDeviceInfo(info);    //设备描述与状态一致

  if(m_listenTimer)    //通道打开后开始被动读取设备打印
  {
    m_listenTimer->start();
  }
}

void FwBridge::onClosedFinished()    //传输线程关闭完成
{
  if(m_connected || m_connecting)    //关闭完成回调晚到时，不能把刚建立的新连接状态清掉
  {
    return;
  }
  if(m_listenTimer)    //通道关闭后停止被动读取
  {
    m_listenTimer->stop();
  }
  m_rxBuffer.clear();    //丢弃残留的接收缓冲
  m_textBuffer.clear();  //丢弃未成行的文本
  setConnected(false);    //确保界面处于未连接
  setConnecting(false);    //关闭流程结束
  setStatus(QStringLiteral("未连接"));    //状态文字复位
  setDeviceInfo(QStringLiteral("未连接"));    //设备描述复位
  setClientInfo(QString());    //清空客户端信息
  m_clientList.clear();    //清空客户端描述缓存
  m_clientRows.clear();    //清空表格行缓存
  m_clientIndex = -1;    //关闭通道后不选中任何行，避免空列表第0行被高亮
  emit clientIndexChanged();    //通知界面刷新选中行
  emit clientListChanged();    //通知界面清空客户端列表
}

void FwBridge::onLinkLost(const QString &reason)    //传输线程发现链路已断开
{
  if(!m_connected)    //已经处理过或本就未连接
  {
    return;
  }
  log(QStringLiteral("通道已断开：%1，请重新打开通道").arg(reason));    //告诉操作者发生了什么
  if(m_listenTimer)    //停止被动读取，避免继续轮询已失效的句柄
  {
    m_listenTimer->stop();
  }
  m_rxBuffer.clear();    //丢弃残留接收缓冲
  m_textBuffer.clear();  //丢弃未成行的文本
  setConnected(false);   //复位界面连接状态
  setStatus(QStringLiteral("通道已断开"));
  setDeviceInfo(QStringLiteral("未连接"));
  //链路断开后客户端表已失效，不清会让界面一直显示已经不存在的连接
  setClientInfo(QString());    //清空客户端信息
  m_clientList.clear();    //清空客户端描述缓存
  m_clientRows.clear();    //清空表格行缓存
  if(m_clientIndex != -1)    //仅在仍有选中时复位，避免多余信号
  {
    m_clientIndex = -1;    //取消选中
    emit clientIndexChanged();    //通知界面取消高亮
  }
  emit clientListChanged();    //通知界面清空客户端列表
}

void FwBridge::onClientsChanged(const QStringList &infos, const QVariantList &rows)    //传输线程上报客户端列表
{
  m_clientList = infos;    //缓存最新的客户端描述
  m_clientRows = rows;    //缓存表格行
  if(m_clientList.isEmpty())    //没有客户端时不应保留选中项
  {
    if(m_clientIndex != -1)    //仅在仍有选中时复位
    {
      m_clientIndex = -1;    //清空选中
      emit clientIndexChanged();    //通知界面取消高亮
    }
  }
  else if(m_clientIndex >= m_clientList.size() || m_clientIndex < 0)    //选中项已不存在或未选中
  {
    /* 不自动改选第一项：列表变化后原索引已经指向另一台设备，
       自动选中会让下一次烧录打到别的板子上，改为取消选中等操作者重新指定 */
    if(m_clientIndex != -1)
    {
      m_clientIndex = -1;    //取消选中
      emit clientIndexChanged();    //通知界面取消高亮
    }
  }
  emit clientListChanged();    //通知界面刷新列表
}

void FwBridge::onRunnerProgress(int percent)    //传输线程上报升级进度
{
  setProgress(percent);    //写属性并发信号
  if(!m_flashActive)    //首次进度上报表示进入发送阶段
  {
    m_flashActive = true;    //打开日志末尾的进度行
    m_progressStamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));    //记下起始时刻，整段发送期间不再变化
  }
  emit progressLineChanged();    //只让界面替换末尾进度行，避免每帧整段重排
}

void FwBridge::onFramesChanged(uint32_t frameCurrent, uint32_t frameTotal)    //传输线程上报帧号
{
  //帧号当前只作记录（进度行显示的是百分比），刷新统一由 onRunnerProgress 触发，
  //避免同一帧触发两次整段富文本重排
  m_frameCurrent = frameCurrent;    //记录当前帧
  m_frameTotal = frameTotal;        //记录总帧数
}

void FwBridge::postFrames(uint32_t frameCurrent, uint32_t frameTotal)    //可在任意线程调用
{
  QMetaObject::invokeMethod(this, [this, frameCurrent, frameTotal]()
  {
    onFramesChanged(frameCurrent, frameTotal);    //回到GUI线程写成员
  }, Qt::QueuedConnection);
}

void FwBridge::onRunnerStatus(const QString &text)    //传输线程上报状态文字
{
  setStatus(text);    //写属性并发信号
}

void FwBridge::onFlashFinished(bool ok, bool linkBroken, bool stoppedWaiting,
                               const QString &message)    //传输线程升级结束
{
  setBusy(false);    //界面退出忙碌态
  if(ok)    //成功时进度补满
  {
    setProgress(100);
  }

  if(!linkBroken)    //正常结束时才用传输线程给的状态文字覆盖
  {
    setStatus(message);    //最终状态由传输线程给出
  }

  if(stoppedWaiting)    //设备已收完数据在自行重建，这段时间内不能发起新一轮升级
  {
    //差分包只有一帧时，数据早已全部到达，设备仍在解码并逐页写Flash，
    //此时再发START_DELTA只会堆在串口缓冲里，不如等它结束并复位
    m_deviceBusyUntilMs = QDateTime::currentMSecsSinceEpoch() + 60000;    //按最长重建时间留一分钟
    setStatus(QStringLiteral("设备处理中，请勿断电，约 1 分钟后复位确认版本"));
  }

  if(m_flashActive)    //发送阶段结束，日志末尾不再显示进度行
  {
    m_flashActive = false;
    m_progressStamp.clear();    //下一轮发送重新取起始时刻
    emit logTextChanged();    //让界面回到纯历史日志
  }

  if(linkBroken)    //链路已断，复位界面连接状态并提示重新打开通道
  {
    onLinkLost(QStringLiteral("升级过程中传输中断"));
  }

  if(m_closeAfterCancel)    //升级期间点过关闭，这里补做真正的关闭
  {
    m_closeAfterCancel = false;
    closeConnection();    //此时m_busy已复位，可以正常释放通道
  }
}

/**
 * @brief   接收传输线程上报的上传阶段
 * @param   phase 0发送中、1等设备处理
 */
void FwBridge::onFlashPhaseChanged(int phase)
{
  if(m_flashPhase == phase)    //只在真正切换时通知界面，避免多余重绘
  {
    return;
  }
  m_flashPhase = phase;    //记录当前阶段
  emit flashPhaseChanged();    //界面据此把按钮文字在取消烧录与停止等待之间切换
}

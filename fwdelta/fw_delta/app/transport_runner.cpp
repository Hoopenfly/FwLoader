#include "transport_runner.h"

#include <QFile>        //读取固件与差分包
#include <QFileInfo>    //判断文件是否存在
#include <QTimer>       //客户端轮询定时器

#include <cstddef>      //size_t
#include <cstdint>      //UINT32_MAX
#include <vector>       //固件字节缓冲

#include "fw_fwd.h"     //fwd包结构、读写与版本号编码
#include "fw_image.h"   //镜像元数据长度常量
#include "transport/firmware_uploader.h"    //分帧发送与设备应答处理
#include "transport/serial_transport.h"     //串口通道实现
#include "transport/tcp_transport.h"        //TCP客户端与服务器通道实现
#include "transport/udp_transport.h"        //UDP通道实现

namespace    //匿名命名空间，内部工具函数仅本文件可见
{

/**
 * @brief   读整个固件文件
 * @param   path  固件文件路径
 * @param   data  输出缓冲区，存放文件全部字节
 * @return  true读取成功且文件非空
 */
bool readBinFile(const QString &path, std::vector<uint8_t> &data)
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
 * @brief   把设备返回的IAP状态码翻译成中文说明
 * @param   status  设备在RESP帧里回的状态码
 * @return  中文说明，未定义的状态码返回"未知状态"
 * @note    状态码定义见library/inc/fw_iap.h，0x0A表示版本低于设备当前运行镜像的版本
 */
QString iapStatusText(uint8_t status)
{
  switch(status)
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
 * @brief   把32位版本号拆成"主.次.修订"
 * @param   ver  打包后的版本号
 * @return  形如"1.0.3"的字符串
 */
QString versionText(uint32_t ver)
{
  return QStringLiteral("%1.%2.%3").arg(static_cast<uint>((ver >> 24) & 0xFFu))    //最高字节为主版本
                                   .arg(static_cast<uint>((ver >> 16) & 0xFFu))    //次高字节为次版本
                                   .arg(static_cast<uint>((ver >> 8) & 0xFFu));    //第三字节为修订号
}

} // namespace

/**
 * @brief   构造传输执行器，创建客户端轮询定时器
 * @param   cancelFlag  升级取消标志，由GUI线程原子置位
 * @param   parent      父对象
 */
TransportRunner::TransportRunner(std::atomic<bool> *cancelFlag, QObject *parent)
  : QObject(parent), m_cancelFlag(cancelFlag)
{
  m_pollTimer = new QTimer(this);    //定时器作为子对象，随本对象迁移到工作线程
  m_pollTimer->setInterval(300);    //每300毫秒轮询一次，及时感知客户端接入与断开
  connect(m_pollTimer, &QTimer::timeout, this, [this]() { pollOnce(); });
}

/**
 * @brief   析构，释放传输对象与轮询定时器
 */
TransportRunner::~TransportRunner()
{
  stopPollTimer();    //停止轮询

  if(m_transport)    //通道仍在时先关闭
  {
    m_transport->close();    //释放socket或串口句柄
    m_transport.reset();    //销毁对象
  }
}

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
void TransportRunner::openTransport(int transportType, const QString &portName, int baud,
                                    int dataBits, int stopBits, int parity, int flowControl,
                                    const QString &host, int port, int role)
{
  stopPollTimer();    //先停旧连接的轮询

  if(m_transport)    //同一时刻只保留一条通道
  {
    m_transport->close();    //关闭旧socket或串口
    m_transport.reset();    //销毁旧对象
  }

  m_transportType = transportType;    //记录通道类型
  m_role = role;    //记录角色，0客户端、1服务器

  if(transportType == 1)    //TCP：角色决定监听还是主动连接
  {
    m_transport.reset(new TcpTransport(host, static_cast<quint16>(port), role == 1,
                                       [this](const QString &clientInfo)
                                       {
                                         //客户端接入回调发生在工作线程，直接记录并刷新列表
                                         emit logText(QStringLiteral("客户端已连接: %1").arg(clientInfo));
                                         emitClients();
                                       }));
  }
  else if(transportType == 2)    //UDP：按角色创建套接字
  {
    m_transport.reset(new UdpTransport(host, static_cast<quint16>(port), role == 1));
  }
  else    //其余情况按串口处理
  {
    m_transport.reset(new SerialTransport(portName, baud, dataBits, stopBits, parity, flowControl));
  }

  QString error;    //存放打开失败的原因
  if(!m_transport->open(error))
  {
    m_transport.reset();    //释放半开的通道对象
    m_transportType = -1;    //复位通道类型
    m_role = 0;    //复位角色
    emit openFinished(false, QString(), error);    //失败原因放在第三个参数，由界面统一记一行日志
    return;
  }

  emitClients();    //连接成功后先刷新一次客户端列表

  QString info;    //通道描述文字，界面状态栏显示
  if(transportType == 1 && role == 1)    //TCP服务器只监听，客户端可能稍后才连
  {
    info = QStringLiteral("0.0.0.0:%1 监听中").arg(port);
    emit logText(QStringLiteral("TCP 服务器已打开: 0.0.0.0:%1 (监听中)").arg(port));
    startPollTimer();    //启动轮询，等待客户端接入
  }
  else if(transportType == 0)    //串口
  {
    info = portName + QStringLiteral(" 已连接");
    emit logText(QStringLiteral("串口已打开: %1").arg(portName));
  }
  else    //TCP客户端或UDP
  {
    const QString type = (transportType == 1) ? QStringLiteral("TCP") : QStringLiteral("UDP");
    const QString roleText = (role == 1) ? QStringLiteral("服务器") : QStringLiteral("客户端");
    info = host + QStringLiteral(":") + QString::number(port) + QStringLiteral(" 已连接");
    emit logText(QStringLiteral("%1 %2 已打开: %3:%4").arg(type, roleText, host).arg(port));
  }

  emit openFinished(true, info, QString());    //成功时描述放在第二个参数
}

/**
 * @brief   关闭当前通道并释放传输对象
 */
void TransportRunner::closeTransport()
{
  stopPollTimer();    //先停轮询，避免访问即将释放的传输对象

  if(m_transport)
  {
    m_transport->close();    //关闭socket或串口句柄
    m_transport.reset();    //销毁对象，后续调用走未连接分支
  }

  m_transportType = -1;    //通道已释放，类型复位
  m_role = 0;    //角色复位
  emitClients();    //通知界面清空客户端列表
  emit closedFinished();
}

/**
 * @brief   切换TCP服务器模式下发送的目标客户端
 * @param   index  客户端索引，-1表示取消选择
 */
void TransportRunner::selectClient(int index)
{
  if(m_transport)    //通道存在时切换目标
  {
    m_transport->setClient(index);
  }
}

/**
 * @brief   启动客户端轮询
 */
void TransportRunner::startPollTimer()
{
  if(m_pollTimer && !m_pollTimer->isActive())
  {
    m_pollTimer->start();
  }
}

/**
 * @brief   停止客户端轮询
 */
void TransportRunner::stopPollTimer()
{
  if(m_pollTimer && m_pollTimer->isActive())
  {
    m_pollTimer->stop();
  }
}

/**
 * @brief   轮询一次并刷新客户端列表
 */
void TransportRunner::pollOnce()
{
  if(!m_transport)
  {
    return;
  }
  m_transport->poll();    //accept新客户端并清理已断开的客户端
  emitClients();    //有变化时通知界面
}

/**
 * @brief   非阻塞读取一次串口或网络数据
 * @note    升级过程中不读取，避免与固件分帧收发抢数据
 */
void TransportRunner::pollText()
{
  if(!m_transport)    //通道未建立时直接返回
  {
    return;
  }

  QByteArray got;    //本次读到的全部字节
  uint8_t buf[256];  //单次读取缓冲
  for(int round = 0; round < 64; ++round)    //设迭代上限，避免数据源持续有数据时无法返回
  {
    const int n = m_transport->read(buf, sizeof(buf), 0);    //超时0即不等待
    if(n < 0)    //负值表示链路已失效，与“暂时没数据”区分开
    {
      emit linkLost(QStringLiteral("串口设备已从系统中移除"));    //通知界面复位连接状态
      break;    //不再继续读
    }
    if(n == 0)    //没有更多数据
    {
      break;
    }
    got.append(reinterpret_cast<const char *>(buf), n);    //累积到本次结果
  }

  if(!got.isEmpty())    //有数据才通知界面
  {
    emit rawBytes(got);
  }
}

/**
 * @brief   取当前客户端列表，与上次不同时发出信号
 */
void TransportRunner::emitClients()
{
  const QStringList names = m_transport ? m_transport->clientList() : QStringList();
  const QVariantList rows = m_transport ? m_transport->clientRows() : QVariantList();

  if(names == m_lastNames && rows == m_lastRows)    //内容没变化时不刷新界面
  {
    return;
  }
  m_lastNames = names;
  m_lastRows = rows;
  emit clientsChanged(names, rows);
}

/**
 * @brief   读取升级取消标志
 * @return  true表示界面已请求取消
 */
bool TransportRunner::clientCancelled() const
{
  return m_cancelFlag ? m_cancelFlag->load() : false;
}

/**
 * @brief   执行一次完整升级流程
 * @param   newFile  新固件本地路径
 * @param   fwdFile  差分包本地路径，可为空
 * @note    发送方式由本函数内部决定：差分包可用且差分率够低时走差分，
 *          否则或差分失败时回退整包；决定结果在发送前写进日志。
 *          版本与水印一律取自新固件自带的镜像元数据。
 */
void TransportRunner::runFlash(const QString &newFile, const QString &fwdFile)
{
  stopPollTimer();    //升级期间暂停客户端轮询，避免与发送争用socket

  uint32_t version = 0;    //版本号，由新固件的镜像元数据给出
  uint32_t project = 0;    //项目水印，同样由镜像元数据给出

  const auto postLog = [this](const QString &text) { emit logText(text); };
  const auto postStatus = [this](const QString &text) { emit statusText(text); };
  const auto postProgress = [this](int percent) { emit progressChanged(percent); };

  //统一收尾：恢复客户端轮询并通知GUI结束忙碌态
  const auto finish = [this](bool ok, const QString &message, bool linkBroken = false)
  {
    if(m_transport && m_transportType == 1 && m_role == 1)    //TCP服务器仍在监听时恢复轮询
    {
      startPollTimer();
    }
    emit flashFinished(ok, linkBroken, message);
  };

  if(!m_transport)    //通道可能已被界面关闭
  {
    postLog(QStringLiteral("请先打开连接"));
    postStatus(QStringLiteral("升级失败"));
    finish(false, QStringLiteral("未连接"));
    return;
  }

  if(newFile.isEmpty())    //没有新固件就无法发送
  {
    postLog(QStringLiteral("请先选择新固件"));
    postStatus(QStringLiteral("升级失败"));
    finish(false, QStringLiteral("未选择新固件"));
    return;
  }
  if(!QFileInfo::exists(newFile))    //文件被移动或删除
  {
    postLog(QStringLiteral("新固件不存在: %1").arg(newFile));
    postStatus(QStringLiteral("升级失败"));
    finish(false, QStringLiteral("新固件不存在"));
    return;
  }

  std::vector<uint8_t> code;    //整包固件字节
  if(!readBinFile(newFile, code))
  {
    postLog(QStringLiteral("读取新固件失败: %1").arg(newFile));
    postStatus(QStringLiteral("升级失败"));
    finish(false, QStringLiteral("读取新固件失败"));
    return;
  }
  //文件末尾若自带64字节镜像元数据则就地剥离，避免把元数据当成代码烧进设备
  fw::ImageMeta embedded;    //文件自带的镜像元数据
  const int stripResult = fw::strip_embedded_meta(code, embedded);
  if(stripResult < 0)    //魔数匹配但内容不一致，说明文件被截断
  {
    postLog(QStringLiteral("该固件的镜像元数据不完整，文件可能被截断或不是发布包"));
    postStatus(QStringLiteral("升级失败"));
    finish(false, QStringLiteral("镜像元数据损坏"));
    return;
  }
  if(stripResult == 0)    //纯代码bin没有版本与水印来源，不允许继续
  {
    postLog(QStringLiteral("该文件不含镜像元数据，请选择发布包（Converter.pkg.bin）"));
    postStatus(QStringLiteral("升级失败"));
    finish(false, QStringLiteral("固件缺少镜像元数据"));
    return;
  }

  //命中镜像元数据，本次只发送代码区，版本与水印全部以元数据为准
  postLog(QStringLiteral("检测到自带镜像元数据：版本 %1，水印 %2，已剥离，代码 %3 字节")
          .arg(versionText(embedded.image_ver), QString::number(embedded.project_id),
               QString::number(code.size())));
  version = embedded.image_ver;    //版本只认固件里的值
  project = embedded.project_id;   //水印只认固件里的值

  //镜像总长要能放进协议的32位偏移字段；具体能收多少由设备端槽位决定
  if(code.size() > static_cast<std::size_t>(UINT32_MAX - fw::kImgMetaLen))
  {
    postLog(QStringLiteral("固件长度超出协议可表示范围"));
    postStatus(QStringLiteral("升级失败"));
    finish(false, QStringLiteral("固件长度超出协议可表示范围"));
    return;
  }

  fw::DeltaPack pack;    //读入的差分包内容
  bool useDelta = false;    //本次是否走差分发送
  if(!fwdFile.isEmpty())    //填了差分包路径就尝试差分，是否真正采用还看后续校验
  {
    if(!QFileInfo::exists(fwdFile))    //路径失效时退回整包，不阻断升级
    {
      postLog(QStringLiteral("差分包不存在，改发整包: %1").arg(fwdFile));
    }
    else
    {
      useDelta = fw::read_fwd(QFile::encodeName(fwdFile).toStdString(), pack);
      if(!useDelta)    //文件损坏或不是本工具生成的差分包
      {
        postLog(QStringLiteral("差分包读取失败，改发整包"));
      }
      else if(static_cast<uint64_t>(pack.payload.size()) * 100u
              > static_cast<uint64_t>(code.size()) * 60u)    //差分率超60%时整包更省时
      {
        postLog(QStringLiteral("差分率超过 60%，改发整包"));
        useDelta = false;
      }
    }
  }

  if(useDelta && (pack.meta.image_len < fw::kImgMetaLen
                  || static_cast<uint64_t>(pack.meta.image_len) - fw::kImgMetaLen != code.size()
                  || pack.meta.image_ver != version
                  || pack.meta.project_id != project))    //长度相同但版本或水印不同的差分包同样不能用
  {
    postLog(QStringLiteral("差分包与新固件不匹配"));    //避免设备校验失败后才发现
    postStatus(QStringLiteral("升级失败"));
    finish(false, QStringLiteral("差分包与新固件不匹配"));
    return;
  }

  //发送前把本次采用的方式明确写给操作者，避免不清楚发的是哪一种
  if(useDelta)
  {
    const double rate = static_cast<double>(pack.payload.size()) * 100.0
                      / static_cast<double>(code.empty() ? 1u : code.size());    //避免除零
    const qint64 deltaFrames = (static_cast<qint64>(pack.payload.size()) + 255) / 256;    //每帧最多256字节
    postLog(QStringLiteral("本次发送：差分升级，差分包 %1 字节，差分率 %2%，总帧数：%3")
            .arg(QString::number(pack.payload.size()))
            .arg(QString::number(rate, 'f', 2))
            .arg(deltaFrames));
  }
  else
  {
    postLog(QStringLiteral("本次发送：整包升级，代码 %1 字节 + 镜像元数据 64 字节")
            .arg(QString::number(code.size())));
  }

  FirmwareUploader uploader(m_transport.get());    //复用当前通道做分帧发送
  uploader.setCancelCheck([this]() { return clientCancelled(); });    //让上传器能中断长流程
  uploader.setLogSink([this](const QString &line) { emit logText(line); });    //把等待设备处理的节点写进日志

  bool didDelta = false;    //记录差分是否真正成功
  auto progress = [&](int percent, uint32_t frameCurrent, uint32_t frameTotal)    //发送进度回调，在工作线程执行
  {
    postProgress(percent);    //百分比仍走原有通道
    emit framesChanged(frameCurrent, frameTotal);    //帧号单独上报，供日志末尾两行显示
    postStatus(QStringLiteral("发送中"));    //百分比只在日志末尾的进度条里体现
  };

  FirmwareUploader::Result result;    //保存本次发送结果
  if(useDelta)    //先尝试差分升级
  {
    result = uploader.sendDelta(pack, progress);
    if(result.ok && result.status == 0)    //设备接受并校验通过
    {
      didDelta = true;
    }
    else if(!result.cancelled && result.ok)    //设备明确拒绝差分才回退整包
    {
      postLog(QStringLiteral("差分未成功，设备回复 0x%1：%2，回退整包")
              .arg(QString::number(result.status, 16).toUpper().rightJustified(2, QChar('0')), iapStatusText(result.status)));
    }
    else
    {
      if(!result.ok)
      {
        postLog(QStringLiteral("差分升级失败: %1").arg(result.error));
      }
      else
      {
        postLog(QStringLiteral("差分升级失败，设备回复 0x%1：%2")
                .arg(QString::number(result.status, 16).toUpper().rightJustified(2, QChar('0')), iapStatusText(result.status)));
      }
      postStatus(QStringLiteral("升级失败"));
      finish(false, QStringLiteral("差分升级失败"), result.fatal);
      return;
    }
  }

  if(result.cancelled)    //用户在差分阶段取消
  {
    postLog(QStringLiteral("已取消升级"));
    postStatus(QStringLiteral("已取消"));
    finish(false, QStringLiteral("已取消"));
    return;
  }

  if(!didDelta)    //整包发送：自动回退或全量优先模式
  {
    const qint64 totalFrames = (static_cast<qint64>(code.size()) + 255) / 256;    //每帧最多256字节，向上取整
    postLog(QStringLiteral("整包升级: %1 字节，总帧数：%2")
            .arg(static_cast<qint64>(code.size()))
            .arg(totalFrames));
    fw::ImageMeta meta;    //整包镜像元数据
    meta.image_ver = version;    //版本号在启动时已编码
    meta.project_id = project;    //项目水印
    result = uploader.sendFull(code, meta, progress);    //下发完整镜像
  }

  if(result.cancelled)    //用户在整包阶段取消
  {
    postLog(QStringLiteral("已取消升级"));
    postStatus(QStringLiteral("已取消"));
    finish(false, QStringLiteral("已取消"));
    return;
  }
  if(!result.ok)    //通信失败
  {
    postLog(QStringLiteral("升级失败: %1").arg(result.error));
    postStatus(QStringLiteral("升级失败"));
    finish(false, result.error, result.fatal);
    return;
  }
  if(result.status != 0)    //设备返回异常状态码
  {
    postLog(QStringLiteral("升级失败，设备回复 0x%1：%2")
            .arg(QString::number(result.status, 16).toUpper().rightJustified(2, QChar('0')), iapStatusText(result.status)));
    postStatus(QStringLiteral("升级失败"));
    finish(false, QStringLiteral("设备返回错误状态码"));
    return;
  }

  const uint32_t expect = didDelta ? static_cast<uint32_t>(pack.payload.size())
                                   : static_cast<uint32_t>(code.size());    //设备确认位置应等于发送总量
  if(result.offset != expect)    //偏移不符说明数据丢失或被截断
  {
    postLog(QStringLiteral("确认位置不符: %1/%2").arg(result.offset).arg(expect));
    postStatus(QStringLiteral("升级失败"));
    finish(false, QStringLiteral("确认位置不符"));
    return;
  }

  postLog(QStringLiteral("升级成功，设备已提交；请断电重上电或按复位键"));
  postStatus(QStringLiteral("升级成功"));
  finish(true, QStringLiteral("升级成功"));
}

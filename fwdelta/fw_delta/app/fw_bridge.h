#ifndef FWBRIDGE_H        //防止头文件被重复包含
#define FWBRIDGE_H        //定义头文件保护宏

#include <QFuture>        //异步任务状态，用于差分生成线程
#include <QByteArray>     //原始字节缓冲，承载设备主动发来的数据
#include <QObject>        //对象基类，提供信号槽机制
#include <QStringList>    //字符串列表，用于串口与客户端列表
#include <QVariant>       //通用类型，向QML暴露客户端明细
#include <QtQml/qqmlregistration.h>  //QML类型注册标记，供qmllint生成类型信息

#include <atomic>         //原子布尔，跨线程取消标志

class QThread;        //传输工作线程的前置声明
class QTimer;         //被动读取定时器的前置声明
class TransportRunner; //传输执行器的前置声明

//日志来源分类，界面只给时间戳之后的内容着色，时间戳保持原色
enum FwLogKind
{
  kLogHost = 0,     //上位机自身的操作与状态
  kLogDevice = 1,   //设备上行打印
  kLogSuccess = 2,  //升级或生成成功的结果
  kLogError = 3     //失败与错误
};

/**
 * @brief   QML与固件升级逻辑之间的桥接对象
 * @note    界面只调用这里的接口；差分算法、串口发送仍在C++里。
 */
class FwBridge : public QObject
{
  Q_OBJECT //启用Qt元对象系统，支持信号槽与属性
  QML_ELEMENT  //注册为QML模块FwDelta中的类型
  QML_SINGLETON  //注册为QML单例，界面按FwBridge访问
  Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)                          //升级或连接任务进行中
  Q_PROPERTY(bool deltaBusy READ deltaBusy NOTIFY deltaBusyChanged)           //差分生成任务进行中
  Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)           //传输通道已打开
  Q_PROPERTY(bool connecting READ connecting NOTIFY connectingChanged)        //连接建立中，界面据此屏蔽重复操作
  Q_PROPERTY(QString status READ status NOTIFY statusChanged)                 //当前状态文字，显示在状态栏
  Q_PROPERTY(QStringList portLabels READ portLabels NOTIFY portsChanged)      //串口下拉框显示文本
  Q_PROPERTY(bool hasPorts READ hasPorts NOTIFY portsChanged)                 //当前是否有可用串口
  Q_PROPERTY(QStringList portNames READ portNames NOTIFY portsChanged)        //串口短名列表，供下拉框直接绑定
  Q_PROPERTY(QStringList localIps READ localIps NOTIFY localIpsChanged)       //本机IPv4地址列表
  Q_PROPERTY(int progress READ progress NOTIFY progressChanged)               //升级进度，0~100
  Q_PROPERTY(QString logText READ logText NOTIFY logTextChanged)              //日志全文，供界面显示与保存
  Q_PROPERTY(QString plainLogText READ plainLogText NOTIFY logTextChanged)    //日志纯文本，供导出使用
  Q_PROPERTY(double deltaPercent READ deltaPercent NOTIFY deltaStatsChanged)  //差分率，差分包占新固件的百分比
  Q_PROPERTY(qint64 oldSize READ oldSize NOTIFY deltaStatsChanged)            //旧固件字节数
  Q_PROPERTY(qint64 newSize READ newSize NOTIFY deltaStatsChanged)            //新固件字节数
  Q_PROPERTY(QString strategy READ strategy NOTIFY deltaStatsChanged)         //本次升级采用的数据来源
  Q_PROPERTY(QString deltaPath READ deltaPath NOTIFY deltaPathChanged)        //最近一次生成的差分包实际落盘路径
  Q_PROPERTY(QString deviceInfo READ deviceInfo NOTIFY deviceInfoChanged)     //设备信息，升级后由设备返回
  Q_PROPERTY(QString clientInfo READ clientInfo NOTIFY clientInfoChanged)     //当前客户端信息
  Q_PROPERTY(QStringList clientList READ clientList NOTIFY clientListChanged) //客户端下拉框文本列表
  Q_PROPERTY(QVariant clientRows READ clientRows NOTIFY clientListChanged)    //客户端明细，供表格显示
  Q_PROPERTY(int clientIndex READ clientIndex NOTIFY clientIndexChanged)      //当前选中的客户端索引

public:
  /**
   * @brief   构造桥接对象，绑定传输回调
   * @param   parent  父对象
   */
  explicit FwBridge(QObject *parent = nullptr);

  /**
   * @brief   析构，等待升级线程结束并释放传输对象
   */
  ~FwBridge() override;

  /**
   * @brief   是否正在升级或连接
   */
  bool busy() const;
  /**
   * @brief   差分生成任务是否正在进行
   */
  bool deltaBusy() const;
  /**
   * @brief   传输通道是否已打开
   */
  bool connected() const;
  /**
   * @brief   是否正在建立连接
   */
  bool connecting() const;
  /**
   * @brief   当前状态文字
   */
  QString status() const;
  /**
   * @brief   串口下拉框显示的文本列表
   */
  QStringList portLabels() const;
  /**
   * @brief   当前是否有可用串口
   */
  bool hasPorts() const;
  /**
   * @brief   串口短名列表，与portLabels按下标一一对应
   */
  QStringList portNames() const;
  /**
   * @brief   升级进度，0~100
   */
  int progress() const;
  /**
   * @brief   日志全文
   */
  QString logText() const;
  /**
   * @brief   日志纯文本，供导出使用
   */
  QString plainLogText() const;
  /**
   * @brief   差分率，差分包占新固件的百分比
   */
  double deltaPercent() const;
  /**
   * @brief   旧固件字节数
   */
  qint64 oldSize() const;
  /**
   * @brief   新固件字节数
   */
  qint64 newSize() const;
  /**
   * @brief   本次升级采用的数据来源
   */
  QString strategy() const;
  /**
   * @brief   最近一次生成的差分包实际落盘路径
   * @note    生成时未指定输出路径则由C++按默认规则决定，界面据此回显
   */
  QString deltaPath() const;
  /**
   * @brief   设备信息
   */
  QString deviceInfo() const;
  /**
   * @brief   当前客户端信息
   */
  QString clientInfo() const;
  /**
   * @brief   客户端下拉框文本列表
   */
  QStringList clientList() const;
  /**
   * @brief   客户端明细列表，供表格显示
   */
  QVariantList clientRows() const;
  /**
   * @brief   当前选中的客户端索引
   */
  int clientIndex() const;

  /**
   * @brief   刷新串口列表
   */
  Q_INVOKABLE void refreshPorts();

  /**
   * @brief   获取本机IPv4地址列表
   * @return  IP列表，服务器模式监听地址用
   * @note    属性形式暴露，打开连接前发出localIpsChanged通知界面重取
   */
  QStringList localIps() const;

  /**
   * @brief   打开串口或网络连接
   * @param   transportType 0串口、1TCP、2UDP
   * @param   portIndex     串口下拉索引
   * @param   baud          波特率
   * @param   dataBits      数据位
   * @param   stopBits      停止位
   * @param   parity        校验位
   * @param   host          目标地址
   * @param   port          目标端口
   * @param   role          0客户端、1服务器
   */
  Q_INVOKABLE void openConnection(int transportType, int portIndex, int baud,
                                  int dataBits, int stopBits, int parity,
                                  int flowControl, const QString &host,
                                  int port, int role);

  /**
   * @brief   关闭当前连接
   */
  Q_INVOKABLE void closeConnection();

  /**
   * @brief   按索引取串口名
   * @param   index  下拉框索引
   * @return  串口名，例如COM5
   */
  Q_INVOKABLE QString portName(int index) const;

  /**
   * @brief   把日志写入文件
   * @param   path  目标文件
   * @param   text  日志内容
   * @return  true成功
   */
  Q_INVOKABLE bool saveLog(const QString &path, const QString &text);

  /**
   * @brief   复制日志到剪贴板
   */
  Q_INVOKABLE void copyLog();

  /**
   * @brief   清空日志
   */
  Q_INVOKABLE void clearLog();

  /**
   * @brief   选择TCP服务器模式下的目标客户端
   * @param   index  客户端下拉索引
   */
  Q_INVOKABLE void selectClient(int index);

  /**
   * @brief   解析固件自带镜像元数据并把结果写进日志
   * @param   path  固件文件路径
   * @return  命中时返回"版本|水印"，没有镜像元数据或内容损坏时返回空串
   * @note    版本与水印只存在于固件的镜像元数据中，界面不再提供手工输入。
   */
  Q_INVOKABLE QString inspectFirmware(const QString &path);

  /**
   * @brief   生成差分包
   * @param   oldPath  旧固件
   * @param   newPath  新固件
   * @param   outPath  输出的.fwd，为空时写入新固件同目录的app.fwd
   * @return  true成功
   * @note    新固件必须自带镜像元数据，版本与水印直接取自该镜像元数据。
   */
  Q_INVOKABLE bool makeDelta(const QString &oldPath, const QString &newPath,
                             const QString &outPath);

  /**
   * @brief   开始烧录
   * @param   newPath     新固件
   * @param   fwdPath     差分包，可为空
   * @note    差分包路径非空时由后台线程自行判断走差分还是整包，
   *          并在发送前把决定结果写进日志，界面不再提供手动选择；
   *          版本与水印取自新固件的镜像元数据，不接受界面传参。
   */
  Q_INVOKABLE void startFlash(const QString &newPath, const QString &fwdPath);

  /**
   * @brief   请求取消正在进行的升级
   */
  Q_INVOKABLE void cancelFlash();

  /**
   * @brief   清空已生成的差分包路径
   * @note    清空后开始烧录不再携带差分包，改走整包发送。
   */
  Q_INVOKABLE void clearDeltaPath();

signals:
  void busyChanged();                   //忙状态变化
  void deltaBusyChanged();              //差分生成状态变化
  void connectedChanged();              //连接状态变化
  void connectingChanged();             //连接建立中状态变化
  void statusChanged();                 //状态文字变化
  void portsChanged();                  //串口列表变化
  void logAdded(int kind, const QString &stamp, const QString &text);   //新增一行日志，kind为来源分类
  void progressChanged(int percent);    //升级进度变化
  void framesChanged(uint32_t frameCurrent, uint32_t frameTotal);    //升级帧号变化
  void logTextChanged();                //日志全文变化
  void deltaStatsChanged();             //差分统计变化
  void deltaPathChanged();              //差分包实际落盘路径变化
  void deviceInfoChanged();             //设备信息变化
  void clientInfoChanged();             //客户端信息变化
  void clientListChanged();             //客户端列表变化
  void clientIndexChanged();            //当前客户端索引变化
  void localIpsChanged();               //本机地址列表变化

private:
  void setBusy(bool busy);                          //设置忙状态并发出通知
  void setDeltaBusy(bool busy);                     //设置差分生成状态并发出通知
  void setConnected(bool connected);                //设置连接状态并发出通知
  void setStatus(const QString &status);            //设置状态文字并发出通知
  void setProgress(int percent);                    //设置升级进度并发出通知
  void setDeviceInfo(const QString &info);          //设置设备信息并发出通知
  void setClientInfo(const QString &info);          //设置客户端信息并发出通知
  void log(const QString &text);                    //追加一条上位机信息，时间戳后按灰色显示
  void logDevice(const QString &text);              //追加一条设备上行打印，时间戳后按深色显示
  void logSuccess(const QString &text);             //追加一条成功信息，时间戳后按绿色显示
  void logError(const QString &text);               //追加一条失败信息，时间戳后按红色显示
  void emitLog(int kind, const QString &text);      //统一加时间戳后投递到GUI线程
  void appendLog(int kind, const QString &stamp, const QString &text);  //追加日志文本并发出通知
  static QString logColor(int kind);                //日志来源对应的文字颜色
  void trimLog();                                   //日志超过上限时丢弃最老的一批
  void postStatus(const QString &status);           //从工作线程投递状态文字到GUI线程
  void postProgress(int percent);                   //从工作线程投递升级进度到GUI线程
  void postFrames(uint32_t frameCurrent, uint32_t frameTotal);       //从工作线程投递帧号到GUI线程
  void postDeltaStats(double percent, qint64 oldSize,
                      qint64 newSize, const QString &strategy);  //从工作线程投递差分统计到GUI线程
  void postDeltaPath(const QString &path);          //从工作线程投递差分包实际路径到GUI线程
  void setConnecting(bool connecting);              //设置连接建立中状态并发出通知
  void onOpenFinished(bool ok, const QString &info, const QString &error);  //传输线程打开结果
  void onClosedFinished();                          //传输线程关闭完成
  void onClientsChanged(const QStringList &names, const QVariantList &rows);  //传输线程客户端列表变化
  void onRunnerProgress(int percent);               //传输线程升级进度
  void onRunnerStatus(const QString &text);         //传输线程状态文字
  void onFlashFinished(bool ok, bool linkBroken, const QString &message);  //传输线程升级结束，linkBroken表示链路已断
  void onFramesChanged(uint32_t frameCurrent, uint32_t frameTotal);  //传输线程上报帧号
  void onRawBytes(const QByteArray &bytes);         //收到设备主动发来的原始字节
  void onLinkLost(const QString &reason);           //链路断开时复位界面连接状态
  void pollPassive();                               //定时向传输线程索取一次被动数据
  void flushText(const QByteArray &raw);            //把一段原始字节按行输出为设备打印
  QString progressLines(bool rich) const;           //生成日志末尾的实时两行，rich决定是否带颜色标记
  bool doMakeDelta(const QString &oldPath, const QString &newPath,
                   const QString &outPath);            //执行差分生成，运行在后台线程
  static QString localPath(const QString &path);    //把QML的file地址转成本地路径

  bool m_busy = false;                         //升级任务进行中
  bool m_deltaBusy = false;                    //差分生成任务进行中
  bool m_connected = false;                    //传输通道已打开
  bool m_connecting = false;                   //正在建立连接
  std::atomic<bool> m_cancelRequested{false};  //升级取消标志，发送线程轮询
  QFuture<void> m_deltaFuture;                 //差分生成线程，析构时等待结束
  QThread *m_thread = nullptr;                 //传输工作线程
  TransportRunner *m_runner = nullptr;         //传输执行器，运行在工作线程
  int m_transportType = -1;                    //当前通道类型，升级结束后据此恢复客户端轮询
  int m_role = 0;                              //当前通道角色，0客户端、1服务器
  int m_progress = 0;                          //升级进度，0~100
  QString m_status;                            //当前状态文字
  QString m_logText;                           //日志全文
  int m_logCount = 0;                          //日志行数，超过上限时触发裁剪
  QString m_plainLog;                          //日志纯文本，与m_logText同步维护，供导出
  bool m_flashActive = false;                  //是否处于发送阶段，决定日志末尾是否显示进度行
  QString m_progressStamp;                     //发送开始时刻的时间戳，进度行固定使用，不随刷新变化
  bool m_closeAfterCancel = false;             //升级期间点过关闭，等升级线程退出后补做关闭
  uint32_t m_frameCurrent = 0;                 //已发送帧号
  uint32_t m_frameTotal = 0;                   //本次总帧数
  QTimer *m_listenTimer = nullptr;             //非升级阶段的被动读取定时器
  QByteArray m_rxBuffer;                       //接收缓冲，用于拆出协议帧与文本
  QString m_textBuffer;                        //按行累积的设备打印文本
  QString m_deviceInfo;                        //设备信息
  QString m_clientInfo;                        //当前客户端信息
  QStringList m_clientList;                    //客户端名称缓存，避免界面线程直接访问传输层
  QVariantList m_clientRows;                   //客户端明细缓存，避免界面线程直接访问传输层
  int m_clientIndex = 0;                       //当前选中的客户端索引
  double m_deltaPercent = 0.0;                 //差分率
  qint64 m_oldSize = 0;                        //旧固件字节数
  qint64 m_newSize = 0;                        //新固件字节数
  QString m_strategy;                          //升级策略说明
  QString m_deltaPath;                         //最近一次生成的差分包实际路径
  QStringList m_portNames;                     //串口设备名列表
  QStringList m_portLabels;                    //串口下拉显示文本列表
};

#endif // FWBRIDGE_H

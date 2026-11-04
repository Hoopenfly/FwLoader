#include <QGuiApplication>        //GUI应用对象，管理事件循环
#include <QIcon>                  //窗口图标
#include <QLibraryInfo>           //Qt安装路径信息，用于定位翻译文件
#include <QLocale>                //区域与语言设置
#include <QQmlApplicationEngine>  //QML加载引擎
#include <QQmlContext>            //QML上下文，用于注入C++对象
#include <QQuickStyle>            //Qt Quick控件风格
#include <QTranslator>            //翻译文件加载器
#include <QUrl>                   //URL类型

#include "fw_bridge.h"  //QML与升级逻辑之间的桥接对象

/**
 * @brief   程序入口
 * @param   argc  参数个数
 * @param   argv  参数
 * @return  退出码
 */
int main(int argc, char *argv[])
{
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))  //Qt5需要手动开启高DPI支持
  //Qt5需要显式开启高DPI缩放
  QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);  //启用高DPI缩放
  QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);     //使用高分辨率图标
#endif  //Qt5兼容分支结束

  QGuiApplication app(argc, argv);  //创建GUI应用，接管事件循环

  //QML使用Material风格
  QQuickStyle::setStyle(QStringLiteral("Material"));  //设置控件风格为Material

  //加载Qt中文翻译，让标准对话框和控件文本显示中文
  QTranslator qtTranslator;      //qrc内置的翻译对象
  QTranslator qtBaseTranslator;  //Qt安装目录的翻译对象
  if(qtTranslator.load(QStringLiteral(":/qt_zh_CN.qm")))  //优先加载qrc内置翻译
  {
    app.installTranslator(&qtTranslator);  //安装翻译，界面显示中文
  }
  else if(qtBaseTranslator.load(QLocale(QLocale::Chinese, QLocale::China), QStringLiteral("qtbase"), QStringLiteral("_"),
                                QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
  {
    //qrc里没有翻译文件时回退到Qt安装目录自带的qtbase翻译
    app.installTranslator(&qtBaseTranslator);  //安装回退翻译
  }

  //应用元信息与窗口图标
  app.setApplicationName(QStringLiteral("FwDelta"));      //应用名，决定配置目录
  app.setApplicationVersion(QStringLiteral("1.0.0"));     //应用版本号
  app.setOrganizationName(QStringLiteral("FwLoader"));    //组织名
  app.setWindowIcon(QIcon(QStringLiteral(":/app.ico")));  //窗口图标

  FwBridge bridge;  //桥接对象，注册给QML调用

  QQmlApplicationEngine engine;  //QML引擎，负责加载界面
  //优先从exe旁边的qml目录加载插件，保证双击可运行
  engine.addImportPath(QCoreApplication::applicationDirPath() + QStringLiteral("/qml"));  //追加插件搜索路径
  //把桥接对象注册为QML单例，界面按FwBridge访问
  qmlRegisterSingletonInstance("FwDelta", 1, 0, "FwBridge", &bridge);  //注册单例，qmllint静态可识别
  engine.load(QUrl(QStringLiteral("qrc:/qml/main.qml")));  //加载主界面
  if(engine.rootObjects().isEmpty())  //QML加载失败时没有根对象
  {
    return -1;  //返回错误码
  }
  return app.exec();  //进入事件循环，返回退出码
}

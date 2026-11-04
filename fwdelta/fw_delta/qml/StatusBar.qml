import QtQuick 6.12           //导入Qt Quick基础模块
import QtQuick.Controls 6.12  //导入控件模块
import QtQuick.Layouts 6.12   //导入布局模块
import FwDelta 1.0            //导入C++注册的FwBridge单例

/**
 * 状态栏：状态点、状态文字、设备信息（进度显示在日志区末尾两行）
 */
RowLayout {                   //状态栏根元素使用水平布局
    id: bar                   //状态栏标识

    Layout.fillWidth: true    //横向填满
    Layout.preferredHeight: 30  //固定高度30
    spacing: 6                //子项间距8

    Rectangle {               //左侧状态圆点
        Layout.preferredWidth: 8   //圆点宽8
        Layout.preferredHeight: 8  //圆点高8
        radius: 4             //圆角4，形成圆形
        color: FwBridge.connected ? "#22C55E"                                  //已连接绿色
                                : (String(FwBridge.status).indexOf("失败") >= 0 ? "#EF4444" : "#4F6BED")  //失败红色，其他蓝色
    }

    Label {                   //状态文字
        text: String(FwBridge.status).length > 0 ? FwBridge.status : "等待操作"  //无状态时显示等待操作
        font.pixelSize: 13    //字号13
        color: "#111827"      //主文字色
    }

    Item { Layout.fillWidth: true }  //中间弹簧，把设备信息推到右侧

    Label {                   //右侧设备信息
        text: (String(FwBridge.deviceInfo).length > 0 ? FwBridge.deviceInfo : "未连接") + " v1.0.0"  //设备信息加版本号
        font.pixelSize: 13    //字号13
        color: "#7C8AA5"      //次要文字色
    }
}

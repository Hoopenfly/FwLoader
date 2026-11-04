import QtQuick 6.12           //导入Qt Quick基础模块
import QtQuick.Controls 6.12  //导入控件模块

/**
 * 按钮：28 高、4px 圆角；颜色可配置，支持图标
 */
Button {                                  //按钮根元素
    id: btn                               //按钮标识，供内部引用

    property color baseColor: "#4F6BED"        //常态背景色
    property color hoverColor: "#5B7CFA"       //悬停背景色
    property color pressedColor: "#3F56C9"     //按下背景色
    property color textColor: "#FFFFFF"        //文字颜色
    property string iconSource: ""             //图标路径，空串表示显示文字

    implicitHeight: 28                    //固定高度28
    font.pixelSize: 14                    //字号14
    hoverEnabled: true                    //开启悬停跟踪，hovered才会随鼠标更新

    //去掉内边距
    topPadding: 0
    bottomPadding: 0
    leftPadding: 0
    rightPadding: 0

    property string toolTipText: ""       //悬停提示文字，空串表示不显示提示

    HoverHandler { id: hoverHandler }     //跟踪鼠标在控件内的位置，供提示框定位

    HintTip {                             //悬停提示，白底小字
        text: btn.toolTipText                                  //提示内容取控件属性
        hoverActive: btn.toolTipText.length > 0 && btn.hovered  //有提示文字且鼠标悬停时启动计时
        anchorItem: btn                                        //鼠标坐标属于按钮自身坐标系
        cursorPos: hoverHandler.point.position                  //提示框跟随鼠标，显示在鼠标右侧
    }

    background: Rectangle {               //按钮背景
        anchors.fill: parent              //填满按钮区域，背景高度与控件高度一致
        radius: 4                         //圆角4
        color: !btn.enabled ? "#C7D2E5"                        //禁用时灰色
                             : (btn.pressed ? btn.pressedColor  //按下色
                                            : (btn.hovered ? btn.hoverColor : btn.baseColor))  //悬停或常态色
        Behavior on color { ColorAnimation { duration: 120 } }  //颜色变化动画120毫秒
    }

    contentItem: Item {                   //按钮内容容器
        Text {                            //文字内容
            anchors.centerIn: parent      //居中
            text: btn.text                //显示按钮文本
            color: btn.enabled ? btn.textColor : "#6B7280"  //禁用时改用深灰，避免白字浅底看不清
            font: btn.font                //沿用按钮字体
            visible: btn.iconSource === ""  //没有图标时才显示文字
        }
        Image {                           //图标内容
            anchors.centerIn: parent      //居中
            width: 28                     //图标宽28，与按钮边长一致
            height: 28                    //图标高28，与按钮边长一致
            source: btn.iconSource        //图标路径
            visible: btn.iconSource !== ""  //有图标时才显示
            fillMode: Image.PreserveAspectFit  //保持比例缩放
        }
    }
}

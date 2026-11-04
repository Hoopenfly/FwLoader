import QtQuick 6.12           //导入Qt Quick基础模块
import QtQuick.Controls 6.12  //导入控件模块
import QtQuick.Layouts 6.12   //导入布局模块，供最小宽度附加属性使用

/**
 * 输入框：28 高、4px 圆角、14px Consolas
 */
TextField {                         //输入框根元素
    id: field                       //输入框标识，供内部引用

    implicitHeight: 28              //固定高度28
    implicitWidth: 0                //不按占位文本撑宽度，由外层布局分配
    Layout.minimumWidth: 0          //允许被压缩，避免多列布局被单个输入框撑破
    leftPadding: 4                  //左内边距
    rightPadding: 4                 //右内边距
    topPadding: 4                   //上内边距
    bottomPadding: 4                //下内边距
    verticalAlignment: TextInput.AlignVCenter   //文字垂直居中
    font.pixelSize: 14              //字号14
    font.family: "Consolas"         //等宽字体
    color: field.enabled ? "#111827" : "#9CA3AF"    //可用深灰近黑，禁用中灰
    placeholderTextColor: field.enabled ? "#9CA3AF" : "#CBD2DD" //占位文字同样随禁用变浅
    selectByMouse: true             //允许鼠标拖选文本
    hoverEnabled: true              //开启悬停跟踪，hovered才会随鼠标更新

    property string toolTipText: ""//悬停提示文字，空串表示不显示提示

    HoverHandler { id: hoverHandler }  //跟踪鼠标在控件内的位置，供提示框定位

    HintTip {                        //悬停提示，白底小字
        text: field.toolTipText                                  //提示内容取控件属性
        hoverActive: field.toolTipText.length > 0 && field.hovered  //有提示文字且鼠标悬停时启动计时
        anchorItem: field                                        //鼠标坐标属于输入框自身坐标系
        cursorPos: hoverHandler.point.position                    //提示框跟随鼠标，显示在鼠标右侧
    }

    background: Rectangle {       //输入框背景
        radius: 4                 //圆角4
        color: field.enabled ? "#F9FAFB" : "#F1F3F6"  //可用浅白，禁用浅灰
        border.color: field.activeFocus ? "#4F6BED" : "#E1E5EC"  //聚焦蓝色，否则浅灰
        border.width: 1           //边框宽度1
    }
}

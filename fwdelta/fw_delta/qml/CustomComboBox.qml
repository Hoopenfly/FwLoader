import QtQuick 6.12           //导入Qt Quick基础模块
import QtQuick.Controls 6.12  //导入控件模块
import QtQuick.Layouts 6.12   //导入布局模块，供最小宽度附加属性使用

pragma ComponentBehavior: Bound  //委托内可限定访问外层控件属性

/**
 * 下拉框：28 高、4px 圆角、14px Consolas
 */
ComboBox {                        //下拉框根元素
    id: combo                     //下拉框标识，供内部引用

    implicitHeight: 28            //固定高度28
    implicitWidth: 0              //不按最长选项撑宽度，由外层布局分配
    Layout.minimumWidth: 0        //允许被压缩，避免多列布局被单个下拉框撑破
    topPadding: 0
    bottomPadding: 0
    leftPadding: 0
    rightPadding: 0
    font.pixelSize: 14            //字号14
    font.family: "Consolas"       //等宽字体，便于数字对齐
    hoverEnabled: true            //开启悬停跟踪，hovered才会随鼠标更新

    property string toolTipText: ""  //悬停提示文字，空串表示不显示提示

    HoverHandler { id: hoverHandler }  //跟踪鼠标在控件内的位置，供提示框定位

    HintTip {                        //悬停提示，白底小字
        text: combo.toolTipText                                  //提示内容取控件属性
        hoverActive: combo.toolTipText.length > 0 && combo.hovered  //有提示文字且鼠标悬停时启动计时
        anchorItem: combo                                        //鼠标坐标属于下拉框自身坐标系
        cursorPos: hoverHandler.point.position                   //提示框跟随鼠标，显示在鼠标右侧
    }

    background: Rectangle {       //下拉框背景
        radius: 4                 //圆角4
        color: combo.enabled ? "#F9FAFB" : "#F1F3F6"  //可用浅白，禁用浅灰
        border.color: combo.activeFocus ? "#4F6BED" : "#E1E5EC"  //聚焦蓝色，否则浅灰
        border.width: 1           //边框宽度1
    }

    contentItem: Text {           //下拉框显示文本
        leftPadding: 12           //左内边距12
        rightPadding: 24          //右内边距给箭头留位置
        text: combo.displayText   //显示当前项文本
        color: combo.enabled ? "#111827" : "#9CA3AF"  //可用深灰近黑，禁用中灰
        font: combo.font          //沿用下拉框字体
        verticalAlignment: Text.AlignVCenter  //垂直居中
        elide: Text.ElideRight    //过长时右侧省略
        // 让 Text 填满 contentItem 区域，高度 = 28
        anchors.fill: parent
    }

    indicator: Image {            //右侧箭头
        x: combo.width - width - 12      //贴右侧并留12边距
        y: (combo.height - height) / 2   //垂直居中
        width: 12                 //箭头宽12
        height: 8                 //箭头高8
        source: "qrc:/chevron_down.png"  //箭头资源
        opacity: combo.enabled ? 1.0 : 0.35  //禁用时箭头一起变浅
    }

    property string hoveredItemText: ""     //最近一次悬停的下拉项文本，保留用于淡出时仍有文字
    property bool hoveredItemActive: false  //当前是否悬停在某一项上

    HintTip {                               //列表项悬停提示，与输入框、下拉框用同一套浅灰样式
        text: combo.hoveredItemText         //提示内容取当前悬停项的完整文本
        hoverActive: combo.hoveredItemActive    //仅在悬停时显示；离开时只关显示，文字保留
        x: 0                                //固定在控件左下方，不跟随鼠标
        y: combo.height + 4
    }

    delegate: ItemDelegate {
        // 自定义下拉列表里每一项的委托组件。
        // 不写 delegate 时，ComboBox 用默认 ItemDelegate；写了就完全由下面这套控制
        id: delegateRoot            //委托根标识，供内部限定访问
        required property int index        //委托注入的项索引
        required property var modelData    //委托注入的项数据
        width: combo.width          //项宽度 = 下拉框宽度
        height: 28                  //项高度28px
        padding: 0                  //清除 ItemDelegate 默认内边距（默认约 12px，上下左右都有）
        leftPadding: 12             //左内边距12px
        rightPadding: 12            //右内边距12px
        onHoveredChanged: {         //把当前悬停项的文本同步给提示框
            if(hovered)             //鼠标进入该项
            {
                combo.hoveredItemText = String(modelData)           //记录本项完整文本
                combo.hoveredItemActive = true                      //标记正在悬停，提示框显示
            }
            else                                                    //鼠标离开本项
            {
                combo.hoveredItemActive = false                     //只关显示，文本保留，淡出时仍有内容
            }
        }
        contentItem: Text {
            text: delegateRoot.modelData  //显示当前项的数据
            font: combo.font        //字体沿用 ComboBox 的 font（14px Consolas）
            color: "#111827"        //文字颜色，深灰近黑
            verticalAlignment: Text.AlignVCenter    //文字在 contentItem 高度内垂直居中
            elide: Text.ElideRight  //文本过长时右侧显示省略号（…），不换行、不溢出
        }

        background: Rectangle {     //替换 ItemDelegate 默认背景
            //高亮项（键盘上下键选中或鼠标悬停项）用浅蓝紫 #EEF2FF，其余项透明，露出弹出列表本身的白色背景
            color: delegateRoot.highlighted ? "#EEF2FF" : "transparent"
        }
        //把 delegate 的 highlighted 绑定到“当前高亮索引是否等于本项索引”
        highlighted: combo.highlightedIndex === index
    }
}

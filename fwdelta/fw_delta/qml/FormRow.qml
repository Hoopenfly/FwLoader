import QtQuick 6.12           //导入Qt Quick基础模块
import QtQuick.Controls 6.12  //导入控件模块
import QtQuick.Layouts 6.12   //导入布局模块

/**
 * 表单行：左侧44宽标签，右侧内容区横向填满
 */
RowLayout {                                 //表单行根元素
    id: root                                //行标识，供内部引用

    Layout.fillWidth: true                  //整行横向填满
    spacing: root.rowSpacing                //标签与内容区之间的间距

    property alias label: labelItem.text    //行标签文字
    property int labelWidth: 44             //标签固定宽度44，保证各行标签左对齐
    property int labelHeight: 28            //标签高度28，与同行控件同高
    property int rowSpacing: 6              //标签与内容区、内容控件之间的间距

    default property alias content: contentRow.data  //内容控件直接写在本组件内，自动收进内容区

    Label {                                 //左侧行标签
        id: labelItem
        Layout.preferredWidth: root.labelWidth
        Layout.preferredHeight: root.labelHeight
        Layout.alignment: Qt.AlignVCenter   //与同行控件垂直居中
        horizontalAlignment: Text.AlignLeft //文字左对齐
        verticalAlignment: Text.AlignVCenter
        font.pixelSize: 14                  //字号14
        color: "#374151"                    //标签文字色
    }

    RowLayout {                             //右侧内容区
        id: contentRow
        Layout.fillWidth: true              //横向填满剩余宽度
        Layout.minimumWidth: 0              //允许压缩到0，避免内容宽度撑破外层卡片等分
        spacing: root.rowSpacing            //内容控件之间的间距
    }
}

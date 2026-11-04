import QtQuick 6.12          //导入Qt Quick基础模块
import QtQuick.Layouts 6.12  //导入布局模块
import QtQuick.Effects 6.12  //导入阴影特效模块

/**
 * 卡片容器：白底、8px圆角、浅灰边框和轻微阴影
 */
Rectangle {                           //卡片根元素使用矩形
    id: card                          //卡片标识，供内部引用

    property int contentSpacing: 16   //内容项之间的间距，默认16
    implicitHeight: body.implicitHeight + 32  //高度按内容撑开，额外加32给上下内边距

    radius: 8                         //圆角半径8
    color: "#FFFFFF"                  //卡片底色为白色
    border.color: "#E6EAF0"           //边框颜色为浅灰
    border.width: 1                   //边框宽度1像素

    layer.enabled: true               //开启图层，阴影需要离屏渲染
    layer.effect: MultiEffect {       //离屏渲染时套用阴影特效
        shadowEnabled: true           //启用阴影
        shadowBlur: 0.5               //阴影模糊程度
        shadowColor: "#14000000"      //阴影颜色，带透明度的黑色
        shadowVerticalOffset: 2       //阴影向下偏移2像素
    }

    default property alias content: body.data  //默认子项自动加入内容布局

    ColumnLayout {                    //卡片内容使用垂直布局
        id: body                      //内容布局标识
        anchors.fill: parent          //填满整个卡片
        anchors.margins: 12           //四周内边距12
        spacing: card.contentSpacing  //子项间距由卡片属性控制
    }
}

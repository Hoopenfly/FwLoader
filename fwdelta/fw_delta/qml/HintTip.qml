import QtQuick 6.12           //导入Qt Quick基础模块
import QtQuick.Controls 6.12  //导入控件模块
import QtQuick.Window 6.12    //导入Window模块，用于读取窗口尺寸做边界钳制

/**
 * 悬停提示：浅灰细边框、12号字，用于替代默认深色提示
 * 悬停后弹出固定时长即自动收起，鼠标移出控件再移入才重新弹出
 */
ToolTip {                          //提示根元素，继承自ToolTip
    id: tip                        //提示标识，供内容项引用

    delay: 1000                     //悬停1000毫秒后弹出，避免快速划过时闪烁
    padding: 4                     //文字四周留4像素内边距

    property int showDuration: 1500             //每次弹出后显示的时长，单位毫秒
    property bool hoverActive: false            //鼠标是否停在控件上，由使用处绑定控件自身的hovered
    property point cursorPos: Qt.point(-1, -1)  //鼠标坐标，坐标系由anchorItem决定，x为负表示不跟随鼠标
    property Item anchorItem: parent            //cursorPos所属的Item，默认父项；与控件局部坐标一致时必须显式传入该控件
    property int cursorGap: 12                  //提示框与鼠标的水平间距，单位像素
    property bool shown: false                  //内部可见状态，由悬停计时器驱动

    readonly property bool followCursor: cursorPos.x >= 0  //鼠标位置有效时才启用跟随定位

    visible: shown

    //悬停状态一变就重新计时：移入开始倒计时，移出立即隐藏
    onHoverActiveChanged: {
        if(hoverActive)
        {
            shown = true                                           //先置可见，弹窗按delay计时后出现
            hideTimer.interval = tip.delay + tip.showDuration      //延迟加显示时长，到点收起
            hideTimer.restart()
        }
        else
        {
            hideTimer.stop()                                       //移出时停表并立即隐藏
            shown = false
        }
    }

    Timer {
        id: hideTimer                       //显示时长计时器
        repeat: false                       //只触发一次
        onTriggered: tip.shown = false      //到点收起，鼠标仍在控件内也不再弹出
    }

    property point tipPos: Qt.point(0, 0)  //提示框坐标缓存，按需重算，避免每次绑定求值都做映射

    x: followCursor ? tipPos.x : 0  //跟随鼠标时用缓存坐标，否则由使用处指定
    y: followCursor ? tipPos.y : 0

    //还没弹出时按鼠标位置预定位；一旦弹出就不再跟随，鼠标在控件上移动时位置保持不变
    onCursorPosChanged: if(followCursor && !visible) tipPos = calc_cursor_pos()
    onVisibleChanged: if(visible && followCursor) tipPos = calc_cursor_pos()

    background: Rectangle {        //提示背景
        color: "#303133"
        radius: 4                  //圆角4像素
        border.color: "#E1E5EC"    //浅灰边框，与输入框边框一致
        border.width: 1            //边框宽度1像素
    }

    contentItem: Text {            //提示文字
        text: tip.text             //文字取提示自身的text属性
        color: "#FFFFFF"
        font.pixelSize: 12         //字号12，比控件正文小两号
        font.family: "Microsoft YaHei"  //中文字体，避免宋体发虚
    }

    /**
     * @brief 计算跟随鼠标时的提示框坐标
     * @return 相对父项的坐标，水平优先放在鼠标右侧，右侧放不下时翻到左侧，最后钳制在窗口内
     */
    function calc_cursor_pos() {
        if(!parent)                    //父项未就绪时先返回原点，避免访问空对象
        {
            return Qt.point(0, 0)
        }

        //鼠标点在anchorItem坐标系中的位置，换算成窗口坐标
        var pt = anchorItem.mapToItem(null, cursorPos.x, cursorPos.y)
        var origin = parent.mapToItem(null, 0, 0)
        var win = parent.Window.window          //取所在窗口，用于边界判断
        var winW = win ? win.width : 0
        var winH = win ? win.height : 0

        //水平默认放在鼠标右侧，右侧超出窗口时翻到左侧
        var wx = pt.x + cursorGap
        if(winW > 0 && wx + width > winW)
        {
            wx = pt.x - cursorGap - width
        }
        if(winW > 0)                    //兜底钳制，两侧都放不下时贴边显示
        {
            wx = Math.max(0, Math.min(wx, winW - width))
        }

        //垂直方向与鼠标中心对齐，再钳制到窗口上下边界内
        var wy = pt.y - height / 2
        if(winH > 0)
        {
            wy = Math.max(0, Math.min(wy, winH - height))
        }

        //窗口坐标换算回父项坐标，Popup的x、y以父项为原点
        return Qt.point(wx - origin.x, wy - origin.y)
    }
}

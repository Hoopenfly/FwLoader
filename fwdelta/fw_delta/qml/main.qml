import QtQuick 6.12                     //导入Qt Quick基础模块
import QtQuick.Window 6.12              //导入窗口模块，提供Window类型
import QtQuick.Controls 6.12            //导入控件模块，提供Label/Button等
import QtQuick.Controls.Material 6.12   //导入Material主题模块
import QtQuick.Layouts 6.12             //导入布局模块，提供RowLayout等
import QtQuick.Dialogs 6.12             //导入对话框模块，提供FileDialog
import FwDelta 1.0                      //导入C++注册的FwBridge单例

pragma ComponentBehavior: Bound         //委托内可限定访问外层组件属性

/**
 * FwLoader 差分升级工具主界面
 * 三卡片 1:1:1，表单用 ColumnLayout + RowLayout 紧凑排列，按钮贴底
 */
Window {
    id: root                                    //窗口标识，供内部引用

    width: 800                                  //窗口宽度800像素
    height: 600                                 //窗口高度600像素
    minimumWidth: 800                           //最小宽度固定800，不允许横向缩放
    minimumHeight: 600                          //最小高度固定600，不允许纵向缩放
    maximumWidth: 800                           //最大宽度固定800
    maximumHeight: 600                          //最大高度固定600
    visible: true                               //加载完成后立即显示窗口
    title: "FwLoader 差分升级工具"                //窗口标题文字
    color: "#F5F7FA"                            //窗口底色浅灰

    Material.theme: Material.Light              //使用Material浅色主题
    Material.accent: "#4F6BED"                  //主题强调色为蓝色

    Component.onCompleted: FwBridge.refreshPorts()  //窗口载入后自动枚举一次串口，之后由刷新按钮手动触发

    /**
     * @brief   把 FileDialog 的 QUrl 转成本地路径
     * @param   url  文件 URL
     * @return  本地路径字符串
     */
    function toLocalFile(url) {
        var s = url.toString();                 //先把QUrl转成字符串
        if(s.indexOf("file:///") === 0) {       //Windows下带盘符的file:///前缀
            s = s.substring(8);                 //去掉8个字符的file:///
        } else if(s.indexOf("file://") === 0) { //其他形式的file://前缀
            s = s.substring(7);                 //去掉7个字符的file://
            if(s.length > 0 && s.charAt(0) !== "/") {   //不以斜杠开头说明是网络共享路径
                s = "\\\\" + s;                 //补回双反斜杠，恢复成 \\server\share 形式
            }
        }
        s = decodeURIComponent(s);              //把URL编码的中文等还原
        if(s.indexOf("\\\\") === 0) {           //网络共享路径统一改用反斜杠，Windows才能识别
            s = s.replace(/\//g, "\\");
        }
        return s;
    }

    //主布局：从上到下依次放三卡、日志区和状态栏
    ColumnLayout {
        anchors.fill: parent                    //布局填满整个窗口
        anchors.margins: 6                      //四周留6像素边距
        spacing: 6                              //子项之间间隔6像素

        //顶部三张卡片，宽度按1:1:1等分，高度取三者内容最高值
        RowLayout {
            Layout.fillWidth: true              //三卡整行横向填满
            Layout.fillHeight: false            //高度不参与拉伸
            Layout.preferredHeight: 246         //三卡固定高度
            spacing: 6                          //三卡之间间隔

            //通道设置
            Card {
                id: channelCard                 //通道卡标识
                Layout.minimumWidth: 0          //允许被压缩到0，避免撑破布局
                Layout.fillWidth: true          //与其他两卡等分横向空间
                Layout.preferredWidth: 300      //与其他两卡相同的基准宽度，保证1:1:1
                Layout.fillHeight: true         //高度跟随行高填满
                contentSpacing: 4               //卡内各行间距4像素

                //通道卡标题
                SectionTitle { text: "通道设置" }

                FormRow {
                    label: "通道"                   //通道类型标签：串口 / TCP / UDP
                    rowSpacing: 4                                   //标签与控件间距4像素
                    CustomComboBox {
                        id: transportCombo                       //通道类型下拉框
                        Layout.fillWidth: true                   //横向填满剩余宽度
                        model: ["串口", "TCP", "UDP"]             //三个可选通道
                        enabled: !FwBridge.connected && !FwBridge.busy && !FwBridge.connecting   //通道打开、连接中或任务进行中禁止再切换
                        toolTipText: "选择通道类型：串口 / TCP / UDP"
                    }
                }


                FormRow {
                    label: "串口"                                   //串口设备标签
                    rowSpacing: 4                                   //标签与控件间距4像素
                    visible: transportCombo.currentIndex === 0      //只在串口模式下显示
                    CustomComboBox {
                        id: portCombo                            //串口设备下拉框
                        Layout.fillWidth: true                   //横向填满剩余宽度
                        Layout.minimumWidth: 0                   //允许压缩，长描述不撑破卡片
                        model: FwBridge.hasPorts ? FwBridge.portLabels : ["无可用串口"]  //显示"端口名(设备描述)"，便于识别具体设备
                        enabled: !FwBridge.connected && !FwBridge.connecting   //通道打开或连接过程中禁止更换串口
                        toolTipText: !FwBridge.hasPorts ? "当前无可用串口，请点击刷新按钮扫描"
                                    : ((FwBridge.portLabels[portCombo.currentIndex] || "选择串口设备")
                                       + (FwBridge.connected ? "（已打开）" : ""))  //悬停显示完整设备描述
                    }
                }

                FormRow {
                    label: "波特率"                                 //波特率标签
                    rowSpacing: 4                                   //标签与控件间距4像素
                    visible: transportCombo.currentIndex === 0      //只在串口模式下显示
                    CustomComboBox {
                        id: baudCombo                            //波特率下拉框
                        Layout.fillWidth: true                   //横向填满剩余宽度
                        currentIndex: 4                          //默认选中115200
                        model: ["9600", "19200", "38400", "57600", "115200", "230400", "460800", "921600"]  //可选波特率
                        enabled: !FwBridge.connected && !FwBridge.connecting   //通道打开或连接过程中禁止修改波特率
                        toolTipText: "选择串口波特率"
                    }
                }

                FormRow {
                    label: "模式"                                     //TCP角色标签
                    rowSpacing: 4                                   //标签与控件间距4像素
                    visible: transportCombo.currentIndex !== 0      //只在TCP/UDP模式下显示
                    CustomComboBox {
                        id: roleCombo                            //TCP角色下拉框
                        Layout.fillWidth: true                   //横向填满剩余宽度
                        model: ["客户端", "服务器"]                //客户端主动连接，服务器等待接入
                        enabled: !FwBridge.connected && !FwBridge.busy && !FwBridge.connecting   //通道打开、连接中或任务进行中禁止修改角色
                        toolTipText: "选择角色：客户端主动连接，服务器等待接入"
                    }
                }

                FormRow {
                    label: "地址"                                   //地址标签
                    rowSpacing: 4                                   //标签与控件间距4像素
                    visible: transportCombo.currentIndex !== 0      //只在TCP/UDP模式下显示

                    CustomComboBox {
                        id: localIpCombo                         //本机网卡地址下拉框
                        Layout.fillWidth: true                   //横向填满
                        visible: roleCombo.currentIndex === 1    //服务器模式选本机IP
                        model: FwBridge.localIps                 //本机可用IP列表
                        enabled: !FwBridge.connected && !FwBridge.busy   //通道打开或任务进行中禁止修改
                        toolTipText: "选择本机监听的网卡地址"
                    }
                    CustomTextField {
                        id: ipField                              //目标设备IP输入框
                        Layout.fillWidth: true                   //横向填满
                        visible: roleCombo.currentIndex !== 1    //客户端模式手输目标IP
                        text: "192.168.1.100"                    //默认目标地址
                        horizontalAlignment: TextInput.AlignHCenter  //居中显示
                        enabled: !FwBridge.connected && !FwBridge.busy   //通道打开或任务进行中禁止修改
                        validator: RegularExpressionValidator { regularExpression: /^[0-9.]*$/ }  //只允许数字和点，完整格式在编辑完成时校验
                        onEditingFinished: {                     //编辑完成时校验四段点分十进制地址
                            var m = /^([0-9]{1,3})\.([0-9]{1,3})\.([0-9]{1,3})\.([0-9]{1,3})$/.exec(text)
                            if(!m || parseInt(m[1]) > 255 || parseInt(m[2]) > 255
                               || parseInt(m[3]) > 255 || parseInt(m[4]) > 255)
                                text = "192.168.1.100"           //格式不符时恢复初始地址
                        }
                        toolTipText: "输入目标设备的IP地址"
                    }
                }

                FormRow {
                    label: "端口"                                   //端口标签
                    rowSpacing: 4                                   //标签与控件间距4像素
                    visible: transportCombo.currentIndex !== 0      //只在TCP/UDP模式下显示
                    CustomTextField {
                        id: portField                            //端口输入框
                        Layout.fillWidth: true                   //横向填满剩余宽度
                        text: "8080"                             //默认端口8080
                        horizontalAlignment: TextInput.AlignHCenter  //数字居中显示
                        enabled: !FwBridge.connected && !FwBridge.busy   //通道打开或任务进行中禁止修改
                        validator: IntValidator { bottom: 1; top: 65535 }  //端口只允许1~65535的十进制整数
                        toolTipText: "输入端口号（1~65535）"
                    }
                }

                //串口刷新按钮：重新扫描刚插入的串口
                CustomButton {
                    id: refreshBtn                  //串口刷新按钮
                    Layout.fillWidth: false         //不填满卡片，宽度取卡片内容的2/3
                    Layout.alignment: Qt.AlignHCenter           //水平居中，两侧各留1/6
                    Layout.preferredWidth: parent.width //宽度
                    visible: transportCombo.currentIndex === 0   //只在串口模式下显示
                    text: "刷新"                     //按钮文字
                    enabled: !FwBridge.connected && !FwBridge.busy && !FwBridge.connecting   //连接过程中、已打开或任务进行中时禁止刷新
                    toolTipText: "重新扫描当前可用的串口设备"
                    onClicked: FwBridge.refreshPorts()    //手动枚举一次串口
                }

                //底部按钮：打开 / 连接 / 监听 / 关闭
                CustomButton {
                    id: connectBtn                  //通道开关按钮
                    Layout.fillWidth: false         //不填满卡片，宽度取卡片内容的2/3
                    Layout.alignment: Qt.AlignHCenter           //水平居中，与刷新按钮对齐
                    Layout.preferredWidth: parent.width //宽度
                    text: FwBridge.connected ? "关闭"
                         : (transportCombo.currentIndex === 0 ? "打开"
                         : (roleCombo.currentIndex === 1 ? "监听" : "连接"))  //按通道类型和角色切换按钮文字
                    enabled: (FwBridge.connected || !FwBridge.busy) && !FwBridge.connecting   //已连接时始终可点，保证升级中也能关闭通道
                    baseColor: FwBridge.connected ? "#EF4444" : "#4F6BED"       //已连接红色，未连接蓝色
                    hoverColor: FwBridge.connected ? "#F87171" : "#5B7CFA"      //悬停色
                    pressedColor: FwBridge.connected ? "#DC2626" : "#3F56C9"    //按下色
                    toolTipText: FwBridge.connected ? "关闭当前通道" : "打开或连接当前通道"
                    onClicked: {
                        if(FwBridge.connected)        //已连接则执行关闭
                        {
                            FwBridge.closeConnection()
                        }
                        else                        //未连接则按当前选择打开通道
                        {
                            FwBridge.openConnection(transportCombo.currentIndex,    //打开请求排队到传输线程执行
                                                  portCombo.currentIndex,
                                                  parseInt(baudCombo.currentText),
                                                  8, 1, 0, 0,
                                                  (roleCombo.currentIndex === 1 ? localIpCombo.currentText : ipField.text),
                                                  parseInt(portField.text),
                                                  roleCombo.currentIndex)
                        }
                    }
                }
            }

            //烧录设置
            Card {
                id: flashCard                   //烧录卡标识
                Layout.minimumWidth: 0          //允许被压缩，避免撑破布局
                Layout.fillWidth: true          //与其他两卡等分横向空间
                Layout.preferredWidth: 300      //与其他两卡相同的基准宽度，保证1:1:1
                Layout.fillHeight: true         //高度跟随行高填满
                contentSpacing: 4               //卡内各行间距4像素

                //烧录卡标题
                SectionTitle { text: "烧录设置" }

                //表单区：直接铺在卡片里，内容超出卡片高度时裁掉，不提供滚动
                ColumnLayout {
                    Layout.fillWidth: true             //横向填满卡片
                    Layout.fillHeight: true            //纵向吃掉卡片剩余高度
                    clip: true                         //超出卡片的部分裁掉，不画到卡外
                    spacing: 18                         //行间距

                        FormRow {
                            label: "旧固件"                 //旧固件标签
                            rowSpacing: 4                   //标签与控件间距4像素

                            CustomTextField {
                                id: oldField                        //旧固件路径输入框
                                Layout.fillWidth: true              //横向填满
                                readOnly: true                      //路径只由文件对话框写入
                                enabled: !FwBridge.busy && !FwBridge.deltaBusy  //烧录中或差分生成中禁止修改
                                toolTipText: "旧固件文件路径，作为差分的基准版本"
                            }
                            CustomButton {
                                Layout.preferredWidth: 28           //方形按钮，边长与图标一致
                                iconSource: "qrc:/folder.png"       //文件夹图标
                                baseColor: "#00F0F2F5"              //浅灰全透明，与按下色同色相，渐变只改透明度
                                hoverColor: "#00F0F2F5"             //悬停不变色
                                pressedColor: "#F0F2F5"             //按下极浅灰
                                enabled: !FwBridge.busy && !FwBridge.deltaBusy  //烧录中或差分生成中禁止点击
                                toolTipText: "浏览选择旧固件文件"
                                onClicked: oldDialog.open()         //打开文件选择对话框
                            }
                        }

                        FormRow {
                            label: "新固件"                 //新固件标签
                            rowSpacing: 4                   //标签与控件间距4像素

                            CustomTextField {
                                id: newField                        //新固件路径输入框
                                Layout.fillWidth: true              //横向填满
                                readOnly: true                      //路径只由文件对话框写入
                                enabled: !FwBridge.busy && !FwBridge.deltaBusy  //烧录中或差分生成中禁止修改
                                toolTipText: "新固件文件路径，要升级到的目标版本"
                            }
                            CustomButton {
                                Layout.preferredWidth: 28           //方形按钮，边长与图标一致
                                iconSource: "qrc:/folder.png"       //文件夹图标
                                baseColor: "#00F0F2F5"              //浅灰全透明，与按下色同色相，渐变只改透明度
                                hoverColor: "#00F0F2F5"             //悬停不变色
                                pressedColor: "#F0F2F5"             //按下极浅灰
                                enabled: !FwBridge.busy && !FwBridge.deltaBusy  //烧录中或差分生成中禁止点击
                                toolTipText: "浏览选择新固件文件"
                                onClicked: newDialog.open()         //打开文件选择对话框
                            }
                        }

                        //差分包行：标签与路径框占满整行，与上面两行的对齐方式一致
                        FormRow {
                            label: "差分包"                 //差分包标签
                            rowSpacing: 4                   //标签与控件间距4像素

                            CustomTextField {
                                id: outField                        //差分包路径输入框
                                Layout.fillWidth: true              //横向填满
                                readOnly: true                      //路径由生成差分后回填
                                enabled: !FwBridge.busy && !FwBridge.deltaBusy  //烧录中或差分生成中禁止修改
                                toolTipText: "差分包的保存路径，生成差分后自动填入；留空则保存到新固件同目录"
                            }
                            CustomButton {
                                Layout.preferredWidth: 28           //方形按钮，边长与图标一致
                                iconSource: "qrc:/clear.png"        //清空图标
                                baseColor: "#00F0F2F5"              //浅灰全透明，与按下色同色相，渐变只改透明度
                                hoverColor: "#00F0F2F5"             //悬停不变色
                                pressedColor: "#F0F2F5"             //按下极浅灰
                                enabled: !FwBridge.busy && !FwBridge.deltaBusy  //烧录中或差分生成中禁止点击
                                toolTipText: "清空差分包路径，开始烧录时改发整包"
                                onClicked: {
                                    outField.text = ""          //清掉界面上的路径显示
                                    FwBridge.clearDeltaPath()   //同步清掉内部记录，保证开始烧录走整包
                                }
                            }
                        }

                        //操作按钮行：生成差分与开始烧录各占一半权重，二者等分
                        RowLayout {
                            Layout.fillWidth: true              //整行横向填满
                            spacing: 4                          //两个按钮间距

                            CustomButton {
                                id: generateBtn                     //生成差分按钮
                                Layout.fillWidth: true              //左半区占一半权重
                                Layout.minimumWidth: 0              //允许压缩，保证两半等分
                                Layout.preferredWidth: 0            //从零开始算，宽度只由权重决定
                                Layout.preferredHeight: 28          //与其他行控件同高
                                text: "生成差分"                        //按钮文字
                                enabled: !FwBridge.busy && !FwBridge.deltaBusy  //烧录中或差分生成中禁止点击
                                toolTipText: "按新旧固件生成差分包，生成后自动保存并回填路径"
                                onClicked: FwBridge.makeDelta(oldField.text, newField.text, outField.text)
                            }

                            CustomButton {
                                id: startBtn                    //开始烧录按钮
                                Layout.fillWidth: true          //右半区占一半权重
                                Layout.minimumWidth: 0          //允许压缩，保证两半等分
                                Layout.preferredWidth: 0        //从零开始算，宽度只由权重决定
                                Layout.preferredHeight: 28      //与其他行控件同高
                                //发送阶段取消能立刻停住；数据已发完时设备在自行重建，只能停止等待
                                text: !FwBridge.busy ? "开始烧录"
                                                      : (FwBridge.flashPhase === 1 ? "停止等待" : "取消烧录")
                                enabled: true                                    //始终保持蓝色可点击，缺少前置条件时由日志提示
                                toolTipText: !FwBridge.busy
                                             ? (!FwBridge.connected ? "尚未打开通道，点击后会提示先连接"
                                                                    : "开始烧录，有差分包时自动走差分，否则发整包")
                                             : (FwBridge.flashPhase === 1
                                                ? "数据已全部发给设备，设备正在重建并提交镜像；此处只能停止等待，撤不回本次升级，此间不要断电"
                                                : "请求取消当前烧录，已发出的数据帧处理完后生效")
                                onClicked: {
                                    if(FwBridge.busy)            //烧录进行中：转为取消请求
                                    {
                                        FwBridge.cancelFlash()
                                    }
                                    else                          //空闲：按当前选择开始烧录
                                    {
                                        //烧录只需要一个"目标固件"来源，新框留空时用旧框：整包发送的就是它，差分真正发送的是差分包
                                        //"生成差分"才需要新旧两个固件来算差异，与这里的取源无关
                                        var src = newField.text
                                        if(src.length === 0 && outField.text.length > 0)
                                            src = ""            //带差分包时不能回退成旧固件，否则会静默烧掉旧镜像
                                        else if(src.length === 0)
                                            src = oldField.text
                                        FwBridge.startFlash(src, outField.text)
                                    }
                                }
                            }
                        }
                    }
            }

            //客户端列表
            Card {
                id: clientCard                  //客户端卡标识
                Layout.minimumWidth: 0          //允许被压缩，避免撑破布局
                Layout.fillWidth: true          //与其他两卡等分横向空间
                Layout.preferredWidth: 300      //与其他两卡相同的基准宽度，保证1:1:1
                property var rows: FwBridge.clientRows            //当前客户端数据，没有客户端时长度为0
                readonly property int minRowCount: 7            //表格常驻行数，无客户端时这些行留空但仍可选中
                Layout.fillHeight: true         //高度跟随行高填满
                contentSpacing: 6               //卡内标题与表格间距6像素

                //客户端卡标题
                SectionTitle { text: "客户端列表" }

                //表格外框：ListView 自带选中行，点击只改 currentIndex
                Rectangle {
                    id: tableBox                                            //表格外框标识
                    Layout.fillWidth: true                                  //外框横向填满卡片
                    Layout.fillHeight: true                                 //外框纵向填满卡片
                    color: "#FFFFFF"                                        //表格底色白色
                    border.color: "#E1E5EC"                                 //表格边框浅灰
                    border.width: 1                                         //边框宽度1像素

                    HoverHandler { id: tableHover }                         //跟踪鼠标是否停在表格上

                    HintTip {                                               //悬停提示，白底小字
                        text: "客户端列表：点击某一行选中为烧录目标"              //提示文字
                        hoverActive: tableHover.hovered                      //鼠标在表格内时启动计时
                        anchorItem: tableBox                                 //鼠标坐标属于表格外框坐标系
                        cursorPos: tableHover.point.position                 //提示框跟随鼠标，显示在鼠标右侧
                    }

                    ListView {
                        id: clientList                          //客户端列表视图
                        anchors.fill: parent                    //填满外框
                        clip: true                              //超出部分裁剪，不画到框外
                        model: Math.max(clientCard.minRowCount, clientCard.rows.length)  //行数取常驻行数与实际客户端数的较大值
                        currentIndex: -1                        //初始不选中任何行
                        boundsBehavior: Flickable.StopAtBounds  //到头就停，不出现回弹

                        //与C++的选中客户端保持同步：客户端断开或重新选择时刷新高亮
                        Connections {
                            target: FwBridge
                            function onClientIndexChanged() {
                                clientList.currentIndex = FwBridge.clientIndex  //-1表示没有选中
                            }
                        }

                        header: Rectangle {
                            width: ListView.view.width          //表头宽度跟随列表宽度
                            height: 28                          //表头高度28像素
                            color: "#F9FAFB"                    //表头底色浅灰

                            RowLayout {
                                anchors.fill: parent            //表头布局填满
                                spacing: 0                      //单元格紧贴无间隙

                                //序号列
                                Rectangle {
                                    Layout.preferredWidth: 32       //序号列宽32像素
                                    Layout.fillHeight: true         //纵向填满表头
                                    border.color: "#E1E5EC"         //单元格边框浅灰
                                    border.width: 1                 //边框宽度1像素
                                    color: "transparent"            //底色透明，露出表头灰底
                                    Label {
                                        anchors.fill: parent                    //文字填满单元格
                                        text: "序号"                            //列标题
                                        horizontalAlignment: Text.AlignHCenter  //水平居中
                                        verticalAlignment: Text.AlignVCenter    //垂直居中
                                        font.pixelSize: 13                      //字号13
                                        color: "#7C8AA5"                        //表头文字灰蓝色
                                    }
                                }
                                //时间列
                                Rectangle {
                                    Layout.preferredWidth: 70       //时间列宽70像素
                                    Layout.fillHeight: true         //纵向填满表头
                                    border.color: "#E1E5EC"         //单元格边框浅灰
                                    border.width: 1                 //边框宽度1像素
                                    color: "transparent"            //底色透明
                                    Label {
                                        anchors.fill: parent                    //文字填满单元格
                                        text: "时间"                            //列标题
                                        horizontalAlignment: Text.AlignHCenter  //水平居中
                                        verticalAlignment: Text.AlignVCenter    //垂直居中
                                        font.pixelSize: 13                      //字号13
                                        color: "#7C8AA5"                        //表头文字灰蓝色
                                    }
                                }
                                //IP端口列
                                Rectangle {
                                    Layout.fillWidth: true          //占满剩余宽度
                                    Layout.fillHeight: true         //纵向填满表头
                                    border.color: "#E1E5EC"         //单元格边框浅灰
                                    border.width: 1                 //边框宽度1像素
                                    color: "transparent"            //底色透明
                                    Label {
                                        anchors.fill: parent                    //文字填满单元格
                                        text: "IP端口"                          //列标题
                                        horizontalAlignment: Text.AlignHCenter  //水平居中
                                        verticalAlignment: Text.AlignVCenter    //垂直居中
                                        font.pixelSize: 13                      //字号13
                                        color: "#7C8AA5"                        //表头文字灰蓝色
                                    }
                                }
                            }
                        }

                        delegate: Rectangle {
                            id: clientRow                       //行委托标识，供内部限定访问
                            required property int index         //委托注入的行索引
                            width: ListView.view.width          //行宽跟随列表宽度
                            height: 24                          //行高24像素
                            property var rowData: index < clientCard.rows.length ? clientCard.rows[index] : undefined  //当前行对应的客户端，空行时为undefined
                            color: index === clientList.currentIndex ? "#4F6BED" : "#FFFFFF"  //选中行蓝色，其余白色

                            RowLayout {
                                anchors.fill: parent            //行内布局填满整行
                                spacing: 0                      //单元格紧贴

                                //序号单元格
                                Rectangle {
                                    Layout.preferredWidth: 32       //序号列宽32像素
                                    Layout.fillHeight: true         //纵向填满整行
                                    border.color: "#E1E5EC"         //单元格边框浅灰
                                    border.width: 1                 //边框宽度1像素
                                    color: "transparent"            //底色透明，露出行的蓝或白
                                    Label {
                                        anchors.fill: parent                    //文字填满单元格
                                        text: clientRow.rowData ? clientRow.rowData.index : ""      //显示客户端序号，空行留空
                                        horizontalAlignment: Text.AlignHCenter  //水平居中
                                        verticalAlignment: Text.AlignVCenter    //垂直居中
                                        font.pixelSize: 13                      //字号13
                                        color: clientRow.index === clientList.currentIndex ? "#FFFFFF" : "#374151"  //选中行白字，其余深灰
                                    }
                                }
                                //时间单元格
                                Rectangle {
                                    Layout.preferredWidth: 70       //时间列宽70像素
                                    Layout.fillHeight: true         //纵向填满整行
                                    border.color: "#E1E5EC"         //单元格边框浅灰
                                    border.width: 1                 //边框宽度1像素
                                    color: "transparent"            //底色透明
                                    Label {
                                        anchors.fill: parent                    //文字填满单元格
                                        text: clientRow.rowData ? clientRow.rowData.time : ""       //显示连接时间，空行留空
                                        horizontalAlignment: Text.AlignHCenter  //水平居中
                                        verticalAlignment: Text.AlignVCenter    //垂直居中
                                        font.pixelSize: 13                      //字号13
                                        color: clientRow.index === clientList.currentIndex ? "#FFFFFF" : "#374151"  //选中行白字，其余深灰
                                    }
                                }
                                //IP端口单元格
                                Rectangle {
                                    Layout.fillWidth: true          //占满剩余宽度
                                    Layout.fillHeight: true         //纵向填满整行
                                    border.color: "#E1E5EC"         //单元格边框浅灰
                                    border.width: 1                 //边框宽度1像素
                                    color: "transparent"            //底色透明
                                    Label {
                                        anchors.fill: parent                    //文字填满单元格
                                        text: clientRow.rowData ? (clientRow.rowData.ip + ":" + clientRow.rowData.port) : ""  //显示IP与端口，空行留空
                                        horizontalAlignment: Text.AlignHCenter  //水平居中
                                        verticalAlignment: Text.AlignVCenter    //垂直居中
                                        elide: Text.ElideRight                  //过长时右侧省略
                                        font.pixelSize: 13                      //字号13
                                        color: clientRow.index === clientList.currentIndex ? "#FFFFFF" : "#374151"  //选中行白字，其余深灰
                                    }
                                }
                            }

                            MouseArea {
                                anchors.fill: parent            //点击区覆盖整行
                                onClicked: {
                                    clientList.currentIndex = clientRow.index                      //空行也可以选中，选中只是视觉标记
                                    FwBridge.selectClient(clientRow.index < clientCard.rows.length ? clientRow.index : -1)  //数据行同步给C++作为通信目标，空行则取消选择
                                }
                            }
                        }
                    }
                }
            }
        }

        //日志区
        Card {
            Layout.fillWidth: true              //日志卡横向填满
            Layout.fillHeight: true             //日志卡纵向占满剩余高度
            contentSpacing: 6                   //标题行与正文间距6像素

            //日志标题行：左侧标题，右侧导出/清空按钮
            RowLayout {
                Layout.fillWidth: true              //标题行横向填满
                Layout.preferredHeight: 28          //标题行高28像素
                spacing: 6                          //子项间距6像素

                //日志标题
                SectionTitle { text: "日志" }
                Item { Layout.fillWidth: true }     //弹簧空白，把按钮推到右侧
                CustomButton {
                    text: "清空"                    //按钮文字
                    Layout.preferredWidth: 72       //固定宽72像素
                    Layout.preferredHeight: 24      //固定高24像素
                    baseColor: "#4F6BED"            //蓝色底
                    hoverColor: "#5B7CFA"           //悬停色
                    pressedColor: "#3F56C9"         //按下色
                    textColor: "#FFFFFF"            //白色文字
                    toolTipText: "清空日志内容"
                    onClicked: FwBridge.clearLog()    //调用C++清空日志
                }
                CustomButton {
                    text: "导出"                    //按钮文字
                    Layout.preferredWidth: 72       //固定宽72像素
                    Layout.preferredHeight: 24      //固定高24像素
                    baseColor: "#4F6BED"            //蓝色底
                    hoverColor: "#5B7CFA"           //悬停色
                    pressedColor: "#3F56C9"         //按下色
                    textColor: "#FFFFFF"            //白色文字
                    toolTipText: "把日志内容导出为文本文件"
                    onClicked: {
                        //默认文件名用当前时刻，格式 YYYYMMDDHHMMSS.log
                        exportDialog.currentFile = Qt.formatDateTime(new Date(), "yyyyMMddHHmmss") + ".log"
                        exportDialog.open()             //打开导出对话框
                    }
                }
            }

            //日志正文外框，负责画边框和裁剪
            Rectangle {
                id: logBox                          //日志外框标识
                Layout.fillWidth: true              //外框横向填满
                Layout.fillHeight: true             //外框纵向填满
                color: "#FFFFFF"                    //正文底色白色
                border.color: "#E1E5EC"             //外框边框浅灰
                border.width: 1                     //边框宽度1像素
                clip: true                          //超出部分裁剪

                //日志正文滚动区
                ScrollView {
                    id: logScroll
                    anchors.fill: parent            //滚动区填满外框
                    clip: true                      //超出部分裁剪

                    TextArea {
                        id: logArea                             //日志文本区
                        property int logHtmlLen: 0              //已渲染进文本区的富文本长度，用于判断是否需要整段重设
                        //日志一般走增量追加；只有清空、裁剪或末尾进度行刷新时才整段重设
                        Component.onCompleted: {
                            text = FwBridge.logText             //首次加载时一次性填入已有日志
                            logHtmlLen = FwBridge.logText.length    //记下已渲染长度
                        }
                        Connections {
                            target: FwBridge
                            function onLogTextChanged() {            //全文变化：清空、裁剪后需要整段重设
                                if (FwBridge.logText.length === logArea.logHtmlLen)
                                    return                          //长度未变，增量追加已经处理过
                                var ci = logScroll.contentItem       //滚动区内容项
                                var atBottom = (ci.contentHeight - ci.contentY - logScroll.height) < 8    //判断是否停在底部
                                var keepY = ci.contentY              //记住当前滚动位置
                                var selStart = logArea.selectionStart    //记住拖选范围，整段赋值会清掉选择
                                var selEnd = logArea.selectionEnd
                                logArea.text = FwBridge.logText      //整段赋值，末尾两行随之刷新
                                logArea.logHtmlLen = FwBridge.logText.length    //同步已渲染长度
                                if(selEnd > selStart)                //之前有选中时按原下标恢复，越界部分夹到文本末尾
                                    logArea.select(Math.min(selStart, logArea.length),
                                                   Math.min(selEnd, logArea.length))
                                if(atBottom)                         //原来在底部时继续跟随到底部
                                    ci.contentY = Math.max(0, ci.contentHeight - logScroll.height)
                                else                                 //原来在中间或顶部时保持原位
                                    ci.contentY = keepY
                            }
                            function onProgressLineChanged() {   //仅进度行变化：只替换末尾两行
                                var ci = logScroll.contentItem       //滚动区内容项
                                var atBottom = (ci.contentHeight - ci.contentY - logScroll.height) < 8    //判断是否停在底部
                                var full = FwBridge.logText          //含进度行的最新全文
                                var base = FwBridge.logBodyLength    //历史日志长度，进度行从 base 开始
                                if (base > 0 && base <= logArea.length) {
                                    logArea.remove(base, logArea.length)    //删掉旧进度行
                                    logArea.insert(base, full.substring(base))    //写入新进度行
                                } else {
                                    logArea.text = full               //结构异常时退回整段赋值
                                }
                                logArea.logHtmlLen = full.length
                                if (atBottom)                        //原来在底部时继续跟随到底部
                                    ci.contentY = Math.max(0, ci.contentHeight - logScroll.height)
                            }
                            function onLogSegmentAppended(html) {    //尾部新增日志：只追加，避免整段复制
                                var ci = logScroll.contentItem       //滚动区内容项
                                var atBottom = (ci.contentHeight - ci.contentY - logScroll.height) < 8    //判断是否停在底部
                                logArea.append(html)                 //追加增量片段
                                logArea.logHtmlLen += html.length    //累加已渲染长度
                                if(atBottom)                         //原来在底部时继续跟随到底部
                                    ci.contentY = Math.max(0, ci.contentHeight - logScroll.height)
                            }
                        }
                        textFormat: TextEdit.RichText           //日志带颜色标记，按富文本渲染
                        readOnly: true                          //只读，不允许编辑
                        selectByMouse: true                     //允许鼠标拖选
                        wrapMode: TextArea.Wrap                 //超长自动换行
                        font.pixelSize: 13                      //字号13
                        color: "#4B5563"                        //正文深灰
                        ContextMenu.menu: null                  //禁用自带右键菜单，改用自定义的
                        background: null                        //去掉默认背景，由外层矩形提供
                        topPadding: 6
                        bottomPadding: 6
                        leftPadding: 6
                        rightPadding: 6

                        MouseArea {
                            anchors.fill: parent                    //覆盖整个文本区
                            acceptedButtons: Qt.RightButton         //只接收右键
                            propagateComposedEvents: true           //左键等事件继续向下传递
                            onClicked: function(mouse) {
                                logArea.forceActiveFocus()                      //先把焦点交给文本框，保证复制可用
                                var p = mapToItem(null, mouse.x, mouse.y)       //把鼠标坐标映射到窗口坐标
                                logMenu.x = p.x                                 //菜单横坐标
                                logMenu.y = p.y                                 //菜单纵坐标
                                logMenu.popup()                                 //弹出右键菜单
                            }
                        }
                    }
                }
            }
        }

        //底部状态栏：状态点、状态文字、进度、设备信息
        StatusBar { }
    }

    //日志区右键菜单：只保留复制和全选
    Menu {
        id: logMenu                 //右键菜单标识
        width: 80                   //菜单宽度80像素
        font.pixelSize: 13          //统一设置菜单文字大小

        MenuItem {
            text: "复制"             //菜单项文字
            height: 32              //控制菜单项高度
            enabled: logArea.selectedText.length > 0    //没有选中内容时禁用
            onClicked: logArea.copy()                   //复制选中文本到剪贴板
        }
        MenuItem {
            text: "全选"             //菜单项文字
            height: 32              //控制菜单项高度
            onClicked: logArea.selectAll()  //选中全部日志文本
        }
    }

    //旧固件选择对话框
    FileDialog {
        id: oldDialog                       //对话框标识
        title: "选择旧固件"                  //对话框标题
        fileMode: FileDialog.OpenFile       //打开已有文件
        nameFilters: ["固件文件 (*.bin)", "所有文件 (*)"]  //文件类型过滤
        onAccepted: oldField.text = root.toLocalFile(oldDialog.selectedFile)  //选中后把路径填进输入框
    }

    //新固件选择对话框
    FileDialog {
        id: newDialog                       //对话框标识
        title: "选择新固件"                  //对话框标题
        fileMode: FileDialog.OpenFile       //打开已有文件
        nameFilters: ["固件文件 (*.bin)", "所有文件 (*)"]  //文件类型过滤
        onAccepted: {                                       //选中后立即解析固件自带的镜像元数据
            newField.text = root.toLocalFile(newDialog.selectedFile)  //把路径填进输入框
            FwBridge.inspectFirmware(newField.text)                   //版本与水印只来自镜像元数据，结果写进日志
        }
    }

    //差分包生成完成后回显实际落盘路径，路径由C++决定，界面只负责显示
    Connections {
        target: FwBridge
        function onDeltaPathChanged() {                   //C++已把实际路径写入deltaPath属性
            if(FwBridge.deltaPath.length > 0)             //空路径表示尚未生成过
                outField.text = FwBridge.deltaPath        //直接填进差分包输入框
        }
    }

    //日志导出对话框
    FileDialog {
        id: exportDialog                                    //对话框标识
        title: "导出日志"                                    //对话框标题
        fileMode: FileDialog.SaveFile                       //另存为
        nameFilters: ["日志文件 (*.log *.txt)", "所有文件 (*)"]  //文件类型过滤
        defaultSuffix: "log"                                //用户没写后缀时自动补 .log
        onAccepted: {
            //导出纯文本，不带颜色标记；失败时由FwBridge写日志提示，这里不再重复
            FwBridge.saveLog(root.toLocalFile(exportDialog.selectedFile), FwBridge.plainLogText)
        }
    }

}

<#
  FwLoader 产线串口烧录工具
  流程：找bin -> 选串口 -> 自动试波特率 -> 逐帧发送(带重传) -> 提示手动复位
  重要：串口读取一律用 BytesToRead 轮询，绝不用 ReadLine——
        ReadLine 一旦超时抛异常，.NET SerialPort 内部状态会坏掉，之后再也读不到数据。
#>

#可传入的参数：固件路径、串口号、等待秒数(都有默认值)
param([string]$BinFile = "", [string]$PortName = "", [int]$WaitReadySec = 150)

#任何错误都立即终止，不静默继续
$ErrorActionPreference = "Stop"
#取脚本自身所在目录，保证双击运行和命令行调用都能定位到 app.bin 与 log
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$LogDir = Join-Path $ScriptDir "log"
if(-not(Test-Path $LogDir))
{
    New-Item -ItemType Directory -Path $LogDir | Out-Null
}
$LogFile = Join-Path $LogDir ("flash_{0}.txt" -f (Get-Date -Format "yyyyMMdd_HHmmss"))
#串口接收累积缓冲(脚本作用域，供Read-OneLine使用)
$RxBuf = ""
#进度行是否正在屏幕上(它是用NoNewline原地刷新的，打日志前需要先收尾)
$script:ProgressShown = $false
#是否具备控制台：被重定向输出或后台运行时没有
#探测必须和Clear-Host一样去写光标位置：只读CursorPosition不报错、写才报"句柄无效"，只看读会误判
#没有控制台时跳过清屏，并且不等待回车，否则调用方会被挂住
$script:HasConsole = $true
try { $Host.UI.RawUI.CursorPosition = $Host.UI.RawUI.CursorPosition } catch { $script:HasConsole = $false }

<#
@brief   同时输出到屏幕和日志文件
@param   m  消息内容
@param   c  屏幕颜色
#>
function Write-Log([string]$m, [string]$c = "Gray")
{
    $line = "[{0}] {1}" -f (Get-Date -Format "HH:mm:ss"), $m
    Write-Host $line -ForegroundColor $c
    Add-Content -Path $LogFile -Value $line -Encoding UTF8
}

<#
@brief   提示后按回车再退出
@param   code   退出码
@param   Pause  是否停下来等回车。异常退出要停(否则窗口一闪就没)，操作员主动按Q退出时传$false直接走
@note    双击bat运行时，脚本一退出窗口就会关闭。所有异常退出路径都走这里，
         否则操作员根本来不及看清报错就没了。
         被重定向调用时没有控制台，此时不等待回车，直接退出。
#>
function Exit-WithPause([int]$code, [bool]$Pause = $true)
{
    if($Pause -and $script:HasConsole)
    {
        Write-Host ""
        Write-Host "按回车键退出" -NoNewline
        [Console]::ReadLine() | Out-Null
    }
    exit $code
}

<#
@brief   打日志前把原地刷新的进度行收尾
@note    进度行没显示时什么都不做，避免凭空多出一个空行
#>
function End-ProgressLine
{
    if($script:ProgressShown)
    {
        Write-Host ""
        $script:ProgressShown = $false
    }
}

<#
@brief   计算CRC16-Modbus
@param   d  数据
@return  CRC值
@note    初值0xFFFF、多项式0xA001，必须与设备端fw_iap.c逐位一致
#>
function Get-Crc16([byte[]]$d)
{
    $crc = 0xFFFF
    foreach($b in $d)
    {
        $crc = $crc -bxor $b
        for($i = 0; $i -lt 8; $i++)
        {
            if($crc -band 1)
            {
                $crc = ($crc -shr 1) -bxor 0xA001
            }
            else
            {
                $crc = $crc -shr 1
            }
        }
    }
    return ($crc -band 0xFFFF)
}

<#
@brief   从串口取出一整行(轮询方式，不用ReadLine)
@param   sp         串口对象
@param   timeoutMs  最长等待毫秒数
@return  取到的一行，超时返回$null
@note    ReadLine超时会抛异常并破坏SerialPort内部状态，之后再也读不到数据，
         所以自己用BytesToRead轮询 + 累积缓冲拼行。
#>
function Read-OneLine($sp, [int]$timeoutMs)
{
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while($sw.Elapsed.TotalMilliseconds -lt $timeoutMs)
    {
        if($sp.BytesToRead -gt 0)
        {
            #$script: 才能改到脚本作用域，否则半行数据会丢在函数局部变量里
            $script:RxBuf += $sp.ReadExisting()
            if($script:RxBuf.Contains("`n"))
            {
                $i = $script:RxBuf.IndexOf("`n")
                $line = $script:RxBuf.Substring(0, $i).Trim()
                $script:RxBuf = $script:RxBuf.Substring($i + 1)
                return $line
            }
        }
        #轮询间隔15毫秒：再短会空转占用CPU，再长则高波特率下接收缓冲可能溢出
        Start-Sleep -Milliseconds 15
    }
    return $null
}

<#
@brief   确定固件文件
@return  固件的完整路径
#>
function Resolve-BinFile
{
    if($BinFile -ne "" -and (Test-Path $BinFile))
    {
        return (Resolve-Path $BinFile).Path
    }
    $auto = Join-Path $ScriptDir "app.bin"
    if(Test-Path $auto)
    {
        return $auto
    }
    Write-Log "当前目录下缺少 app.bin，请将固件拷贝至当前目录并命名为 app.bin" "Yellow"
    $f = Read-Host "或输入 bin 文件的完整路径"
    if(-not(Test-Path $f))
    {
        Write-Log "文件不存在: $f" "Red"
        Exit-WithPause 1
    }
    return (Resolve-Path $f).Path
}

<#
@brief   确定串口
@return  串口名(如COM5)
#>
function Resolve-Port
{
    #@()必需：只有一个串口时Sort-Object会返回标量，$ports[0]会退化成取字符串首字符(COM5 -> C)
    $ports = @([System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object)
    if($ports.Count -eq 0)
    {
        #没检测到串口时不定死：自动轮询等待，操作员插上线就继续
        Write-Log "未检测到串口，请检查串口线插入状态(按 Q 退出)" "Red"
        while($ports.Count -eq 0)
        {
            if(-not $script:HasConsole)
            {
                #非交互调用没有操作员插线，直接退出，避免调用方被永久挂住
                Write-Log "无控制台且未检测到串口，退出" "Red"
                Exit-WithPause 1 -Pause $false
            }
            Start-Sleep -Milliseconds 1000
            #输入被重定向(非交互)时KeyAvailable会抛异常，先判断
            if(-not [Console]::IsInputRedirected -and [Console]::KeyAvailable)
            {
                $k = [Console]::ReadKey($true)
                if($k.Key -eq "Q")
                {
                    #操作员主动按Q，直接退出（不要求按回车）
                    Exit-WithPause 1 -Pause $false
                }
            }
            $ports = @([System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object)
        }
        Write-Log ("检测到串口: " + ($ports -join ", ")) "Green"
    }
    if($PortName -ne "" -and ($ports -contains $PortName))
    {
        return $PortName
    }
    Write-Log "检测到串口:" "Yellow"
    for($i = 0; $i -lt $ports.Count; $i++)
    {
        Write-Host ("   [{0}] {1}" -f ($i + 1), $ports[$i])
    }
    $sel = $null
    try
    {
        $sel = Read-Host "输入序号"
    }
    catch
    {
        #非交互模式(脚本/CI调用)下Read-Host不可用，明确报错而不是抛出异常
        Write-Log "无法交互选择串口，请用 -PortName 指定(如 -PortName COM5)" "Red"
        Exit-WithPause 1
    }
    $idx = 0
    if(-not [int]::TryParse($sel, [ref]$idx))
    {
        Write-Log "序号无效" "Red"
        Exit-WithPause 1
    }
    $idx = $idx - 1
    if($idx -lt 0 -or $idx -ge $ports.Count)
    {
        Write-Log "序号无效" "Red"
        Exit-WithPause 1
    }
    return $ports[$idx]
}

<#
@brief   发送一帧并等待应答
@param   sp         串口对象
@param   frame      整帧数据
@param   no         帧号(仅用于日志)
@param   retryMax   最多重发几次
@param   waitSec    每次等待秒数
@return  哈希表 @{ok=是否成功; line=设备回的那一行}
@note    收到$UP应答算成功；以$开头的其它内容是设备报错，不重试；
         不以$开头的是设备调试输出(如升级进度)，记下来继续等。
#>
function Send-Frame($sp, [byte[]]$frame, [int]$no, [int]$retryMax, [int]$waitSec = 8)
{
    for($t = 1; $t -le $retryMax; $t++)
    {
        #把整帧写出去
        $sp.Write($frame, 0, $frame.Length)
        #本次等待的截止时刻
        $deadline = (Get-Date).AddSeconds($waitSec)
        while((Get-Date) -lt $deadline)
        {
            #每次最多阻塞300ms，好让外层能检查超时
            $line = Read-OneLine $sp 300
            #空行/超时都跳过继续等
            if($line -eq $null -or $line -eq "")
            {
                continue
            }
            #必须用单引号：双引号会把$UP当作变量展开，匹配条件永远不成立
            if($line -like '$UP*')
            {
                return @{ok = $true; line = $line}
            }
            if($line -like '$*')
            {
                return @{ok = $false; line = $line}
            }
        }
        #本次超时，进入下一轮重发
        End-ProgressLine
        Write-Log ("第{0}帧 第{1}次无应答，重发" -f $no, $t) "Yellow"
    }
    return @{ok = $false; line = "TIMEOUT"}
}

# ==================== 主流程 ====================

if($script:HasConsole)
{
    try
    {
        Clear-Host
    }
    catch
    {
        #个别宿主仍可能拒绝清屏，忽略即可：日志照常输出，只是不清屏
    }
}
Write-Host "==========================================" -ForegroundColor Cyan
Write-Host "   FwLoader 产线串口烧录工具" -ForegroundColor Cyan
Write-Host "==========================================" -ForegroundColor Cyan

#第一步：确定固件
$bin = Resolve-BinFile
$fw = [System.IO.File]::ReadAllBytes($bin)
$total = $fw.Length
#一次性读入整个固件，算总长度和整包CRC16
$crc = Get-Crc16 $fw
Write-Log ("固件: {0}" -f (Split-Path $bin -Leaf))
Write-Log ("大小: {0} 字节   整包CRC16: {1}" -f $total, $crc)
#第二步：确定串口
$port = Resolve-Port

$tpl = '$UP {0:D6} {1:D6} {2:D5} '
#协议上限FW_IAP_TEXT_DATA_MAX=300，帧长324小于接收缓冲340
$chunk = 300
$done = $false
$uncertain = $false

#依次尝试4种波特率(设备波特率存在EEPROM里，现场不一定知道)
foreach($baud in @(9600, 115200, 57600, 38400))
{
    Write-Log ("尝试波特率 {0} ..." -f $baud) "Cyan"
    $sp = New-Object System.IO.Ports.SerialPort $port, $baud, "None", 8, "One"
    #写超时3秒，防止串口异常时永久卡死
    $sp.WriteTimeout = 3000
    try
    {
        #按 8-N-1 打开串口
        $sp.Open()
    }
    catch
    {
        Write-Log ("打开串口失败: " + $_.Exception.Message) "Red"
        continue
    }
    #清空接收缓冲，避免上一轮残留数据干扰
    $script:RxBuf = ""
    $handshaked = $false

    $n0 = [Math]::Min($chunk, $total)
    $hb = [System.Text.Encoding]::ASCII.GetBytes(($tpl -f 0, $total, $crc))
    $fr = New-Object byte[] ($hb.Length + $n0)
    [Array]::Copy($hb, 0, $fr, 0, $hb.Length)
    [Array]::Copy($fw, 0, $fr, $hb.Length, $n0)
    #第一帧作为握手帧：只重试2次、每次3秒，失败即切换下一个波特率
    $r = Send-Frame $sp $fr 1 2 3
    if(-not $r.ok)
    {
        $sp.Close()
        Write-Log "无应答，更换波特率" "Yellow"
        continue
    }
    Write-Log ("波特率 {0} 已握手: {1}" -f $baud, $r.line) "Green"
    #握手成功说明波特率是对的，后面再丢帧就是线路问题，不该再换波特率
    $handshaked = $true

    #逐帧发完剩下的固件
    $offset = $n0
    $no = 1
    $t0 = Get-Date
    $abort = $false
    $lastAck = $false
    while($offset -lt $total)
    {
        #本帧要发的字节数(最后一帧可能不足300)
        $n = [Math]::Min($chunk, $total - $offset)
        $hb = [System.Text.Encoding]::ASCII.GetBytes(($tpl -f $offset, $total, $crc))
        $fr = New-Object byte[] ($hb.Length + $n)
        [Array]::Copy($hb, 0, $fr, 0, $hb.Length)
        [Array]::Copy($fw, $offset, $fr, $hb.Length, $n)
        $isLast = (($offset + $n) -ge $total)
        $r = Send-Frame $sp $fr ($no + 1) 3 3
        #最后一帧的应答就是"整包已收完并提交"的确认
        if($r.ok -and $isLast)
        {
            $lastAck = $true
        }
        if(-not $r.ok)
        {
            if($isLast)
            {
                #提交那一刻设备串口行为特殊，收不到完整应答属正常
                End-ProgressLine
                Write-Log "最后一帧无完整应答(设备已提交，属正常)" "Yellow"
            }
            else
            {
                End-ProgressLine
                Write-Log ("第{0}帧 发送失败: {1}" -f ($no + 1), $r.line) "Red"
                $abort = $true
            }
        }
        #推进已发送字节数
        $offset += $n
        #推进帧号
        $no++
        #用回车符回到行首配合NoNewline原地覆盖，避免进度刷满整个窗口
        Write-Host ("`r发送进度 {0,3}%  ({1}/{2} 字节)" -f [int](100 * $offset / $total), $offset, $total) -NoNewline
        $script:ProgressShown = $true
        if($abort)
        {
            break
        }
    }
    End-ProgressLine

    if(-not $abort)
    {
        Write-Log ("发送完成: {0} 帧, 耗时 {1:N1}s" -f $no, ((Get-Date) - $t0).TotalSeconds) "Green"
        #设备提交固件后不会自行复位(这样才能保持串口连接有效)，
        #升级的复位由工作人员断电或按复位键完成。
        #提交确认已在发送循环中取得，此处无需再等待一轮。
        if($lastAck)
        {
            Write-Log "设备已确认整包接收完成" "Green"
            $done = $true
        }
        else
        {
            Write-Log "未收到最后一帧的完整应答" "Red"
            $uncertain = $true
        }
    }
    try
    {
        $sp.Close()
    }
    catch
    {
    }
    #握过手就不再试别的波特率：换波特率解决不了线路丢帧，只会把已发进度作废
    if($done -or $uncertain -or $handshaked)
    {
        break
    }
}

# ==================== 最终结果 ====================

End-ProgressLine
if($done)
{
    Write-Host "======== 固件已提交，请手动复位 ========" -ForegroundColor Green
    Write-Log "固件已提交到设备，请断电重新上电(或按复位键)，BootLoader会自动完成升级并启动App" "Green"
}
elseif($uncertain)
{
    Write-Host "============ 已发送，待人工确认 ============" -ForegroundColor Yellow
    Write-Log "固件已发送，但未收到提交确认，请人工确认板子状态" "Yellow"
}
else
{
    Write-Host "================ 失败 ================" -ForegroundColor Red
    Write-Log "烧录失败：请检查串口线、板子是否上电、固件是否匹配" "Red"
}
Write-Log ("日志: " + $LogFile)
End-ProgressLine
Exit-WithPause 0

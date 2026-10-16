/**
 * @file    fw_iap.c
 * @brief   IAP协议解析与数据处理
 * @note    支持两种协议格式：
 *          1. 文本格式（$UP指令）
 *          2. 二进制格式（自定义协议）
 */

#include "fw_iap.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "fw_port.h"   //fw_port_send_buf
#include "fw_msg.h"

/* App构建必带IAP；BootLoader构建由FW_BOOT_IAP_ENABLE决定是否内置(见fw_boot.h) */
#if (FW_BOOT_BUILD_LOADER == 1) || (FW_BOOT_IAP_ENABLE != 0)

/* 编译期保护：平台串口接收缓冲必须放得下最长的IAP帧(见fw_iap.h的FW_IAP_RX_FRAME_MAX)。
 * 缓冲大小由平台通过FW_PORT_RX_BUF_SIZE声明，本模块不直接引用平台头文件。 */
#if (FW_IAP_RX_FRAME_MAX > FW_PORT_RX_BUF_SIZE)
	#error "FW_PORT_RX_BUF_SIZE is too small for IAP frames"
#endif

//IAP响应接口：发送经平台适配层，本模块不直接调用平台串口函数
#define FW_IAP_SEND(buf, len)  fw_port_send_buf((const uint8_t *)(buf), (uint16_t)(len))

//协议常量(帧格式/类型/状态码)见 fw_iap.h；HEX的就地解析用调用方的缓冲，
//会话状态全部放在下面的 cache 里，本文件没有HEX专用的静态变量

//IAP数据接收缓冲区，用于IAP过程中缓存一页数据
__attribute__((aligned(FW_FLASH_WRITE_BYTE))) uint8_t iap_page_buf[FW_FLASH_PAGE_SIZE];

//IAP接收状态缓存：记录当前接收进度、写入地址、镜像大小等
static iap_page_cache_t cache = {0};

/**
 * @brief   写入一页数据到Flash（写入前自动擦除）
 * @param   addr        Flash目标地址（应为页起始地址）
 * @param   buf         数据缓冲区指针
 * @param   size        有效数据大小
 * @param   is_last     是否为最后一页
 * @return  0=成功，-1=失败
 * @note    非最后一页：size须等于页大小
 *          最后一页：在buf末尾填充擦除态(FW_ERASED_VALUE)至写粒度对齐边界
 */
static int fw_iap_write_page(uint32_t addr, uint8_t *buf, uint32_t size, uint8_t is_last)
{
	const uint32_t gra_byte = FW_FLASH_WRITE_BYTE;
	uint32_t aligned_size;
	uint32_t page_start = addr & ~(FW_FLASH_PAGE_SIZE - 1);
	//擦除目标页
	if(fw_port_flash_erase(page_start, FW_FLASH_PAGE_SIZE) != 0)
	{
		return -1;
	}
	//计算写入大小
	if(is_last)
	{
		//最后一页：对齐到写入粒度，不足部分填充擦除态
		aligned_size = ((size + gra_byte - 1) / gra_byte) * gra_byte;
		if(aligned_size > size)
		{
			memset(buf + size, FW_ERASED_VALUE, aligned_size - size);
		}
	}
	else
	{
		//非最后一页：须写满整页
		if(size != FW_FLASH_PAGE_SIZE)
		{
			return -1;
		}
		aligned_size = FW_FLASH_PAGE_SIZE;
	}
	//写入Flash
	if(fw_port_flash_write(addr, buf, aligned_size) != 0)
	{
		return -1;
	}
	return 0;
}

/**
 * @brief   IAP升级提交：在协议层校验通过后写入Boot标志
 * @param   image_size  本次写入的镜像字节数(交换模式据此只交换有效页)
 * @return  0表示成功
 * @note    文本协议在全文件CRC校验通过后调用
 *          二进制协议在帧级CRC校验通过后调用（结束帧时）
 *          所有模式统一写入FW_BOOT_FLAG_VALID，由bootloader根据模式执行升级
 */
static int fw_iap_commit_upgrade(uint32_t image_size)
{
	//检查IAP数据是否完整
	if(image_size == 0)
	{
		return -1;//未接收到有效数据
	}
#if (FW_BOOT_UPGRADE_MODE == 1)
	//写入固件实际大小，交换模式据此仅交换有效页数
	//文本协议用已接收字节数，二进制协议用END帧确认的总长度
	if(fw_boot_image_size_write(image_size) != 0)
	{
		return -1;
	}
#endif
	//写入Boot标志
	if(fw_boot_flag_write(FW_BOOT_FLAG_VALID) != 0)
	{
		return -1;
	}
	fw_msg_iap_done(image_size);
	return 0;
}

/**
 * @brief   按升级模式确定本会话的写入目标地址
 * @return  目标槽位的起始地址
 * @note    覆写/交换模式写入SLOT1；XIP模式写入当前运行槽位的对侧槽位，
 *          即升级到当前没在运行的那一份，运行中的镜像不被破坏。
 *          文本与二进制两条协议共用本函数，保证目标槽位判断一致。
 */
static uint32_t fw_iap_write_target(void)
{
#if (FW_BOOT_UPGRADE_MODE == 2)
	//XIP模式：写入运行槽位的对侧槽位
	return (fw_xip_get_run_slot() == FW_XIP_RUN_SLOT0) ? SLOT1_ADDR : SLOT0_ADDR;
#else
	//覆写/交换模式：写入SLOT1
	return SLOT1_ADDR;
#endif
}

/**
 * @brief   处理IAP数据包
 * @param   data        数据缓冲区指针
 * @param   len         数据长度
 * @param   total       固件文件总大小（起始帧时传入）
 * @param   frame_type  帧类型：0-起始帧，1-数据帧，2-结束帧
 * @return  成功返回实际接收数据大小，失败返回负错误码
 */
int fw_iap_process_data(uint8_t *data, uint32_t len, uint32_t total, uint8_t frame_type)
{
	//参数检查
	if(data == NULL || len == 0)
	{
		return FW_IAP_ERR_PARAM;
	}
	//========== 起始帧：初始化IAP缓存 ==========
	if(frame_type == 0)
	{
		cache.page_index = 0;
		cache.total_size = total;
		cache.received_size = 0;
		cache.in_progress = 1;
		cache.percent = 0;            //进度清零(上一轮可能停在100)
		cache.file_crc = 0xFFFF;      //整包CRC16初值
		cache.page_buf = iap_page_buf;
		//根据升级模式确定写入目标地址
		cache.write_addr = fw_iap_write_target();
		//页缓冲区初始化为擦除态
		memset(cache.page_buf, FW_ERASED_VALUE, FW_FLASH_PAGE_SIZE);
		//重置升级进度状态，保证新IAP周期从初始状态开始
#if (FW_BOOT_UPGRADE_MODE == 1)
		fw_boot_progress_write(FW_SWAP_PROGRESS_NONE);
#elif (FW_BOOT_UPGRADE_MODE == 2)
		fw_boot_progress_write(fw_xip_get_run_slot());
#endif
		//起始帧先检查声明的镜像总大小：超过一个槽位直接拒收，不写入任何字节
		//(二进制协议不带总大小，total=0时不判断，由逐帧边界检查兜底)
		if(cache.total_size > FW_IAP_IMAGE_MAX_SIZE)
		{
			cache.in_progress = 0;
			return FW_IAP_ERR_SIZE;
		}
		fw_msg_iap_start(cache.total_size);
	}
	//========== 检查IAP是否已开始 ==========
	if(!cache.in_progress)
	{
		return FW_IAP_ERR_NOT_START;
	}
	//========== 保存当前状态用于异常恢复 ==========
	uint32_t saved_page_index = cache.page_index;
	uint32_t saved_received_size = cache.received_size;
	uint32_t saved_current_addr = cache.write_addr;
	//========== 循环拷贝：按页写入Flash（支持一帧数据跨多页） ==========
	uint32_t data_offset = 0;
	while(data_offset < len)
	{
		uint32_t page_buf_remain = FW_FLASH_PAGE_SIZE - cache.page_index;
		uint32_t copy_len = ((len - data_offset) < page_buf_remain) ? (len - data_offset) : page_buf_remain;
		//边界保护：单次IAP最多写满一个槽位，超出则作废本次下载，避免写到其它分区
		if(cache.received_size + copy_len > FW_IAP_IMAGE_MAX_SIZE)
		{
			cache.in_progress = 0;
			return FW_IAP_ERR_SIZE;
		}
		//拷贝数据到页缓冲区
		memcpy(cache.page_buf + cache.page_index, data + data_offset, copy_len);
		cache.page_index += copy_len;
		cache.received_size += copy_len;
		data_offset += copy_len;
		//页满时写入Flash
		if(cache.page_index == FW_FLASH_PAGE_SIZE)
		{
			//写入当前页
			if(fw_iap_write_page(cache.write_addr, cache.page_buf, FW_FLASH_PAGE_SIZE, 0) != 0)
			{
				//写入失败：恢复此前状态
				cache.page_index = saved_page_index;
				cache.received_size = saved_received_size;
				cache.write_addr = saved_current_addr;
				return FW_IAP_ERR_WRITE;
			}
			//更新写入地址到下一页
			cache.write_addr += FW_FLASH_PAGE_SIZE;
			cache.page_index = 0;
			//重置页缓冲区
			memset(cache.page_buf, FW_ERASED_VALUE, FW_FLASH_PAGE_SIZE);
		}
	}
	//========== 更新接收进度 ==========
	if(cache.total_size > 0u)
	{
		cache.percent = (uint8_t)((cache.received_size * 100u) / cache.total_size);
	}
	//========== 结束帧处理 ==========
	if(frame_type == 2)
	{
		//页缓冲区仍有未写入的数据
		if(cache.page_index > 0)
		{
			//写入最后一页
			if(fw_iap_write_page(cache.write_addr, cache.page_buf, cache.page_index, 1) != 0)
			{
				cache.page_index = saved_page_index;
				cache.received_size = saved_received_size;
				cache.write_addr = saved_current_addr;
				return FW_IAP_ERR_WRITE;
			}
			cache.write_addr += cache.page_index;
			cache.page_index = 0;
		}
		//========== 验证镜像有效性 ==========
		uint32_t start_addr = cache.write_addr - cache.received_size;
		if(!fw_boot_image_is_valid(start_addr))
		{
			cache.in_progress = 0;//重置状态，允许重试
			return FW_IAP_ERR_INVALID;
		}
		cache.in_progress = 0;
		cache.percent = 100u;//接收完成置100，查询时保持满值
	}
	return cache.received_size;
}

/**
 * @brief   查询IAP接收状态
 * @param   total_size    输出：镜像总大小
 * @param   received_size 输出：已接收字节数
 * @param   percent       输出：接收进度(0~100)
 * @return  FW_IAP_IDLE/FW_IAP_RUNNING/FW_IAP_DONE(指针必须有效)
 */
iap_state_t fw_iap_get_status(uint32_t *total_size, uint32_t *received_size, uint8_t *percent)
{
	*total_size = cache.total_size;
	*received_size = cache.received_size;
	*percent = cache.percent;
	if(cache.in_progress != 0u)
	{
		return FW_IAP_RUNNING;
	}
	if(cache.received_size > 0u && cache.received_size >= cache.total_size)
	{
		return FW_IAP_DONE;
	}
	*percent = 0u;   //空闲：无会话，进度按0上报
	return FW_IAP_IDLE;
}

/**
 * @brief   Modbus CRC16校验计算（逐位计算）
 * @param   crc     初始CRC值（首次调用时为0xFFFF）
 * @param   data    待计算的数据缓冲区指针
 * @param   len     数据长度（字节数）
 * @return  计算后的CRC16校验值
 */
static uint16_t fw_iap_crc16_modbus_update(uint16_t crc, const uint8_t *data, uint32_t len)
{
	for(uint32_t i = 0; i < len; i++)
	{
		crc ^= data[i];
		for(uint8_t j = 0; j < 8; j++)
		{
			if(crc & 1)
			{
				crc = (crc >> 1) ^ 0xA001;
			}
			else
			{
				crc >>= 1;
			}
		}
	}
	return crc;
}

/**
 * @brief   发送IAP文本协议ACK应答（$UP指令格式）
 * @param   recvlen     接收到的数据长度
 */
static void fw_iap_text_send_ack(uint32_t recvlen)
{
	char buf[16];
	int len = snprintf(buf, sizeof(buf), "$UP %06d\r\n", recvlen);
	if(len > 0)
	{
		FW_IAP_SEND(buf, len);
	}
}

/**
 * @brief   按IAP错误码取英文简短提示(文本协议用)
 * @param   ret  fw_iap_process_data()返回的负错误码
 * @return  英文提示字符串
 */
static const char *fw_iap_text_err_msg(int ret)
{
	switch(ret)
	{
		case FW_IAP_ERR_PARAM:     return "BAD FRAME";
		case FW_IAP_ERR_NOT_START: return "NOT STARTED";
		case FW_IAP_ERR_SIZE:      return "IMAGE TOO BIG";
		case FW_IAP_ERR_WRITE:     return "WRITE FAIL";
		case FW_IAP_ERR_INVALID:   return "IMAGE INVALID";
		default:                return "IAP ERROR";
	}
}

/**
 * @brief   发送文本协议的错误提示
 * @param   msg  英文简短提示(不含'$'前缀与换行)
 * @note    文本协议约定：正常收帧返回"$UP 长度"，其余情况返回"$+英文提示"
 */
static void fw_iap_text_send_err(const char *msg)
{
	char buf[40];
	int len = snprintf(buf, sizeof(buf), "$%s\r\n", msg);
	if(len > 0)
	{
		FW_IAP_SEND(buf, len);
	}
}

/**
 * @brief   解析文本格式IAP帧（$UP指令格式）
 * @param   frame   帧数据缓冲区
 * @param   len     帧长度
 * @note    帧格式：$UP OOOOOO TTTTTT CCCCC DATA...
 *          OOOOOO = 偏移位置（6位数字）
 *          TTTTTT = 文件总大小（6位数字）
 *          CCCCC  = CRC16校验值（5位数字）
 *          DATA   = 固件数据（可变长度）
 */
void fw_iap_parse_text_frame(uint8_t *frame, uint32_t len)
{
	//最小帧长度：24字节
	if(len < 24)
	{
		return;
	}
	char value[7] = {0};
	//========== 解析偏移位置（第4-9字节，6位数字）==========
	memcpy(value, frame + 4, 6);
	uint32_t offset = (uint32_t)strtol((char *)value, NULL, 10);
	//========== 解析文件总大小（第11-16字节，6位数字）==========
	memset(value, 0, sizeof(value));
	memcpy(value, frame + 11, 6);
	uint32_t total_size = (uint32_t)strtol((char *)value, NULL, 10);
	//========== 解析文件CRC值（第18-22字节，5位数字）==========
	memset(value, 0, sizeof(value));
	memcpy(value, frame + 18, 5);
	uint16_t file_crc = (uint16_t)strtol((char *)value, NULL, 10);
	//========== 提取数据部分（从第24字节开始）==========
	uint8_t *data = frame + 24;
	uint32_t data_len = len - 24;
	//========== 判断帧类型 ==========
	uint8_t frame_type;
	if(offset == 0)
	{
		frame_type = 0;		//起始帧
	}
	else if (offset + data_len >= total_size)
	{
		frame_type = 2;		//结束帧
	}
	else
	{
		frame_type = 1;		//中间帧
	}
	//========== 调用IAP数据处理 ==========
	int ret = fw_iap_process_data(data, data_len, total_size, frame_type);
	//========== 本帧未写入：返回"$+英文提示"，不返回$UP ==========
	if(ret <= FW_IAP_SUCCESS)
	{
		fw_iap_text_send_err(fw_iap_text_err_msg(ret));
		return;
	}
	//========== 写入成功时才更新CRC ==========
	cache.file_crc = fw_iap_crc16_modbus_update(cache.file_crc, data, data_len);
	//========== 结束帧：整包CRC与升级提交都通过，才算正常更新 ==========
	if(frame_type == 2)
	{
		if(cache.file_crc != file_crc)
		{
			fw_msg_iap_crc_error(cache.file_crc, file_crc);
			fw_iap_text_send_err("CRC ERROR");
			return;
		}
		if(fw_iap_commit_upgrade(cache.received_size) != 0)
		{
			fw_iap_text_send_err("COMMIT FAIL");
			return;
		}
	}
	//========== 本帧正常接收：返回"$UP 已接收长度" ==========
	fw_iap_text_send_ack(ret);
}

/* ==================== HEX协议：应答帧发送 ==================== */

/**
 * @brief   组装并发送一个HEX协议帧(下位机→上位机)
 * @param   type    帧类型(FW_IAP_HEX_TYPE_ACK/NAK)
 * @param   offset  已确认写入到的位置(NAK时为期望位置，上位机据此推进/续传)
 * @param   data    DATA区指针(可为NULL)
 * @param   len     DATA区长度(本函数最多用4字节)
 * @note    CRC16覆盖从SOF到DATA结束，大端发送；协议无序号字段
 */
static void fw_iap_hex_send_frame(uint8_t type, uint32_t offset,
                            const uint8_t *data, uint16_t len)
{
	uint8_t buf[FW_IAP_HEX_HDR_LEN + 4u + FW_IAP_HEX_CRC_LEN];
	uint16_t crc;
	uint32_t i;

	if(len > 4u)
	{
		len = 4u;
	}
	buf[0] = FW_IAP_HEX_SOF0;
	buf[1] = FW_IAP_HEX_SOF1;
	buf[2] = (uint8_t)((FW_IAP_HEX_VER << 4) | (type & 0x0Fu));
	buf[3] = (uint8_t)(offset >> 24);
	buf[4] = (uint8_t)(offset >> 16);
	buf[5] = (uint8_t)(offset >> 8);
	buf[6] = (uint8_t)(offset & 0xFFu);
	buf[7] = (uint8_t)(len >> 8);
	buf[8] = (uint8_t)(len & 0xFFu);
	for(i = 0; i < len; i++)
	{
		buf[FW_IAP_HEX_HDR_LEN + i] = data[i];
	}
	crc = fw_iap_crc16_modbus_update(0xFFFF, buf, FW_IAP_HEX_HDR_LEN + len);
	buf[FW_IAP_HEX_HDR_LEN + len] = (uint8_t)(crc >> 8);
	buf[FW_IAP_HEX_HDR_LEN + len + 1u] = (uint8_t)(crc & 0xFFu);
	FW_IAP_SEND(buf, (uint16_t)(FW_IAP_HEX_HDR_LEN + len + FW_IAP_HEX_CRC_LEN));
}

/**
 * @brief   发送HEX协议ACK(接收并写入成功)
 * @param   offset  已确认写入到的位置
 */
static void fw_iap_hex_send_ack(uint32_t offset)
{
	fw_iap_hex_send_frame(FW_IAP_HEX_TYPE_ACK, offset, NULL, 0u);
}

/**
 * @brief   发送HEX协议NAK(失败，携带状态码与期望位置)
 * @param   offset  期望的写入位置
 * @param   status  NAK状态码(FW_IAP_HEX_ST_xxx)
 */
static void fw_iap_hex_send_nak(uint32_t offset, uint8_t status)
{
	fw_iap_hex_send_frame(FW_IAP_HEX_TYPE_NAK, offset, &status, 1u);
}

/**
 * @brief   把一段镜像数据写进本会话的目标槽位(每页首次写入前擦除一次)
 * @param   offset  该段数据在镜像内的偏移
 * @param   data    数据指针
 * @param   len     数据长度
 * @return  0成功，非0失败
 * @note    不执行整页读-改-写，因此无需页缓冲(不占用RAM/栈，与页大小无关)：
 *          数据按OFFSET递增写入，每页在本次会话首次写入时擦除一次
 *          (是否已擦由写入前沿cache.received_size推算)，之后同页仅追加编程；
 *          页尾未覆盖部分保持擦除态。
 */
static int fw_iap_hex_write_data(uint32_t offset, const uint8_t *data, uint16_t len)
{
	uint32_t written = 0;
	uint32_t frontier = cache.write_addr + cache.received_size;   //本会话已写入到的绝对地址

	while(written < len)
	{
		uint32_t addr = cache.write_addr + offset + written;
		uint32_t page_start = addr & ~((uint32_t)FW_FLASH_PAGE_SIZE - 1u);
		uint32_t n = (uint32_t)FW_FLASH_PAGE_SIZE - (addr - page_start);

		if(n > (uint32_t)(len - written))
		{
			n = (uint32_t)(len - written);
		}
		//本页在本次会话中尚未写入(即未擦除)：先擦除一次(同一页仅擦一次)
		if(page_start >= frontier)
		{
			if(fw_port_flash_erase(page_start, FW_FLASH_PAGE_SIZE) != 0)
			{
				return -1;
			}
		}
		//仅编程本帧实际覆盖的字节，页尾未覆盖部分保持擦除态
		if(fw_port_flash_write(addr, data + written, n) != 0)
		{
			return -1;
		}
		written += n;
	}
	return 0;
}

/**
 * @brief   处理一个已通过CRC校验的HEX协议帧
 * @param   f  完整帧(至少11字节，长度由LEN字段决定)
 * @note    处理规则见fw_iap.h的HEX协议说明
 */
static void fw_iap_hex_handle_frame(const uint8_t *f)
{
	uint8_t  ver_type = f[2];
	uint8_t  type = (uint8_t)(ver_type & 0x0Fu);
	uint32_t offset = ((uint32_t)f[3] << 24) | ((uint32_t)f[4] << 16) |
	                  ((uint32_t)f[5] << 8) | (uint32_t)f[6];
	uint16_t len = (uint16_t)(((uint16_t)f[7] << 8) | f[8]);
	const uint8_t *data = f + FW_IAP_HEX_HDR_LEN;
	uint32_t received = cache.received_size;   //已写入长度=本帧期望的OFFSET

	//版本号不符：帧本身可信(CRC已通过)，返回NAK而非静默丢弃
	if((ver_type >> 4) != FW_IAP_HEX_VER)
	{
		fw_iap_hex_send_nak(received, FW_IAP_HEX_ST_BAD_FRAME);
		return;
	}
	//上位机不应发送ACK/NAK，直接忽略
	if(type == FW_IAP_HEX_TYPE_ACK || type == FW_IAP_HEX_TYPE_NAK)
	{
		return;
	}
	//起始帧：OFFSET=0，DATA为镜像总大小(4字节大端)；开始新一轮会话
	if(type == FW_IAP_HEX_TYPE_START)
	{
		uint32_t total;
		if(len != 4u || offset != 0u)
		{
			fw_iap_hex_send_nak(received, FW_IAP_HEX_ST_BAD_FRAME);
			return;
		}
		total = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
		        ((uint32_t)data[2] << 8) | (uint32_t)data[3];
		if(total == 0u || total > FW_IAP_IMAGE_MAX_SIZE)
		{
			fw_iap_hex_send_nak(0u, FW_IAP_HEX_ST_SIZE);
			return;
		}
		//开始新会话：会话状态在此初始化(与文本协议起始帧一致)
		cache.write_addr = fw_iap_write_target();   //写入目标由升级模式决定
		cache.received_size = 0u;   //已写入字节数(=下一帧期望的OFFSET)
		cache.total_size = total;   //记录总大小，后续每帧据此更新接收进度
		cache.percent = 0u;
		cache.file_crc = 0xFFFFu;   //整包CRC16初值
		cache.in_progress = 1u;
		fw_msg_iap_start(total);
		fw_iap_hex_send_ack(0u);
		return;
	}
	//重传判定(不需要任何额外变量)：
	//  数据帧：帧尾恰好落在写入前沿，说明上位机在重发上一帧，仅重发ACK
	//  结束帧：已提交(会话已结束)且OFFSET==received，返回幂等ACK
	if(received > 0u &&
	   (((type == FW_IAP_HEX_TYPE_DATA) && (offset + (uint32_t)len) == received) ||
	    ((type == FW_IAP_HEX_TYPE_END) && (cache.in_progress == 0u) && (offset == received))))
	{
		fw_iap_hex_send_ack(received);
		return;
	}
	if(!cache.in_progress)
	{
		fw_iap_hex_send_nak(received, FW_IAP_HEX_ST_NOT_STARTED);
		return;
	}
	//OFFSET须与已写入长度严格衔接(以此拒绝丢帧/乱序/粘帧)
	if(offset != received)
	{
		fw_iap_hex_send_nak(received, FW_IAP_HEX_ST_OFFSET_ERROR);
		return;
	}

	if(type == FW_IAP_HEX_TYPE_DATA)
	{
		//长度保护：不得超过起始帧声明的总大小(总大小已校验不超过一个槽位)
		if(((uint32_t)len + received) > cache.total_size)
		{
			fw_iap_hex_send_nak(received, FW_IAP_HEX_ST_SIZE);
			return;
		}
		if(fw_iap_hex_write_data(offset, data, len) != 0)
		{
			//写失败：当前页可能只写了一半，若让上位机重传同一帧会对同一批字节
			//重复编程(同一字在一个擦除周期内只能编程一次)，因此作废本次会话，
			//从OFFSET=0整包重传(擦除进度随会话复位)
			cache.received_size = 0u;   //会话作废：上位机需从起始帧重来
			cache.in_progress = 0u;
			fw_iap_hex_send_nak(0u, FW_IAP_HEX_ST_WRITE_FAIL);
			return;
		}
		//整包文件CRC16边收边累加：无需等END帧再回读Flash
		cache.file_crc = fw_iap_crc16_modbus_update(cache.file_crc, data, len);
		cache.received_size = received + len;
		//实时更新接收进度(总大小由起始帧给出)
		cache.percent = (uint8_t)((cache.received_size * 100u) / cache.total_size);
		fw_iap_hex_send_ack(cache.received_size);
		return;
	}

	if(type == FW_IAP_HEX_TYPE_END)
	{
		//END：DATA=整包文件CRC16(2字节大端)，LEN=2；LEN=0表示不做文件校验
		if(len == 2u)
		{
			uint16_t file_crc = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
			if(file_crc != cache.file_crc)   //与本会话累加值比对
			{
				//整包CRC不符：作废本会话并要求从偏移0整包重传
				cache.received_size = 0u;   //会话作废：上位机需从起始帧重来
				fw_msg_iap_crc_error(cache.file_crc, file_crc);
				cache.in_progress = 0u;
				fw_iap_hex_send_nak(0u, FW_IAP_HEX_ST_CRC_ERROR);
				return;
			}
		}
		else if(len != 0u)
		{
			fw_iap_hex_send_nak(received, FW_IAP_HEX_ST_BAD_FRAME);
			return;
		}
		if(received == 0u || !fw_boot_image_is_valid(cache.write_addr))
		{
			//镜像头无效：同样要求整包重传
			cache.received_size = 0u;   //会话作废：上位机需从起始帧重来
			cache.in_progress = 0u;
			fw_iap_hex_send_nak(0u, FW_IAP_HEX_ST_IMAGE_INVALID);
			return;
		}
		if(fw_iap_commit_upgrade(received) != 0)
		{
			//写boot标志失败：镜像本身有效，但需重传整包后重新提交
			cache.received_size = 0u;   //会话作废：上位机需从起始帧重来
			cache.in_progress = 0u;
			fw_iap_hex_send_nak(0u, FW_IAP_HEX_ST_COMMIT_FAIL);
			return;
		}
		//提交成功：仅清除in_progress，保留received_size用于重复END的幂等应答
		cache.in_progress = 0u;
		cache.percent = 100u;//接收完成后置100
		fw_iap_hex_send_ack(received);
		return;
	}
	//未知类型
	fw_iap_hex_send_nak(received, FW_IAP_HEX_ST_BAD_FRAME);
}

/**
 * @brief   解析HEX协议的字节流（与 fw_iap_parse_text_frame() 对应）
 * @param   buf  本次收到的字节，就地解析(下位机App里就是串口接收缓冲)
 * @param   len  字节数
 * @note    库本身不占用整帧缓冲：在buf中查找SOF，按LEN取整帧并校验CRC16后处理，
 *          粘帧/脏字节/坏帧均可自同步；帧未收全或校验不通过则丢弃，由上位机超时重传
 *          (正常情况下一个串口空闲中断收到一个完整帧)。
 */
void fw_iap_parse_hex_frame(uint8_t *buf, uint32_t len)
{
	uint32_t i;
	uint32_t have;
	uint16_t payload_len;
	uint32_t frame_len;
	uint16_t crc;

	if(buf == NULL || len == 0u)
	{
		return;//参数非法
	}
	have = len;

	for(;;)
	{
		//1.在缓冲中查找帧头SOF
		for(i = 0; (i + 1u) < have; i++)
		{
			if(buf[i] == FW_IAP_HEX_SOF0 && buf[i + 1u] == FW_IAP_HEX_SOF1)
			{
				break;
			}
		}
		if((i + 1u) >= have)
		{
			break;
		}
		if(i > 0u)
		{
			//丢弃SOF之前的无效字节
			have -= i;
			memmove(buf, buf + i, have);
		}
		//2.帧头是否收全
		if(have < FW_IAP_HEX_HDR_LEN)
		{
			break;
		}
		payload_len = (uint16_t)(((uint16_t)buf[7] << 8) | buf[8]);
		if(payload_len > FW_IAP_HEX_MAX_PAYLOAD)
		{
			//长度字段非法：跳过一个字节重新查找SOF
			have--;
			memmove(buf, buf + 1u, have);
			continue;
		}
		frame_len = FW_IAP_HEX_HDR_LEN + (uint32_t)payload_len + FW_IAP_HEX_CRC_LEN;
		if(have < frame_len)
		{
			break;   //本帧未收全：丢弃，由上位机超时重传
		}
		//3.CRC16校验(覆盖SOF..DATA结束)
		crc = fw_iap_crc16_modbus_update(0xFFFF, buf, FW_IAP_HEX_HDR_LEN + (uint32_t)payload_len);
		if(crc != (uint16_t)(((uint16_t)buf[FW_IAP_HEX_HDR_LEN + payload_len] << 8) |
		                     buf[FW_IAP_HEX_HDR_LEN + payload_len + 1u]))
		{
			//坏帧：丢弃并从该SOF之后重新查找
			have--;
			memmove(buf, buf + 1u, have);
			continue;
		}
		//4.处理本帧
		fw_iap_hex_handle_frame(buf);
		//5.移除已处理的帧，继续处理缓冲中剩余字节
		have -= frame_len;
		if(have == 0u)
		{
			break;
		}
		memmove(buf, buf + frame_len, have);
	}
}

#endif

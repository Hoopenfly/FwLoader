/**
* @file    fw_cfg.c
* @brief   fw_kv分区表的实例化(表的内容在fw_cfg.h的FW_KV_PART_TABLE宏里)
* @note    本文件与工程绑定：引用了具体的后端读写擦函数和分区名，
*          通用库fw_kv.c不依赖任何后端。
*/

#include "fw_kv.h"
#include "fw_boot.h"       //FW_BOOT_STAGE_NAME
#include "fw_port.h"     //fw_port_flash_* / fw_port_eeprom_*(分区表挂接)

/* 分区表：内容由 FW_KV_PART_TABLE 决定 */
const fw_kv_part_t fw_kv_part_table[] = FW_KV_PART_TABLE;
const uint32_t     fw_kv_part_count = sizeof(fw_kv_part_table) / sizeof(fw_kv_part_table[0]);

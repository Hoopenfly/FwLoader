#ifndef FW_FWD_H                                    //头文件保护宏，避免重复包含
#define FW_FWD_H                                    //置位头文件保护宏

#include <cstdint>                                  //uint8_t、uint32_t
#include <string>                                   //std::string
#include <vector>                                   //std::vector

#include "fw_delta_codec.h"                         //DeltaPack定义

namespace fw                                        //固件镜像与差分处理命名空间
{

/**
 * @brief   把差分包打包成.fwd文件字节
 * @param   pack  差分包
 * @return  文件字节；失败返回空
 */
std::vector<uint8_t> build_fwd(const DeltaPack &pack);

/**
 * @brief   解析.fwd文件字节
 * @param   data  文件字节
 * @param   pack  输出
 * @return  true成功
 */
bool parse_fwd(const std::vector<uint8_t> &data, DeltaPack &pack);

/**
 * @brief   写.fwd文件
 * @param   path  文件路径
 * @param   pack  差分包
 * @return  true成功
 */
bool write_fwd(const std::string &path, const DeltaPack &pack);

/**
 * @brief   读.fwd文件
 * @param   path  文件路径
 * @param   pack  输出
 * @return  true成功
 */
bool read_fwd(const std::string &path, DeltaPack &pack);

} // namespace fw

#endif // FW_FWD_H

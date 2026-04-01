#ifndef TACO_DECODER_PLUGIN_LOADER_HPP
#define TACO_DECODER_PLUGIN_LOADER_HPP

#include <string>

/**
 * @brief 运行时加载 TACO 厂商注册（可选，Linux 动态库路径）
 *
 * 动态库需导出 C 符号：register_taco_decoder_vendor()
 * （与静态链接 libcomponents 时的自动注册二选一即可；重复注册会覆盖同 kind 工厂）
 */
bool loadTacoDecoderVendorPlugin(const std::string& so_path, std::string& err);

#endif

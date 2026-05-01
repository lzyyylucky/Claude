#pragma once

#include <string>

namespace cct::util {

/** 组件 slug：UTF-8，≤120 字节；禁止路径与控制字符及 Windows 文件名非法字符 */
bool valid_component_name_utf8(const std::string& n);

}

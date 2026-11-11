
#pragma once

#include <filesystem>
#include <string>

// YAML::Node 前向声明，提取声明好这个类，防止报错
namespace YAML
{
    class Node;
}


namespace industrial_gateway::infrastructure::config
{

// ==================================================
// YAML 配置文件加载器
// ==================================================

class YamlConfigLoader final
{
public:

    // 加载 YAML 配置文件
    //
    // file_path: 配置文件路径
    // root:      输出解析后的 YAML 根节点
    // error_message: 加载失败时的错误信息
    //
    // 返回值：
    // true  - 加载成功
    // false - 加载失败
    bool loadFile(
        const std::filesystem::path& file_path,
        YAML::Node& root,
        std::string& error_message
    ) const;

};

} // namespace industrial_gateway::infrastructure::config

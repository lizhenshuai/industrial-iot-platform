
#include "YamlConfigLoader.hpp"

#include <yaml-cpp/yaml.h>

#include <exception>


namespace industrial_gateway::infrastructure::config
{

// ==================================================
// 加载 YAML 配置文件
// ==================================================

bool YamlConfigLoader::loadFile(
    const std::filesystem::path& file_path,
    YAML::Node& root,
    std::string& error_message
) const
{
    // 清理上一次的错误信息
    error_message.clear();

    // 重置输出节点，避免失败时残留上一次的数据
    root = YAML::Node{};

    // 检查文件路径
    if (file_path.empty())
    {
        error_message = "配置文件路径不能为空";
        return false;
    }

    try
    {
        // 读取并解析 YAML 文件
        root = YAML::LoadFile(file_path.string());

        return true;
    }
    catch (const YAML::Exception& e)
    {
        // YAML 文件读取或解析异常
        error_message =
            "加载 YAML 配置文件失败 [" +
            file_path.string() +
            "]: " +
            e.what();

        return false;
    }
    catch (const std::exception& e)
    {
        // 其他标准异常
        error_message =
            "加载配置文件时发生异常 [" +
            file_path.string() +
            "]: " +
            e.what();

        return false;
    }
}

} // namespace industrial_gateway::infrastructure::config

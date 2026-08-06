#include "JsonTool.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <boost/json.hpp>

namespace fs = std::filesystem;

JsonTool::~JsonTool()
{
}

// 从文件读取 JSON，文件不存在时返回空对象
bool JsonTool::ReadJsonFile(const std::string& path, boost::json::value& out_val, std::string* err)
{
    std::ifstream f(path);
    if (!f.is_open())
    {
        // 文件不存在：返回空对象
        out_val = boost::json::object{};
        return true;
    }

    std::stringstream buffer;
    buffer << f.rdbuf();
    f.close();

    std::string content = buffer.str();

    try
    {
        out_val = boost::json::parse(content);
        return true;
    }
    catch (const std::exception& e)
    {
        if (err) *err = std::string("Parse error: ") + e.what();
        return false;
    }
}

// 写入 JSON 到文件，原子写入（先写临时文件再 rename）
bool JsonTool::WriteJsonFile(const std::string& path, const boost::json::value& in_val, std::string* err)
{
    // 确保目录存在
    fs::path dir = fs::path(path).parent_path();
    if (!dir.empty())
    {
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec)
        {
            if (err) *err = std::string("Create directory failed: ") + ec.message();
            return false;
        }
    }

    // 序列化 JSON（紧凑格式）
    std::string json = boost::json::serialize(in_val);

    // 写临时文件，然后 rename 实现原子写入
    std::string tmp_path = path + ".tmp";
    {
        std::ofstream f(tmp_path, std::ios::binary);
        if (!f.is_open())
        {
            if (err) *err = "Open for write failed: " + tmp_path;
            return false;
        }
        f.write(json.data(), json.size());
        if (!f.good())
        {
            if (err) *err = "Write failed";
            f.close();
            fs::remove(tmp_path);
            return false;
        }
        f.close();
    }

    // 原子替换
    std::error_code ec;
    fs::rename(tmp_path, path, ec);
    if (ec)
    {
        if (err) *err = std::string("Rename failed: ") + ec.message();
        fs::remove(tmp_path);
        return false;
    }

    return true;
}
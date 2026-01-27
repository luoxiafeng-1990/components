#include <gtest/gtest.h>
#include <CLI/CLI.hpp> // 包含CLI11头文件
#include <string>

std::string g_input_file = ""; // 全局变量，用于存储从命令行接收的参数值
std::string g_output_file = ""; // 全局变量，用于存储从命令行接收的参数值
std::string g_rtsp_url = ""; // 全局变量，用于存储从命令行接收的参数值
std::string g_pix_fmt = ""; // 全局变量，用于存储从命令行接收的参数值

int main(int argc, char* argv[])
{
    // 初始化Google Test（注意：这里传递的是原始参数，Google Test会处理它自己的参数）
    testing::InitGoogleTest(&argc, argv);

    // 创建CLI11应用实例
    CLI::App app{"Google Test with custom command line parameters"};
    
    // 添加自定义命令行选项
    app.add_option("--input_file", g_input_file, "Sets the string for the test")
        ->default_val(""); // 设置默认值
    app.add_option("--output_file", g_output_file, "Sets the string for the test")
        ->default_val(""); // 设置默认值
    app.add_option("--rtsp_url", g_rtsp_url, "Sets the string for the test")
        ->default_val(""); // 设置默认值
    app.add_option("--pix_fmt", g_pix_fmt, "Sets the pix_fmt for the test")
        ->default_val(""); // 设置默认值

    try {
        // 先解析自定义参数
        app.parse(argc, argv);
    } catch (const CLI::ParseError &e) {
        // 如果解析出错，让CLI11处理退出逻辑（包括帮助信息显示）
        return app.exit(e);
    }
    
    return RUN_ALL_TESTS();
}
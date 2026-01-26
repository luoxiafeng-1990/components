#include <gtest/gtest.h>
#include <vector>
#include <tuple>
#include <string>
#include <iostream>

// 全局期望值变量（根据你的需求定义）
std::string g_expected_value = "expected_result";

// 被测函数
static std::string str_join(int a, const std::string& b, bool c, int d, const std::string& e) {
    return std::to_string(a) + b + (c ? "true" : "false") + std::to_string(d) + e;
}

// ==================== 字符串连接测试：使用 Combine 生成器 ====================
class StringJoin : public testing::TestWithParam<std::tuple<int, std::string, bool, int, std::string>> {
protected:
    static void SetUpTestSuite() {
        // TestSuite级前置步骤 - 整个测试套件执行一次
        std::cout << "StringJoin TestSuite 开始" << std::endl;
    }
    
    static void TearDownTestSuite() {
        // TestSuite级后置步骤 - 整个测试套件执行一次  
        std::cout << "StringJoin TestSuite 结束" << std::endl;
    }
    
    void SetUp() override {
        // Case级前置步骤 - 每个测试用例执行前都会调用
        const testing::TestInfo* const test_info = testing::UnitTest::GetInstance()->current_test_info();
        std::cout << test_info->name() << "开始" << std::endl;
    }
    
    void TearDown() override {
        // Case级后置步骤 - 每个测试用例执行后都会调用
        const testing::TestInfo* const test_info = testing::UnitTest::GetInstance()->current_test_info();
        std::cout << test_info->name() << "结束" << std::endl;
    }
};

TEST_P(StringJoin, HandlesVariousInputs) {
    // 使用结构化绑定解包参数：现在有6个参数（5个输入+1个期望结果）
    auto [a, b, c, d, e] = GetParam();
    
    std::string result = str_join(a, b, c, d, e);
    
    // 你可以选择使用全局变量或参数中的期望值
    EXPECT_EQ(result, g_expected_value);  // 使用参数中的期望值
    // 或者使用全局变量：EXPECT_EQ(result, g_expected_value);
}

std::vector<int> DemoGenerateTestData(){
    return {5,6};
}

// 使用 Combine 生成器创建参数组合
INSTANTIATE_TEST_SUITE_P(
    StringTest,           // 测试实例前缀
    StringJoin,          // 测试类名
    testing::Combine(     // 组合生成器[1,2](@ref)
        testing::Range(1, 4, 1),                    // 整数参数：1, 2, 3[2,4](@ref)
        testing::Values("Hello", "World", "Test"),   // 字符串参数[1,2](@ref)
        testing::Bool(),                            // 布尔参数[2,4](@ref)
        testing::ValuesIn(DemoGenerateTestData()),               // 第四个参数：整数值
        testing::Values("ABC", "BCD", "CDE", "Additional")  // 第五个参数：字符串[2](@ref)
        // 注意：期望结果需要与参数组合对应，这里简化处理
    )
);
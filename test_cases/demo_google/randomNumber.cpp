#include <gtest/gtest.h>
#include <random>

class RandomNumberTest : public testing::Test {

protected:
    // 声明随机数生成所需的组件
    std::random_device rd; // 用于获取随机种子
    std::mt19937 gen;      // 随机数生成引擎
    std::uniform_int_distribution<int> dist; // 均匀整数分布

    void SetUp() override {
        // 在SetUp中初始化随机数生成器，指定范围[0, 10]
        gen = std::mt19937(rd()); // 用随机设备初始化生成器
        dist = std::uniform_int_distribution<int>(0, 10); // 设置分布范围
    }
    void TearDown() override {
        std::cout << "[Teardown] RandomNumberTest" << std::endl;
    }

    int getRandomNumber(){
        return dist(gen);
    }
    
};
    
TEST_F(RandomNumberTest, LowerRange) {
    EXPECT_TRUE(0 < getRandomNumber() < 6);
}

TEST_F(RandomNumberTest, UpperRange) {
    EXPECT_TRUE(5 < getRandomNumber() < 10);
}
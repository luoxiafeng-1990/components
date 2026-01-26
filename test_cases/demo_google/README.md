## TestCase目录结果

- test_cases
    - Dir: Ffmpeg
        - Cpp: Decode_mp4
        - Cpp: Encode_mp4
            - Class: EncodeRgb
            - Class: EncodeYuv
    - Dir: Opencv

## GoogleTest 框架简介

## 注册TestCase（大驼峰命名，名称唯一）
```bash
Test(TestSuite1, TestCase1){函数体}
Test(TestSuite2, TestCase1){函数体}
Test(TestSuite2, TestCase2){函数体}
```

## 测试例之间不要有顺序！不要有前后依赖！不允许并行执行！

## 分组运行
```bash
运行TestSuite1下面所有测试用例
./demo_google --gtest_filter=TestSuite1.*
运行全部测试用例，但排除TestCase2
./demo_google --gtest_filter=*-*.TestCase2
```

## 输出TestCase列表
```bash
打印全部测试用例
./demo_google --gtest_list_tests
打印部分测试用例
./demo_google --gtest_list_tests --gtest_filter=*.AddTest
```

## 官方文档
英文版: https://google.github.io/googletest/

中文版: https://ggdocs.cn/googletest/reference/testing.html

## 高级功能
断言;setup(),teardown();参数化测试;Mock;分布式测试;事件监听器;死亡测试
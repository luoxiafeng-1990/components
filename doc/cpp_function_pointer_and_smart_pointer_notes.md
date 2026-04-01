# C/C++ 函数指针与智能指针学习笔记

## 问题背景

在 `DisplayPlugin` 重构过程中，使用了 **成员函数指针** 作为分发表的值类型：

```cpp
using ExtBuilder = std::unique_ptr<IDisplayVendorExtension>(DisplayPlugin::*)() const;
```

以下是对该语法的完整拆解和学习记录。

---

## 一、C 语言函数指针

### 正确写法

```c
int (*f)(void);
```

- `(*f)` 表示 f 是一个指针
- `int` 是返回值类型
- `(void)` 是参数列表

### 常见错误写法

```c
int (f*)(void);  // ✗ 星号位置不对
```

### 用 typedef / using 取别名

```c
// C 风格
typedef int (*Func)(void);

// C++ 风格（等价，更清晰）
using Func = int(*)(void);

// 使用
Func f = &some_function;
int result = f();
```

---

## 二、C++ 成员函数指针

普通函数指针用 `*`，成员函数指针用 `ClassName::*`：

```
C 函数指针:       int (*      )(void)     "指向某个函数"
C++ 成员函数指针:  int (Dog::* )() const   "指向 Dog 类的某个成员函数"
```

### 示例

```cpp
class Dog {
public:
    int age() const { return age_; }
    int age_ = 5;
};

using Getter = int(Dog::*)() const;

Getter g = &Dog::age;
Dog d;
int a = (d.*g)();  // 等价于 d.age()，a = 5
```

### 返回值为 string 的情况

```cpp
class Dog {
public:
    std::string name() const { return name_; }
    std::string name_ = "Buddy";
};

using Getter = std::string(Dog::*)() const;

Getter g = &Dog::name;
Dog d;
std::string n = (d.*g)();  // n = "Buddy"
```

---

## 三、返回值为 unique_ptr 的成员函数指针

当返回值从 `int` 换成 `std::unique_ptr<T>`，语法结构不变：

```
int                                     (Dog::*)          () const  ← 返回 int
std::string                             (Dog::*)          () const  ← 返回 string
std::unique_ptr<IDisplayVendorExtension> (DisplayPlugin::*) () const  ← 返回 unique_ptr
```

**关键理解：`std::unique_ptr<IDisplayVendorExtension>` 在这里只是返回值类型，和 int、string 的位置一模一样，跟指针机制完全无关。**

真正的「指针」是 `(DisplayPlugin::*)` 这部分——它是 C++ 成员函数指针的固定写法。

---

## 四、std::unique_ptr 智能指针本身的用法

`unique_ptr` 是 C++11 引入的智能指针，核心特点是**独占所有权**：

```cpp
// 创建
std::unique_ptr<Dog> p1 = std::make_unique<Dog>("Buddy");

// 使用（和普通指针一样）
p1->bark();
std::string name = p1->name();

// 不能复制（独占所有权）
std::unique_ptr<Dog> p2 = p1;            // ✗ 编译错误

// 可以转移所有权
std::unique_ptr<Dog> p2 = std::move(p1); // ✓ p1 变成空，p2 接管

// 离开作用域自动释放，不需要手动 delete
```

---

## 五、项目中的实际应用

### DisplayPlugin 中的分发表模式

```cpp
// 类型别名：成员函数指针，返回 unique_ptr
using ExtBuilder = std::unique_ptr<IDisplayVendorExtension>(DisplayPlugin::*)() const;

// 静态分发表
static const std::unordered_map<std::string, ExtBuilder> map = {
    {"tacopro", &DisplayPlugin::buildTacoProExtension},
    {"taco",    &DisplayPlugin::buildTacoExtension},
};

// 调用
config.consumer_type.display.vendor = (this->*(it->second))();
```

### 调用语法解释

`(this->*(it->second))()` 拆解：

1. `it->second` — 从 map 取出函数指针
2. `this->*` — 把函数指针绑定到当前对象
3. `()` — 执行调用

等价于 `this->buildTacoProExtension()`，但具体调哪个函数由 map 查表在运行时决定。

---

## 六、总结

| 概念 | 说明 |
|------|------|
| `int (*f)(void)` | C 语言普通函数指针 |
| `int (Dog::*g)() const` | C++ 成员函数指针 |
| `using ExtBuilder = ...` | 给成员函数指针类型取别名 |
| `unique_ptr<T>` | 独占所有权的智能指针，自动管理堆内存 |
| 分发表模式 | 用 map 存储函数指针，运行时查表调用，取代 if-else |

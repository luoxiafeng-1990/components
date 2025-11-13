#ifndef VIDEO_READER_FACTORY_HPP
#define VIDEO_READER_FACTORY_HPP

#include "IVideoReader.hpp"
#include <memory>

/**
 * VideoReaderFactory - 视频读取器工厂类
 * 
 * 设计模式：工厂模式（Factory Pattern）
 * 
 * 职责：
 * - 根据环境和配置创建合适的 IVideoReader 实现
 * - 封装对象创建逻辑
 * - 支持自动检测和手动指定两种模式
 * 
 * 优势：
 * - 用户无需了解具体实现类
 * - 可以根据运行环境自动选择最优实现
 * - 支持通过配置、环境变量控制
 */
class VideoReaderFactory {
public:
    /**
     * 读取器类型枚举
     */
    enum class ReaderType {
        AUTO,          // 自动检测（默认）
        MMAP,          // 强制使用 mmap 实现
        IOURING,       // 强制使用 io_uring 实现
        DIRECT_READ,   // 强制使用普通 read 实现（暂未实现）
        RTSP           // RTSP 视频流解码器
    };
    
    /**
     * 创建视频读取器（工厂方法）
     * 
     * 创建策略（优先级从高到低）：
     * 1. 用户显式指定 (type != AUTO)
     * 2. 环境变量 (VIDEO_READER_TYPE)
     * 3. 配置文件 (/etc/video_reader.conf)
     * 4. 自动检测系统能力
     * 
     * @param type 读取器类型（默认AUTO）
     * @return 视频读取器实例（智能指针）
     */
    static std::unique_ptr<IVideoReader> create(ReaderType type = ReaderType::RTSP);
    
    /**
     * 从名称创建读取器
     * @param name 类型名称（"mmap", "iouring", "auto"）
     * @return 视频读取器实例
     */
    static std::unique_ptr<IVideoReader> createByName(const char* name);
    
    /**
     * 检查 io_uring 是否可用
     * @return 可用返回true
     */
    static bool isIoUringAvailable();
    
    /**
     * 检查 mmap 是否可用
     * @return 可用返回true
     */
    static bool isMmapAvailable();
    
    /**
     * 获取推荐的读取器类型
     * @return 推荐类型
     */
    static ReaderType getRecommendedType();
    
    /**
     * 将类型转换为字符串
     * @param type 类型
     * @return 类型名称
     */
    static const char* typeToString(ReaderType type);

private:
    /**
     * 自动检测并创建最优读取器
     */
    static std::unique_ptr<IVideoReader> autoDetect();
    
    /**
     * 根据类型创建读取器
     */
    static std::unique_ptr<IVideoReader> createByType(ReaderType type);
    
    /**
     * 从环境变量读取类型
     */
    static ReaderType getTypeFromEnvironment();
    
    /**
     * 从配置文件读取类型
     */
    static ReaderType getTypeFromConfig();
    
    /**
     * 检查 io_uring 是否适合当前场景
     */
    static bool isIoUringSuitable();
};

#endif // VIDEO_READER_FACTORY_HPP





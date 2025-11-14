#ifndef DECODER_FACTORY_HPP
#define DECODER_FACTORY_HPP

#include "IDecoder.hpp"
#include <memory>

/**
 * DecoderFactory - 解码器工厂类
 * 
 * 负责根据类型创建具体的解码器实例
 * 
 * 设计模式：工厂模式（Factory Pattern）
 * - 封装对象创建逻辑
 * - 客户端通过类型参数获取解码器实例
 * - 解耦创建逻辑和使用逻辑
 * 
 * 优势：
 * - 统一创建入口
 * - 易于扩展新类型
 * - 降低客户端复杂度
 * 
 * 参考：Chromium VideoDecoderFactory、FFmpeg、Android MediaCodec
 */
class DecoderFactory {
public:
    /**
     * DecoderType - 解码器类型枚举
     */
    enum class DecoderType {
        AUTO,           // 自动选择（优先硬件，回退软件）
        FFMPEG,         // FFmpeg软件解码
        HARDWARE,       // 通用硬件加速解码
        VAAPI,          // VA-API硬件解码（Intel/AMD）
        NVDEC,          // NVIDIA硬件解码
        VIDEOTOOLBOX,   // Apple VideoToolbox（macOS/iOS）
        CUSTOM          // 自定义解码器
    };
    
    /**
     * 创建解码器实例
     * @param type 解码器类型
     * @return 解码器实例指针（unique_ptr），创建失败返回nullptr
     */
    static std::unique_ptr<IDecoder> createDecoder(DecoderType type);
    
    /**
     * 根据名称创建解码器
     * @param type_name 解码器类型名称（如 "ffmpeg", "vaapi", "nvdec"）
     * @return 解码器实例指针，创建失败返回nullptr
     * 
     * 说明：
     * - 字符串匹配不区分大小写
     * - 方便配置文件或命令行参数使用
     */
    static std::unique_ptr<IDecoder> createDecoderByName(const char* type_name);
    
    /**
     * 检查指定类型的解码器是否可用
     * @param type 解码器类型
     * @return 可用返回true
     */
    static bool isDecoderAvailable(DecoderType type);
    
    /**
     * 获取解码器类型的名称
     * @param type 解码器类型
     * @return 类型名称字符串
     */
    static const char* getDecoderTypeName(DecoderType type);
    
    /**
     * 获取推荐的解码器类型（根据当前平台）
     * @return 推荐的解码器类型
     */
    static DecoderType getRecommendedDecoderType();

private:
    // 工厂类不可实例化
    DecoderFactory() = delete;
    ~DecoderFactory() = delete;
    DecoderFactory(const DecoderFactory&) = delete;
    DecoderFactory& operator=(const DecoderFactory&) = delete;
};

#endif // DECODER_FACTORY_HPP

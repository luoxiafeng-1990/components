#ifndef IVIDEO_FILE_NAVIGATOR_HPP
#define IVIDEO_FILE_NAVIGATOR_HPP

/**
 * @brief IVideoFileNavigator - 视频文件导航接口
 * 
 * 架构角色：文件操作接口 - 负责所有文件相关操作
 * 
 * 职责：
 * - 文件打开/关闭操作（open、openRaw、close、isOpen）
 * - 文件导航操作（seek、skip等）
 * - 文件状态查询（getTotalFrames、getCurrentFrameIndex等）
 * 
 * 设计模式：接口分离原则（ISP - Interface Segregation Principle）
 * - 将所有文件操作功能从IBufferFillingWorker中分离
 * - Worker可以同时实现IBufferFillingWorker和IVideoFileNavigator
 * - 通过继承IVideoFileNavigator，明确表达Worker的文件操作职责
 * 
 * 优势：
 * - 职责清晰：所有文件操作功能独立为独立接口
 * - 符合ISP：需要文件操作时依赖IVideoFileNavigator，需要填充Buffer时依赖IBufferFillingWorker
 * - 可扩展：未来可以独立扩展文件操作功能
 * - 文档明确：通过接口名称明确表达职责
 */
class IVideoFileNavigator {
public:
    virtual ~IVideoFileNavigator() = default;
    
    // ============ 文件打开/关闭操作 ============
    
    /**
     * 打开编码视频文件（自动检测格式）
     * @param path 文件路径
     * @return 成功返回true
     */
    virtual bool open(const char* path) = 0;
    
    /**
     * 打开原始视频文件（需手动指定格式）
     * @param path 文件路径
     * @param width 视频宽度（像素）
     * @param height 视频高度（像素）
     * @param bits_per_pixel 每像素位数
     * @return 成功返回true
     */
    virtual bool openRaw(const char* path, int width, int height, int bits_per_pixel) = 0;
    
    /**
     * 关闭视频文件
     */
    virtual void close() = 0;
    
    /**
     * 检查文件是否已打开
     */
    virtual bool isOpen() const = 0;
    
    // ============ 文件导航操作 ============
    
    /**
     * 跳转到指定帧
     * @param frame_index 帧索引
     * @return 成功返回true
     */
    virtual bool seek(int frame_index) = 0;
    
    /**
     * 回到文件开头
     */
    virtual bool seekToBegin() = 0;
    
    /**
     * 跳转到文件末尾
     */
    virtual bool seekToEnd() = 0;
    
    /**
     * 跳过N帧（可正可负）
     * @param frame_count 跳过的帧数
     * @return 成功返回true
     */
    virtual bool skip(int frame_count) = 0;
    
    // ============ 文件状态查询 ============
    
    /**
     * 获取总帧数
     */
    virtual int getTotalFrames() const = 0;
    
    /**
     * 获取当前帧索引
     */
    virtual int getCurrentFrameIndex() const = 0;
    
    /**
     * 获取单帧大小（字节）
     */
    virtual size_t getFrameSize() const = 0;
    
    /**
     * 获取文件大小（字节）
     */
    virtual long getFileSize() const = 0;
    
    /**
     * 获取视频宽度
     */
    virtual int getWidth() const = 0;
    
    /**
     * 获取视频高度
     */
    virtual int getHeight() const = 0;
    
    /**
     * 获取每像素字节数
     */
    virtual int getBytesPerPixel() const = 0;
    
    /**
     * 获取文件路径
     */
    virtual const char* getPath() const = 0;
    
    /**
     * 检查是否还有更多帧
     */
    virtual bool hasMoreFrames() const = 0;
    
    /**
     * 检查是否到达文件末尾
     */
    virtual bool isAtEnd() const = 0;
};

#endif // IVIDEO_FILE_NAVIGATOR_HPP


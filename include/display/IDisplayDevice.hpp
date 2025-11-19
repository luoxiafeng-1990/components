  #ifndef IDISPLAYDEVICE_HPP
#define IDISPLAYDEVICE_HPP

#include "../buffer/Buffer.hpp"
#include <stddef.h>

// 前向声明
class BufferPool;

/**
 * IDisplayDevice - 显示设备抽象接口
 * 
 * 定义跨平台的显示设备接口，任何平台的显示实现都需要继承此接口。
 * 
 * 平台实现示例：
 * - Linux Framebuffer: LinuxFramebufferDevice
 * - Windows DirectX: WindowsD3DDisplayDevice
 * - X11: X11DisplayDevice
 * 
 * 核心功能：
 * - 设备初始化和清理
 * - 获取显示属性（分辨率、buffer数量等）
 * - Buffer访问和管理
 * - 显示控制（切换buffer、垂直同步）
 */
class IDisplayDevice {
public:
    virtual ~IDisplayDevice() = default;
    
    // ============ 设备发现 ============
    
    /**
     * 查找显示设备节点
     * 
     * 将逻辑设备索引映射到实际的系统设备路径或标识符。
     * 这是跨平台的通用需求，不同平台有不同的实现方式。
     * 
     * 平台实现示例：
     * - Linux FB: 读取 /proc/fb，将 tpsfb0/tpsfb1 映射到 /dev/fbX
     * - Windows: 查找显示器的设备路径 (\\\\.\\DISPLAY1)
     * - Android: 查找 SurfaceFlinger 的显示设备ID
     * - X11: 查找 DISPLAY 环境变量和屏幕编号
     * 
     * @param device_index 逻辑设备索引（通常0表示主显示，1表示副显示）
     * @return 设备节点路径或标识符，失败返回NULL
     */
    virtual const char* findDeviceNode(int device_index) = 0;
    
    // ============ 生命周期管理 ============
    
    /**
     * 初始化显示设备
     * @param device_index 设备索引（如fb0=0, fb1=1）
     * @return 成功返回true
     */
    virtual bool initialize(int device_index) = 0;
    
    /**
     * 清理并释放显示设备资源
     */
    virtual void cleanup() = 0;
    
    // ============ 显示属性查询 ============
    
    /**
     * 获取显示宽度（像素）
     */
    virtual int getWidth() const = 0;
    
    /**
     * 获取显示高度（像素）
     */
    virtual int getHeight() const = 0;
    
    /**
     * 获取每像素字节数（向上取整）
     * 例如：ARGB888 = 4字节, RGB888 = 3字节
     * 注意：对于非整数字节的格式（如12bit），返回向上取整的值
     */
    virtual int getBytesPerPixel() const = 0;
    
    /**
     * 获取每像素位数
     * 例如：ARGB888 = 32位, RGB888 = 24位, 12bit color = 12位
     */
    virtual int getBitsPerPixel() const = 0;
    
    /**
     * 获取Buffer数量
     * 用于多缓冲显示（双缓冲、三缓冲等）
     */
    virtual int getBufferCount() const = 0;
    
    /**
     * 获取单个Buffer的大小（字节）
     */
    virtual size_t getBufferSize() const = 0;
    
    // ============ 显示控制 ============
    
    /**
     * 切换当前显示的Buffer（方案A：通过Buffer对象）
     * 
     * 告诉显示系统从指定的Buffer读取数据进行显示。
     * 
     * 实现方式（平台相关）：
     * - Linux FB: 通过ioctl设置yoffset（从Buffer的ID获取buffer_index）
     * - Windows: Present()调用
     * - OpenGL: SwapBuffers()
     * 
     * @param buffer 要显示的Buffer对象（必须属于当前设备的BufferPool）
     * @return 成功返回true
     * 
     * @note 通过Buffer对象可以知道它属于哪个BufferPool，更直接
     */
    virtual bool displayBuffer(Buffer* buffer) = 0;
    
    /**
     * 切换当前显示的Buffer（方案B：通过BufferPool和索引）
     * 
     * 告诉显示系统从指定的BufferPool的指定索引Buffer读取数据进行显示。
     * 
     * 实现方式（平台相关）：
     * - Linux FB: 通过ioctl设置yoffset
     * - Windows: Present()调用
     * - OpenGL: SwapBuffers()
     * 
     * @param pool 要显示的Buffer所属的BufferPool
     * @param buffer_index 要显示的Buffer索引（在指定BufferPool中的索引）
     * @return 成功返回true
     * 
     * @note 明确指定BufferPool和索引，适用于需要明确指定BufferPool的场景
     */
    virtual bool displayBuffer(BufferPool* pool, int buffer_index) = 0;
    
    /**
     * 等待垂直同步信号（VSync）
     * 
     * 确保Buffer切换在垂直消隐期进行，避免画面撕裂。
     * 
     * 实现方式（平台相关）：
     * - Linux FB: ioctl(FBIO_WAITFORVSYNC)
     * - Windows: DwmFlush()
     * - OpenGL: glfwSwapInterval()
     * 
     * @return 成功返回true
     */
    virtual bool waitVerticalSync() = 0;
    
    /**
     * 获取当前正在显示的Buffer索引
     */
    virtual int getCurrentDisplayBuffer() const = 0;
};

#endif // IDISPLAYDEVICE_HPP


/**
 * @file DataSourceOptions.hpp
 * @brief DataSource 横切选项辅助类
 *
 * DataSourceConfig 中用户可设置的参数（buffer_count 等）在此统一注册，
 * 各插件在 registerOptions() 中调用 DataSourceOptions::registerTo() 即可，
 * 无需重复编写。
 *
 * 使用方式：
 * @code
 * class VdecPlugin : public IOptionPlugin {
 *     DataSourceOptions ds_opts_;           // 成员变量
 *
 *     void registerOptions(CLI::App& app) override {
 *         ds_opts_.registerTo(app);         // 一行注册 DataSource 选项
 *         // ... 插件自身选项 ...
 *     }
 *
 *     void applyTo(WorkerConfig& config) const override {
 *         ds_opts_.applyTo(config);         // 一行应用 DataSource 设置
 *         // ... 插件自身设置 ...
 *     }
 * };
 * @endcode
 */

#ifndef DATA_SOURCE_OPTIONS_HPP
#define DATA_SOURCE_OPTIONS_HPP

#include "productionline/worker/config/ConfigBuilders.hpp"
#include "productionline/worker/config/MultiWorkerConfig.hpp"
#include "productionline/worker/config/WorkerConfigs.hpp"
#include "third_party/CLI11.hpp"

namespace test {

struct DataSourceOptions {
    int buffer_count = 0;  ///< BufferPool Buffer 数量（0=使用工厂默认值）

    void registerTo(CLI::App& app) {
        app.add_option("-b,--buffer-count", buffer_count,
                       "BufferPool Buffer数量 (0=自动)");
    }

    void applyTo(WorkerConfig& config) const {
        config.data_source = DataSourceConfigBuilder(config.data_source)
            .setBufferCountIfNonZero(buffer_count)
            .build();
    }
};

} // namespace test

#endif // DATA_SOURCE_OPTIONS_HPP

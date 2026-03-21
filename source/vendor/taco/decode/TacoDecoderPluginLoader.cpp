#include "vendor/taco/decode/TacoDecoderPluginLoader.hpp"

#if defined(__linux__)
#include <dlfcn.h>
#endif

bool loadTacoDecoderVendorPlugin(const std::string& so_path, std::string& err) {
#if defined(__linux__)
    void* handle = dlopen(so_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char* e = dlerror();
        err = e ? e : "dlopen failed";
        return false;
    }
    using RegisterFn = void (*)();
    auto fn = reinterpret_cast<RegisterFn>(dlsym(handle, "register_taco_decoder_vendor"));
    if (!fn) {
        err = "dlsym(register_taco_decoder_vendor) failed";
        return false;
    }
    fn();
    return true;
#else
    (void)so_path;
    err = "loadTacoDecoderVendorPlugin: only implemented on Linux";
    return false;
#endif
}

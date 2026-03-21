#include "vendor/taco/decode/TacoDecoderExtension.hpp"
#include "vendor/contracts/DecoderVendorRegistry.hpp"

std::unique_ptr<IDecoderVendorExtension> TacoDecoderExtension::clone() const {
    return makeTacoDecoderExtension(config);
}

bool TacoDecoderExtension::validate(std::string& err) const {
    if (config.ch0_yuv_format < -1) {
        err = "TacoConfig: ch0_yuv_format < -1";
        return false;
    }
    if (config.ch1_yuv_format < -1) {
        err = "TacoConfig: ch1_yuv_format < -1";
        return false;
    }
    return true;
}

TacoDecoderExtension* tacoVendorExtension(WorkerConfig::DecoderConfig& d) {
    if (!d.vendor) {
        return nullptr;
    }
    if (std::strcmp(d.vendor->kind(), "taco") != 0) {
        return nullptr;
    }
    return static_cast<TacoDecoderExtension*>(d.vendor.get());
}

const TacoDecoderExtension* tacoVendorExtension(const WorkerConfig::DecoderConfig& d) {
    if (!d.vendor) {
        return nullptr;
    }
    if (std::strcmp(d.vendor->kind(), "taco") != 0) {
        return nullptr;
    }
    return static_cast<const TacoDecoderExtension*>(d.vendor.get());
}

TacoConfig* tacoDecoderConfig(WorkerConfig::DecoderConfig& d) {
    TacoDecoderExtension* e = tacoVendorExtension(d);
    return e ? &e->config : nullptr;
}

const TacoConfig* tacoDecoderConfig(const WorkerConfig::DecoderConfig& d) {
    const TacoDecoderExtension* e = tacoVendorExtension(d);
    return e ? &e->config : nullptr;
}

std::unique_ptr<TacoDecoderExtension> makeTacoDecoderExtension(const TacoConfig& cfg) {
    auto p = std::make_unique<TacoDecoderExtension>();
    p->config = cfg;
    return p;
}

static void register_taco_decoder_vendor_impl() {
    DecoderVendorRegistry::instance().registerFactory("taco", []() {
        return std::make_unique<TacoDecoderExtension>();
    });
}

namespace {

struct TacoVendorRegistrar {
    TacoVendorRegistrar() { register_taco_decoder_vendor_impl(); }
};

static TacoVendorRegistrar g_taco_vendor_registrar;

}  // namespace

extern "C" void register_taco_decoder_vendor() {
    register_taco_decoder_vendor_impl();
}

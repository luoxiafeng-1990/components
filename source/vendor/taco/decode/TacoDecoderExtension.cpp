#include "vendor/taco/decode/TacoDecoderExtension.hpp"
#include "vendor/contracts/DecoderVendorRegistry.hpp"

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

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

double TacoDecoderExtension::getChannelBytesPerPixel(
    int channel, void* priv_data, int pix_fmt) const
{
    int64_t value = 0;

    if (channel == 0) {
        if (av_opt_get_int(priv_data, "ch0_enable", 0, &value) < 0 || value == 0) {
            return 0.0;
        }
        if (pix_fmt >= 0) {
            const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(
                static_cast<AVPixelFormat>(pix_fmt));
            if (desc) {
                return av_get_bits_per_pixel(desc) / 8.0;
            }
        }
        return 1.5;
    }

    if (channel == 1) {
        if (av_opt_get_int(priv_data, "ch1_enable", 0, &value) < 0 || value == 0) {
            return 0.0;
        }
        if (av_opt_get_int(priv_data, "ch1_rgb", 0, &value) < 0 || value == 0) {
            return 1.5;
        }
        int64_t rgb_format = 0;
        if (av_opt_get_int(priv_data, "ch1_rgb_format", 0, &rgb_format) < 0) {
            return 4.0;
        }
        OutputFormat format = mapRgbDriverValueToOutputFormat(static_cast<int>(rgb_format));
        return getBytesPerPixelFromOutputFormat(format);
    }

    return 0.0;
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
    DecoderVendorRegistry::instance().registerVendor("taco", []() {
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

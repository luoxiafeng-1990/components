#include "vendor/taco/encode/TacoEncoderExtension.hpp"

extern "C" {
#include <libavutil/opt.h>
}

bool TacoEncoderExtension::applyToCodecContext(void* priv_data) const {
    if (!priv_data) return false;

    if (profile > 0) {
        av_opt_set_int(priv_data, "profile", profile, 0);
    }
    if (level > 0) {
        av_opt_set_int(priv_data, "level", level, 0);
    }
    return true;
}

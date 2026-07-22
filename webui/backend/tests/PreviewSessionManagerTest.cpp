#include "PreviewSessionManager.hpp"
#include <cassert>
#include <iostream>

using webui::PreviewSessionManager;
using webui::PreviewSessionConfig;
using webui::PreviewSessionState;
using webui::PreviewJpegSnapshot;

static void test_start_stop_idempotent() {
    PreviewSessionManager mgr;
    int creates = 0, destroys = 0;
    mgr.setEncoderFactory([&](const std::string&, const PreviewSessionConfig&) {
        ++creates;
        return PreviewSessionManager::makeNullEncoder();
    }, [&](auto&) { ++destroys; });

    PreviewSessionConfig cfg{15, 80, "jpeg_taco"};
    auto r1 = mgr.startSession("wk-1", cfg);
    assert(r1.ok);
    assert(creates == 1);
    auto r2 = mgr.startSession("wk-1", cfg);
    assert(r2.ok);
    assert(creates == 1);
    assert(mgr.activeEncoderCount() == 1);

    PreviewSessionConfig other{30, 80, "jpeg_taco"};
    auto conflict = mgr.startSession("wk-1", other);
    assert(!conflict.ok);
    assert(conflict.http_status == 409);

    assert(mgr.stopSession(r1.session_id).ok);
    assert(destroys == 0);
    assert(mgr.stopSession(r2.session_id).ok);
    assert(destroys == 1);
    assert(mgr.stopSession(r1.session_id).ok);
    assert(destroys == 1);
}

static void test_has_active_fast_path() {
    PreviewSessionManager mgr;
    assert(!mgr.hasActiveSession("wk-x"));

    mgr.setEncoderFactory(
        [](const std::string&, const PreviewSessionConfig&) {
            return PreviewSessionManager::makeNullEncoder();
        },
        [](auto& h) { h.reset(); });

    PreviewSessionConfig cfg{15, 80, "jpeg_taco"};
    auto r = mgr.startSession("wk-1", cfg);
    assert(r.ok);
    assert(mgr.hasActiveSession("wk-1"));
    // FrameTap idle contract: hasActiveSession is the only gate before offerFrame.
    assert(mgr.offerFrame("wk-1", nullptr) == false);
    assert(mgr.offerFrame("wk-missing", nullptr) == false);

    PreviewJpegSnapshot snap;
    assert(!mgr.waitLatestJpeg("wk-1", 0, snap, 10));

    assert(mgr.stopSession(r.session_id).ok);
    assert(!mgr.hasActiveSession("wk-1"));
}

static void test_jpeg_taco_factory_host_null() {
    PreviewSessionManager mgr;
    mgr.setEncoderFactory(
        [](const std::string& wid, const PreviewSessionConfig& cfg) {
            return PreviewSessionManager::makeJpegTacoEncoder(wid, cfg);
        },
        [](auto& h) { h.reset(); });

    auto r = mgr.startSession("wk-1", PreviewSessionConfig{15, 80, "jpeg_taco"});
#ifdef PREVIEW_SESSION_NO_FFMPEG
    assert(!r.ok);
    assert(r.http_status == 503);
    assert(r.error_code == "ENCODER_RESOURCE_EXHAUSTED");
#else
    (void)r;
#endif
}

static void test_error_encoder_not_reshared() {
    PreviewSessionManager mgr;
    int creates = 0, destroys = 0;
    mgr.setEncoderFactory([&](const std::string&, const PreviewSessionConfig&) {
        ++creates;
        return PreviewSessionManager::makeNullEncoder();
    }, [&](auto&) { ++destroys; });

    PreviewSessionConfig cfg{15, 80, "jpeg_taco"};
    auto r1 = mgr.startSession("wk-err", cfg);
    assert(r1.ok);
    assert(creates == 1);
    assert(r1.state == PreviewSessionState::RUNNING);
    assert(mgr.activeEncoderCount() == 1);

    // Force ERROR — must not be shareable with ok=true.
    assert(mgr.testingMarkEncoderError("wk-err"));
    assert(!mgr.hasActiveSession("wk-err"));

    PreviewJpegSnapshot snap;
    assert(!mgr.waitLatestJpeg("wk-err", 0, snap, 10));

    // Subsequent START tears down ERROR and creates a fresh encoder.
    auto r2 = mgr.startSession("wk-err", cfg);
    assert(r2.ok);
    assert(creates == 2);
    assert(destroys == 1);
    assert(mgr.activeEncoderCount() == 1);
    assert(mgr.hasActiveSession("wk-err"));
    // Old session id from ERROR encoder is gone (map cleaned on take-stale).
    assert(mgr.getSession(r1.session_id).is_null());

    assert(mgr.stopSession(r2.session_id).ok);
    assert(destroys == 2);
    assert(mgr.activeEncoderCount() == 0);
}

int main() {
    test_has_active_fast_path();
    test_start_stop_idempotent();
    test_jpeg_taco_factory_host_null();
    test_error_encoder_not_reshared();
    std::cout << "PreviewSessionManagerTest OK\n";
    return 0;
}

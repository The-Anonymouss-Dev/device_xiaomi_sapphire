/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "SunlightEnhancementService"

#include "SunlightEnhancement.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <display/drm/mi_disp.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

namespace aidl {
namespace vendor {
namespace lineage {
namespace livedisplay {
namespace xiaomi {

namespace {

constexpr const char* kDispFeaturePath = "/dev/mi_display/disp_feature";
constexpr const char* kBatteryTempPath = "/sys/class/power_supply/battery/temp";

// Battery temperature in tenths of a degree. HBM is held back from the first
// value on and allowed again once the battery has cooled below the second.
constexpr int kHotDeciC = 420;
constexpr int kCoolDeciC = 390;
constexpr auto kGuardInterval = std::chrono::seconds(30);

/*
 * Command sequences in the device tree's DSI packet format:
 * type, last, channel, flags, wait, length (2 bytes), payload.
 * Flags 0x40 queues a packet, the last one (flags 0x00) sends the batch.
 *
 * HBM brightness is 0x36e (878 of 2047), the n7 panel's HBM step 3 of 5:
 * clearly readable in direct sun while keeping heat and panel wear well
 * below full HBM (0x7ff).
 */
#define PKT_Q(...) 0x39, 0x00, 0x00, 0x40, 0x00, 0x00, __VA_ARGS__
#define PKT_SEND(...) 0x39, 0x00, 0x00, 0x00, 0x00, 0x00, __VA_ARGS__

// clang-format off
const std::vector<std::pair<const char*, std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>>
kPanels = {
    // n7: qcom,mdss-dsi-dispparam-hbm-on-l3-command. No HBM off command in
    // its node; leave HBM the way its local-hbm-off-to-normal command does.
    {"n7_38_0c_0a", {
        {PKT_Q(0x02, 0x53, 0xe8), PKT_Q(0x03, 0x51, 0x03, 0x6e), PKT_SEND(0x02, 0xf7, 0x0b)},
        {PKT_Q(0x02, 0x53, 0x28), PKT_SEND(0x02, 0xf7, 0x0b)},
    }},
    // m7 0a/0b: mi,mdss-dsi-hbm-on-command at the step 3 brightness; the
    // driver's HBM off (mi,mdss-dsi-hbm-off-command) turns it off.
    {"m7_38_0c_0a", {
        {PKT_Q(0x02, 0x53, 0xe8), PKT_Q(0x03, 0x51, 0x03, 0x6e), PKT_SEND(0x02, 0xf7, 0x0b)},
        {},
    }},
    {"m7_38_0c_0b", {
        {PKT_Q(0x02, 0x53, 0xf0), PKT_Q(0x03, 0x51, 0x03, 0x6e), PKT_SEND(0x02, 0xf7, 0x07)},
        {},
    }},
};
// clang-format on

bool ioctlDisp(unsigned long request, void* arg, const char* what) {
    android::base::unique_fd fd(open(kDispFeaturePath, O_RDWR | O_CLOEXEC));
    if (fd.get() < 0) {
        PLOG(ERROR) << "Failed to open " << kDispFeaturePath;
        return false;
    }
    if (ioctl(fd.get(), request, arg) < 0) {
        PLOG(ERROR) << "Failed to " << what;
        return false;
    }
    return true;
}

std::string readPanelName() {
    char name[128] = {};
    struct disp_panel_info req;
    memset(&req, 0, sizeof(req));
    req.base.disp_id = MI_DISP_PRIMARY;
    req.info_len = sizeof(name) - 1;
    req.info = name;
    if (!ioctlDisp(MI_DISP_IOCTL_GET_PANEL_INFO, &req, "read the panel name")) return "";
    return name;
}

bool writeDsi(const std::vector<uint8_t>& packets) {
    struct disp_dsi_cmd_req req;
    memset(&req, 0, sizeof(req));
    req.base.flag = MI_DISP_FLAG_BLOCK;
    req.base.disp_id = MI_DISP_PRIMARY;
    req.tx_state = MI_DSI_CMD_LP_STATE;
    req.tx_len = packets.size();
    req.tx_ptr = reinterpret_cast<uintptr_t>(packets.data());
    return ioctlDisp(MI_DISP_IOCTL_WRITE_DSI_CMD, &req, "write the HBM commands");
}

bool setFeature(uint32_t id, int32_t value, const char* what) {
    struct disp_feature_req req;
    memset(&req, 0, sizeof(req));
    req.base.flag = MI_DISP_FLAG_BLOCK;
    req.base.disp_id = MI_DISP_PRIMARY;
    req.feature_id = id;
    req.feature_val = value;
    return ioctlDisp(MI_DISP_IOCTL_SET_FEATURE, &req, what);
}

// Puts the brightness the framework last set back on the panel, through the
// driver's normal backlight path.
void restoreBrightness() {
    struct disp_brightness_req req;
    memset(&req, 0, sizeof(req));
    req.base.disp_id = MI_DISP_PRIMARY;
    if (!ioctlDisp(MI_DISP_IOCTL_GET_BRIGHTNESS, &req, "read the brightness")) return;
    if (req.brightness == 0) return;  // panel off, it starts without HBM anyway
    setFeature(DISP_FEATURE_BACKLIGHT, req.brightness, "restore the brightness");
}

// Returns false when the temperature cannot be read; the guard then stays out
// of the way instead of blocking sunlight mode.
bool readBatteryTemp(int* deciC) {
    std::string buf;
    if (!android::base::ReadFileToString(kBatteryTempPath, &buf)) return false;
    return android::base::ParseInt(android::base::Trim(buf), deciC);
}

}  // namespace

SunlightEnhancement::SunlightEnhancement() {
    std::string name = readPanelName();
    // The driver reports the panel node name from the kernel command line
    // (..._n7_38_0c_0a_...) or, without it, the panel name, which uses spaces
    std::replace(name.begin(), name.end(), ' ', '_');
    for (const auto& [id, cmds] : kPanels) {
        if (name.find(id) != std::string::npos) {
            panel_ = Panel{id, cmds.first, cmds.second};
            break;
        }
    }
    if (panel_) {
        LOG(INFO) << "Sunlight mode for panel " << panel_->id;
    } else {
        LOG(WARNING) << "No sunlight mode for panel \"" << android::base::Trim(name) << "\"";
    }
}

SunlightEnhancement::~SunlightEnhancement() {
    std::unique_lock<std::mutex> lock(mutex_);
    stopping_ = true;
    cv_.notify_all();
    cv_.wait(lock, [this] { return !guardRunning_; });
}

ndk::ScopedAStatus SunlightEnhancement::getEnabled(bool* _aidl_return) {
    std::lock_guard<std::mutex> lock(mutex_);
    *_aidl_return = wanted_;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus SunlightEnhancement::setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    wanted_ = enabled;
    if (enabled) {
        int t;
        hot_ = readBatteryTemp(&t) && t >= kHotDeciC;
        if (hot_) LOG(INFO) << "Battery at " << t / 10 << " C, holding HBM back";
        // The guard only runs while sunlight mode is wanted
        if (!guardRunning_ && panel_) {
            guardRunning_ = true;
            std::thread(&SunlightEnhancement::guardLoop, this).detach();
        }
    }
    cv_.notify_all();
    applyLocked();
    return ndk::ScopedAStatus::ok();
}

void SunlightEnhancement::applyLocked() {
    if (!panel_) return;
    bool on = wanted_ && !hot_;

    if (on) {
        // Sent every time: LiveDisplay repeats the request after the screen
        // comes back on, when the panel has left HBM.
        if (writeDsi(panel_->on)) {
            hbm_ = true;
            LOG(INFO) << "HBM on";
        }
        return;
    }

    // Off only when HBM was sent, so the brightness is not touched otherwise
    if (!hbm_) return;
    bool ok = panel_->off.empty()
                      ? setFeature(DISP_FEATURE_HBM, FEATURE_OFF, "turn HBM off")
                      : writeDsi(panel_->off);
    restoreBrightness();
    if (ok) {
        hbm_ = false;
        LOG(INFO) << "HBM off";
    }
}

void SunlightEnhancement::guardLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_ && wanted_) {
        cv_.wait_for(lock, kGuardInterval);
        if (stopping_ || !wanted_) break;

        int t;
        if (!readBatteryTemp(&t)) continue;
        bool hot = hot_ ? t > kCoolDeciC : t >= kHotDeciC;
        if (hot != hot_) {
            hot_ = hot;
            LOG(INFO) << "Battery at " << t / 10 << " C, HBM " << (hot ? "held back" : "allowed");
            applyLocked();
        }
    }
    guardRunning_ = false;
    cv_.notify_all();
}

}  // namespace xiaomi
}  // namespace livedisplay
}  // namespace lineage
}  // namespace vendor
}  // namespace aidl

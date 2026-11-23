/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/vendor/lineage/livedisplay/BnSunlightEnhancement.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace aidl {
namespace vendor {
namespace lineage {
namespace livedisplay {
namespace xiaomi {

/*
 * Sunlight mode: the panel's HBM.
 *
 * The display driver cannot do it on its own for every sapphire panel: n7
 * has HBM steps but no HBM off command, the m7 panels have an HBM off
 * command but no HBM steps. So the panel's own command sequences (from its
 * device tree node) are sent through MI_DISP_IOCTL_WRITE_DSI_CMD, and the
 * normal brightness is restored through the driver afterwards.
 *
 * LiveDisplay's outdoor mode decides when to use it (ambient light above
 * config_outdoorAmbientLux while the screen is on, never in battery saver)
 * and sends the request again after every screen on, so nothing here watches
 * the display. While sunlight mode is wanted, a guard holds HBM back as long
 * as the battery is hot.
 */
class SunlightEnhancement : public BnSunlightEnhancement {
  public:
    SunlightEnhancement();
    ~SunlightEnhancement() override;

    ndk::ScopedAStatus getEnabled(bool* _aidl_return) override;
    ndk::ScopedAStatus setEnabled(bool enabled) override;

  private:
    struct Panel {
        const char* id;            // part of the panel name
        std::vector<uint8_t> on;   // DSI packets, device tree format
        std::vector<uint8_t> off;  // empty: the driver's own HBM off
    };

    void guardLoop();
    void applyLocked();

    std::optional<Panel> panel_;  // empty: unsupported panel, do nothing

    std::mutex mutex_;
    std::condition_variable cv_;
    bool guardRunning_ = false;
    bool wanted_ = false;  // what LiveDisplay asked for
    bool hot_ = false;     // battery too hot for HBM
    bool hbm_ = false;     // HBM sent to the panel
    bool stopping_ = false;
};

}  // namespace xiaomi
}  // namespace livedisplay
}  // namespace lineage
}  // namespace vendor
}  // namespace aidl

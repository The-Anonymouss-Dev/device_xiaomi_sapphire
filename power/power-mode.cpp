/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "powerhal-ext-sapphire"

#include <aidl/android/hardware/power/BnPower.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/unique_fd.h>
#include <perfmgr/HintManager.h>
#include <sys/system_properties.h>

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace {

// FocalTech exposes double tap to wake as a proc switch; the input
// SYN_CONFIG path below is kept for panels that only take that.
constexpr const char* kTapToWakeNode = "/proc/tp_gesture";

constexpr int kInputEventWakeupModeOff = 4;
constexpr int kInputEventWakeupModeOn = 5;

// Touch input device names used across sapphire panels. FocalTech registers
// as "fts_ts", not "fts".
constexpr const char* kTouchNames[] = {"fts_ts", "fts", "goodix_ts", "NVTCapacitiveTouchScreen"};

android::base::unique_fd openTouchInput() {
    DIR* dir = opendir("/dev/input");
    if (dir == nullptr) return {};

    android::base::unique_fd found;
    while (struct dirent* ent = readdir(dir)) {
        if (ent->d_type != DT_CHR) continue;
        std::string path = std::string("/dev/input/") + ent->d_name;
        android::base::unique_fd fd(open(path.c_str(), O_RDWR | O_CLOEXEC));
        if (fd < 0) continue;

        char name[80] = {};
        if (ioctl(fd.get(), EVIOCGNAME(sizeof(name) - 1), name) <= 0) continue;
        for (const char* touch : kTouchNames) {
            if (strcmp(name, touch) == 0) {
                found = std::move(fd);
                break;
            }
        }
        if (found.ok()) break;
    }
    closedir(dir);
    return found;
}

bool setTapToWake(bool enabled) {
    if (android::base::WriteStringToFile(enabled ? "1" : "0", kTapToWakeNode)) {
        return true;
    }

    android::base::unique_fd fd = openTouchInput();
    if (!fd.ok()) {
        LOG(WARNING) << "Double tap to wake: no " << kTapToWakeNode
                     << " and no supported touchscreen input device";
        return false;
    }
    struct input_event ev = {};
    ev.type = EV_SYN;
    ev.code = SYN_CONFIG;
    ev.value = enabled ? kInputEventWakeupModeOn : kInputEventWakeupModeOff;
    return write(fd.get(), &ev, sizeof(ev)) == sizeof(ev);
}


// KProfiles: a Battery / Balanced / Performance switch in Settings and Quick
// Settings. The app only stores the choice in a persist property; the HAL
// turns it into the PROFILE_* hints of powerhint.json, so it survives a
// reboot and a HAL restart without anything else running. Balanced is the
// stock tuning (no hint). A profile only holds while the screen is on.
constexpr const char* kProfileProp = "persist.vendor.kprofile.mode";

using ::android::perfmgr::HintManager;

std::mutex gProfileLock;
std::string gProfileHint;  // hint currently held, empty for Balanced
std::string gWantedHint;
bool gInteractive = true;
bool gDisplayInactive = false;

std::string hintFor(const std::string& mode) {
    if (mode == "battery") return "PROFILE_BATTERY";
    if (mode == "performance") return "PROFILE_PERFORMANCE";
    return "";
}

// Called with gProfileLock held
void applyProfileLocked() {
    const bool screenOn = gInteractive && !gDisplayInactive;
    const std::string want = screenOn ? gWantedHint : "";
    if (want == gProfileHint) return;

    HintManager* hm = HintManager::GetInstance();
    if (hm == nullptr) return;
    if (!gProfileHint.empty()) hm->EndHint(gProfileHint);
    if (!want.empty() && !hm->DoHint(want)) {
        LOG(ERROR) << "KProfiles: failed to start " << want;
        gProfileHint.clear();
        return;
    }
    gProfileHint = want;
    LOG(INFO) << "KProfiles: " << (want.empty() ? "balanced" : want)
              << (screenOn ? "" : " (screen off)");
}

void watchProfile() {
    uint32_t serial = 0;
    while (true) {
        const prop_info* pi = __system_property_find(kProfileProp);
        if (pi == nullptr) {
            // Not set yet (first boot, or persist props not loaded yet)
            android::base::WaitForPropertyCreation(kProfileProp);
            continue;
        }
        // Take the serial before reading so no change is missed
        serial = __system_property_serial(pi);
        {
            std::lock_guard<std::mutex> lock(gProfileLock);
            gWantedHint = hintFor(android::base::GetProperty(kProfileProp, "balanced"));
            applyProfileLocked();
        }
        __system_property_wait(pi, serial, &serial, nullptr);
    }
}

void startProfileWatcher() {
    static std::once_flag once;
    std::call_once(once, [] { std::thread(watchProfile).detach(); });
}

void setInteractive(bool interactive) {
    std::lock_guard<std::mutex> lock(gProfileLock);
    gInteractive = interactive;
    applyProfileLocked();
}

void setDisplayInactive(bool inactive) {
    std::lock_guard<std::mutex> lock(gProfileLock);
    gDisplayInactive = inactive;
    applyProfileLocked();
}

}  // anonymous namespace

namespace aidl {
namespace google {
namespace hardware {
namespace power {
namespace impl {
namespace pixel {

using ::aidl::android::hardware::power::Mode;

bool isDeviceSpecificModeSupported(Mode type, bool* _aidl_return) {
    // The framework asks for every mode at boot, after HintManager is up
    startProfileWatcher();
    switch (type) {
        case Mode::DOUBLE_TAP_TO_WAKE:
        // The framework only sends INTERACTIVE to a HAL that reports it as
        // supported, and KProfiles needs it to drop the profile with the screen
        case Mode::INTERACTIVE:
            *_aidl_return = true;
            return true;
        default:
            return false;
    }
}

bool setDeviceSpecificMode(Mode type, bool enabled) {
    startProfileWatcher();
    switch (type) {
        case Mode::DOUBLE_TAP_TO_WAKE:
            return setTapToWake(enabled);
        case Mode::INTERACTIVE:
            setInteractive(enabled);
            // powerhint.json has no INTERACTIVE hint; nothing else to do
            return true;
        case Mode::DISPLAY_INACTIVE:
            setDisplayInactive(enabled);
            // Let the HAL apply the DISPLAY_INACTIVE caps as well
            return false;
        default:
            return false;
    }
}

}  // namespace pixel
}  // namespace impl
}  // namespace power
}  // namespace hardware
}  // namespace google
}  // namespace aidl

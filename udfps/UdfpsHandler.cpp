/*
 * Copyright (C) 2024-2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "UdfpsHandler.xiaomi_sm6225"

#include <aidl/android/hardware/biometrics/fingerprint/BnFingerprint.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/unique_fd.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <display/drm/mi_disp.h>

#include "UdfpsHandler.h"
#include "xiaomi_touch.h"

#define COMMAND_NIT 10
#define PARAM_NIT_FOD 1
#define PARAM_NIT_NONE 0

#define COMMAND_FOD_PRESS_STATUS 1
#define PARAM_FOD_PRESSED 1
#define PARAM_FOD_RELEASED 0

#define FOD_STATUS_OFF 0
#define FOD_STATUS_ON 1

#define TOUCH_DEV_PATH "/dev/xiaomi-touch"
#define TOUCH_MAGIC 'T'
#define TOUCH_IOC_SET_CUR_VALUE _IO(TOUCH_MAGIC, SET_CUR_VALUE)

#define DISP_FEATURE_PATH "/dev/mi_display/disp_feature"
#define FOD_PRESS_STATUS_PATH "/sys/class/touch/touch_dev/fod_press_status"
#define BRIGHTNESS_PATH "/sys/class/backlight/panel0-backlight/brightness"

// Screen-off unlock toggle (default on). Nothing in the tree writes it; a ROM
// can mirror the Settings toggle into it.
#define PROP_SCREEN_OFF "persist.vendor.sys.fp.screen_off"
// Also send raw presses to the HAL while the panel is fully off (not dozing).
// Off by default: with the panel off the sensor area cannot be lit, so these
// attempts only end in "Up too fast". Kept as a switch for testing.
#define PROP_SCREEN_OFF_DIRECT "persist.vendor.udfps.screen_off_direct"
// One-touch screen-off unlock: when the display wakes up while the finger
// that started on the sensor (panel off) is still there, start the capture
// with that same touch. Off by default while it is being tested.
#define PROP_WAKE_CONTINUE "persist.vendor.udfps.wake_continue"

using ::aidl::android::hardware::biometrics::fingerprint::AcquiredInfo;

namespace {

// fod_press_status is pulse-like: while a finger rests on the sensor it can
// drop to 0 for 20-200 ms. A press counts once the node has been seen high
// for kPressConfirmMs; a release counts only after the node stayed low for
// kReleaseHoldMs.
constexpr int64_t kPressConfirmMs = 40;
constexpr int64_t kReleaseHoldMs = 150;
// A framework "up" this soon after "down" while the sensor still sees the
// finger is treated as a bounce and deferred to the raw release.
constexpr int64_t kFrameworkUpBounceMs = 250;
// Safety net for a finger-down state nobody clears (e.g. lockout).
constexpr int64_t kStuckRawReleasedMs = 1500;
constexpr int64_t kStuckFrameworkReleasedMs = 3000;
constexpr int64_t kMaxFingerDownMs = 10000;
constexpr int kWatchdogPollMs = 500;
constexpr int64_t kLhbmRecheckMs = 150;
// The panel driver builds the local HBM command from the backlight level at
// that moment. A touch that wakes the screen asks for local HBM while the
// backlight is still 0 (the panel is powering on), and the spot then stays
// too dark for the sensor (first images with 0-200 key points instead of
// ~460, so the first touch after screen-off failed). Wait for the backlight
// before switching local HBM on; it comes ~10 ms after the panel is enabled.
constexpr int64_t kLhbmWaitPollMs = 10;
constexpr int64_t kLhbmWaitMaxMs = 1000;
// One-touch unlock: the press must have started at most this long before
// the display woke up, and the capture starts this long after the wake so
// the panel can take the local HBM request.
constexpr int64_t kWakeContinueWindowMs = 2000;
constexpr int64_t kWakeContinueDelayMs = 80;
// Lockout guard: this many failed matches within the window pause
// screen-off scanning for the lockout time (also stops repeated failed
// attempts from a phone in a pocket).
constexpr size_t kLockoutFailures = 5;
constexpr int64_t kLockoutWindowMs = 60000;
constexpr int64_t kLockoutMs = 30000;
// Only used when the display driver does not deliver power events.
constexpr int kFallbackScreenPollMs = 1000;

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
}

bool readBool(int fd) {
    char c;
    int rc = pread(fd, &c, sizeof(char), 0);
    if (rc != 1) {
        LOG(ERROR) << "failed to read bool from fd, err: " << rc;
        return false;
    }
    return c != '0';
}

int readBrightness() {
    android::base::unique_fd fd(open(BRIGHTNESS_PATH, O_RDONLY | O_CLOEXEC));
    if (fd.get() < 0) return -1;
    char buf[12];
    ssize_t len = read(fd.get(), buf, sizeof(buf) - 1);
    if (len <= 0) return -1;
    buf[len] = '\0';
    return atoi(buf);
}

bool isScreenOn(int state) {
    return state == MI_DISP_POWER_ON;
}

bool isDoze(int state) {
    return state == MI_DISP_POWER_LP1 || state == MI_DISP_POWER_LP2;
}

const char* powerName(int state) {
    switch (state) {
        case MI_DISP_POWER_ON: return "on";
        case MI_DISP_POWER_LP1: return "doze(lp1)";
        case MI_DISP_POWER_LP2: return "doze(lp2)";
        case MI_DISP_POWER_STANDBY: return "standby";
        case MI_DISP_POWER_SUSPEND: return "suspend";
        case MI_DISP_POWER_OFF: return "off";
        default: return "unknown";
    }
}

}  // anonymous namespace

class XiaomiSm6225UdfpsHandler : public UdfpsHandler {
  public:
    ~XiaomiSm6225UdfpsHandler() override { shutdownThreads(); }

    void init(fingerprint_device_t* device) override {
        LOG(INFO) << "Initializing UDFPS handler";
        mDevice = device;

        touch_fd_.reset(open(TOUCH_DEV_PATH, O_RDWR | O_CLOEXEC));
        if (touch_fd_.get() < 0) {
            LOG(ERROR) << "Failed to open " << TOUCH_DEV_PATH << ": " << strerror(errno);
        }
        disp_fd_.reset(open(DISP_FEATURE_PATH, O_RDWR | O_CLOEXEC));
        if (disp_fd_.get() < 0) {
            LOG(ERROR) << "Failed to open " << DISP_FEATURE_PATH << ": " << strerror(errno);
        }
        stop_fd_.reset(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
        kick_fd_.reset(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));

        std::string fpVendor = android::base::GetProperty("persist.vendor.sys.fp.vendor", "none");
        isFpcFod = (fpVendor == "fpc_fod");
        LOG(INFO) << "Fingerprint vendor: " << fpVendor;

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            updateFodScanLocked();
        }

        rawThread_ = std::thread([this]() { fodPressMonitorThread(); });
        dispThread_ = std::thread([this]() { displayEventMonitorThread(); });
        LOG(INFO) << "UDFPS handler initialized";
    }

    void onFingerDown(uint32_t /*x*/, uint32_t /*y*/, float /*minor*/, float /*major*/) override {
        LOG(INFO) << __func__;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (mFingerDown && mDeferredUp) {
                // New tap while the previous one is still waiting for its
                // release: finish that one so the HAL gets a clean up/down.
                LOG(INFO) << "New touch during a deferred up; closing the previous one";
                endFingerDownLocked();
            }
            mDeferredUp = false;
        }
        syncHalFingerState();
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            // fpc_fod does not reliably send the "waiting for finger" message.
            if (isFpcFod) setFodStatus(FOD_STATUS_ON);
            beginFingerDownLocked(Source::FRAMEWORK);
        }
        syncHalFingerState();
        kickRawThread();
    }

    void onFingerUp() override {
        LOG(INFO) << __func__;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!mFingerDown) return;
            int64_t elapsed = nowMs() - mDownMs;
            if (mSource == Source::FRAMEWORK && elapsed < kFrameworkUpBounceMs &&
                mRawPressed.load()) {
                LOG(INFO) << "Framework up after " << elapsed
                          << " ms while the sensor still sees the finger; deferring";
                mDeferredUp = true;
                return;
            }
            mDeferredUp = false;
            endFingerDownLocked();
        }
        syncHalFingerState();
    }

    void onAcquired(int32_t result, int32_t vendorCode) override {
        LOG(INFO) << __func__ << " result: " << result << " vendorCode: " << vendorCode;

        if (static_cast<AcquiredInfo>(result) == AcquiredInfo::GOOD) {
            // Capture done: drop the local HBM right away.
            setLocalHbm(false);
        }

        /*
         * Vendor codes:
         * 21: waiting for finger (goodix_fod)
         * 22: finger down (fpc_fod)
         * 23: finger up
         */
        if ((!isFpcFod && vendorCode == 21) || (isFpcFod && vendorCode == 22)) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            mAuthWaiting = true;
            setFodStatus(FOD_STATUS_ON);
        }
    }

    void onAuthenticationSucceeded() override {
        LOG(INFO) << __func__;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            mFailTimes.clear();
            mLockoutUntilMs.store(0);
            mAuthWaiting = false;
            mDeferredUp = false;
            endFingerDownLocked();
            updateFodScanLocked();
        }
        syncHalFingerState();
    }

    void onAuthenticationFailed() override {
        // The session goes on; the finger-up path or the watchdog clears the state.
        LOG(INFO) << __func__;
        std::lock_guard<std::mutex> lock(state_mutex_);
        int64_t now = nowMs();
        mFailTimes.push_back(now);
        while (!mFailTimes.empty() && now - mFailTimes.front() > kLockoutWindowMs) {
            mFailTimes.pop_front();
        }
        if (mFailTimes.size() >= kLockoutFailures) {
            mFailTimes.clear();
            mLockoutUntilMs.store(now + kLockoutMs);
            LOG(INFO) << "Lockout guard: " << kLockoutFailures
                      << " failed matches, screen-off scanning paused for " << kLockoutMs / 1000
                      << " s";
            updateFodScanLocked();
            kickRawThread();
        }
    }

    void cancel() override {
        LOG(INFO) << __func__;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            mAuthWaiting = false;
            mDeferredUp = false;
            endFingerDownLocked();
            // Re-arms the FOD scan when the screen is off, so screen-off unlock
            // keeps working after a cancelled session.
            updateFodScanLocked();
        }
        syncHalFingerState();
    }

  private:
    enum class Source { NONE, FRAMEWORK, RAW };

    fingerprint_device_t* mDevice = nullptr;
    bool isFpcFod = false;

    android::base::unique_fd touch_fd_;
    android::base::unique_fd disp_fd_;
    android::base::unique_fd stop_fd_;  // readable once shutting down
    android::base::unique_fd kick_fd_;  // wakes the raw thread to re-plan its timers

    std::atomic<bool> isRunning{true};
    std::atomic<int> mPowerState{-1};  // MI_DISP_POWER_*, -1 until the first event

    // Guarded by state_mutex_.
    std::mutex state_mutex_;
    bool mFingerDown = false;
    Source mSource = Source::NONE;
    int64_t mDownMs = 0;
    bool mDeferredUp = false;
    bool mAuthWaiting = false;

    // Debounced raw sensor state, owned by the raw thread.
    std::atomic<bool> mRawPressed{false};
    std::atomic<int64_t> mRawReleasedMs{0};

    // One-touch unlock bookkeeping (raw thread writes, others read).
    std::atomic<int64_t> mRawPressStartMs{0};
    std::atomic<bool> mRawPendingWake{false};    // press seen with the panel off
    std::atomic<int64_t> mWakeContinueAtMs{0};   // 0 = nothing scheduled

    // Lockout guard. mFailTimes is guarded by state_mutex_.
    std::deque<int64_t> mFailTimes;
    std::atomic<int64_t> mLockoutUntilMs{0};

    // Finger state as last sent to the HAL; guarded by hal_mutex_.
    std::mutex hal_mutex_;
    bool mHalFingerDown = false;
    std::atomic<bool> mFingerDownWanted{false};

    std::mutex touch_mutex_;
    std::mutex disp_mutex_;
    std::atomic<bool> mLhbmOn{false};  // last local HBM request
    std::atomic<int64_t> mLhbmRecheckMs{0};  // when to repeat HBM-off, 0 = none
    std::atomic<int64_t> mLhbmWaitAtMs{0};    // next backlight check for HBM-on, 0 = none
    std::atomic<int64_t> mLhbmWaitSinceMs{0};

    std::thread rawThread_;
    std::thread dispThread_;
    std::thread fallbackThread_;

    // ---- screen state ----------------------------------------------------

    int screenState() {
        int s = mPowerState.load();
        if (s >= 0) return s;
        int b = readBrightness();
        if (b < 0) return MI_DISP_POWER_ON;  // unknown: behave as if on (safe)
        return b > 0 ? MI_DISP_POWER_ON : MI_DISP_POWER_OFF;
    }

    static bool screenOffUnlockEnabled() {
        return android::base::GetBoolProperty(PROP_SCREEN_OFF, true);
    }

    bool lockedOut() const { return mLockoutUntilMs.load() > nowMs(); }

    void onScreenStateChanged(int state) {
        LOG(INFO) << "Display power: " << powerName(state);
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            updateFodScanLocked();
        }
        // The display can light the sensor now; if the finger that woke it
        // is still on the sensor, capture with that same touch.
        if ((isScreenOn(state) || isDoze(state)) && mRawPendingWake.load() &&
            android::base::GetBoolProperty(PROP_WAKE_CONTINUE, false)) {
            mWakeContinueAtMs.store(nowMs() + kWakeContinueDelayMs);
            kickRawThread();
        }
    }

    // Runs on the raw thread when the wake-continue timer fires.
    void tryWakeContinue() {
        int64_t now = nowMs();
        int64_t pressedFor = now - mRawPressStartMs.load();
        bool pending = mRawPendingWake.load();
        bool ok = pending && mRawPressed.load() &&
                  pressedFor <= kWakeContinueWindowMs + kWakeContinueDelayMs + 500 &&
                  !lockedOut() && screenOffUnlockEnabled() && !isScreenOff(screenState());
        if (!ok) {
            if (pending) {
                LOG(INFO) << "One-touch unlock skipped (finger gone, too late or locked out)";
            }
            mRawPendingWake.store(false);
            std::lock_guard<std::mutex> lock(state_mutex_);
            updateFodScanLocked();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            mRawPendingWake.store(false);
            if (mFingerDown) return;
            LOG(INFO) << "One-touch unlock: finger still on the sensor after wake-up ("
                      << pressedFor << " ms) -> finger down";
            mDeferredUp = false;
            beginFingerDownLocked(Source::RAW);
            updateFodScanLocked();
        }
        syncHalFingerState();
    }

    static bool isScreenOff(int state) { return !isScreenOn(state) && !isDoze(state); }

    // FOD scanning on the touch IC: on while an auth/enroll session waits for
    // a finger or a finger is down, and while the screen is off/dozing if
    // screen-off unlock is enabled.
    void updateFodScanLocked() {
        bool want;
        bool screenOn = isScreenOn(screenState());
        if (mFingerDown || (mRawPendingWake.load() && mRawPressed.load())) {
            // Keep scanning while a finger is on the sensor, including the
            // one that woke the display (one-touch unlock).
            want = true;
        } else if (!screenOn && lockedOut()) {
            // Too many failed matches: stop scanning in the dark (pocket).
            want = false;
        } else if (mAuthWaiting) {
            want = true;
        } else if (!screenOn) {
            want = screenOffUnlockEnabled();
        } else {
            want = false;
        }
        setFodStatus(want ? FOD_STATUS_ON : FOD_STATUS_OFF);
    }

    // ---- finger state ----------------------------------------------------

    void beginFingerDownLocked(Source source) {
        if (mFingerDown) {
            // e.g. raw press first, then the framework sees the same touch.
            if (source == Source::FRAMEWORK) mSource = source;
            // GOOD may have switched the local HBM off already.
            if (!mLhbmOn) requestLocalHbmOn();
            return;
        }
        mFingerDown = true;
        mSource = source;
        mDownMs = nowMs();
        setTouchFingerDown(true);
        requestLocalHbmOn();
        // On a wake-up touch the HAL hears about the finger only once the
        // spot is lit (checkLhbmWait), as on a touch with the screen on.
        if (mLhbmWaitAtMs.load() == 0) mFingerDownWanted.store(true);
    }

    // Local HBM on, once the backlight is up (see kLhbmWaitPollMs).
    void requestLocalHbmOn() {
        if (readBrightness() != 0) {
            mLhbmWaitAtMs.store(0);
            setLocalHbm(true);
            return;
        }
        int64_t now = nowMs();
        if (mLhbmWaitAtMs.load() == 0) {
            mLhbmWaitSinceMs.store(now);
            LOG(INFO) << "Backlight still off; local HBM waits for it";
        }
        mLhbmWaitAtMs.store(now + kLhbmWaitPollMs);
        kickRawThread();
    }

    // Runs on the raw thread while requestLocalHbmOn() is waiting.
    void checkLhbmWait(int64_t now) {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (mLhbmWaitAtMs.load() == 0) return;
            if (!mFingerDown) {
                mLhbmWaitAtMs.store(0);
                return;
            }
            int64_t waited = now - mLhbmWaitSinceMs.load();
            if (readBrightness() == 0 && waited < kLhbmWaitMaxMs) {
                mLhbmWaitAtMs.store(now + kLhbmWaitPollMs);
                return;
            }
            LOG(INFO) << "Local HBM on after waiting " << waited
                      << " ms for the backlight -> finger down";
            mLhbmWaitAtMs.store(0);
            setLocalHbm(true);
            mFingerDownWanted.store(true);
        }
        syncHalFingerState();
    }

    bool endFingerDownLocked() {
        if (!mFingerDown) return false;
        mFingerDown = false;
        mSource = Source::NONE;
        mFingerDownWanted.store(false);
        setTouchFingerDown(false);
        setLocalHbm(false);
        // Repeat the HBM-off once shortly after: an "off" that lands while
        // the panel is still switching local HBM on can be lost.
        mLhbmRecheckMs.store(nowMs() + kLhbmRecheckMs);
        kickRawThread();
        return true;
    }

    // The HAL may call back into this handler from inside extCmd(), so it is
    // never called with state_mutex_ held. hal_mutex_ keeps the commands in
    // order and drops ones that no longer match the current state.
    void syncHalFingerState() {
        std::lock_guard<std::mutex> lock(hal_mutex_);
        bool want = mFingerDownWanted.load();
        if (want == mHalFingerDown || mDevice == nullptr) return;
        LOG(INFO) << "Finger " << (want ? "down" : "up") << " -> HAL";
        mHalFingerDown = want;
        mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS,
                        want ? PARAM_FOD_PRESSED : PARAM_FOD_RELEASED);
    }

    // ---- raw sensor thread ----------------------------------------------

    void kickRawThread() {
        if (kick_fd_.get() >= 0) eventfd_write(kick_fd_.get(), 1);
    }

    // Called by the raw thread once a press is confirmed.
    void onRawPressConfirmed() {
        int state = screenState();
        if (isScreenOn(state)) {
            // Screen-on touches go through the UDFPS overlay (onFingerDown);
            // raw presses here are stray touches (PIN keys, shade, ...).
            return;
        }
        if (!screenOffUnlockEnabled()) {
            LOG(DEBUG) << "Raw press ignored: screen-off unlock disabled";
            return;
        }
        if (lockedOut()) {
            LOG(INFO) << "Raw press ignored: lockout guard active";
            return;
        }
        if (!isDoze(state) && !android::base::GetBoolProperty(PROP_SCREEN_OFF_DIRECT, false)) {
            // Remember it: if the display wakes while this finger stays on
            // the sensor, the capture can start with this touch.
            mRawPendingWake.store(true);
            // The panel is off and cannot light the sensor. The UDFPS doze
            // sensor wakes the display; the next touch then authenticates.
            LOG(INFO) << "Raw press with panel " << powerName(state)
                      << ": leaving the wake-up to the UDFPS sensor";
            return;
        }
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (mFingerDown) return;
            mDeferredUp = false;
            LOG(INFO) << "Raw press with panel " << powerName(state) << " -> finger down";
            beginFingerDownLocked(Source::RAW);
        }
        syncHalFingerState();
    }

    void onRawReleased() {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!mFingerDown) return;
            // Framework touches end with onFingerUp(), unless it was deferred.
            if (mSource != Source::RAW && !mDeferredUp) return;
            mDeferredUp = false;
            endFingerDownLocked();
        }
        syncHalFingerState();
    }

    void runWatchdog(int64_t now) {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!mFingerDown) return;
            bool rawUp = !mRawPressed.load();
            int64_t releasedFor = rawUp ? now - std::max(mDownMs, mRawReleasedMs.load()) : 0;
            int64_t limit = (mSource == Source::RAW) ? kStuckRawReleasedMs
                                                     : kStuckFrameworkReleasedMs;
            bool stuck = rawUp && releasedFor > limit;
            bool tooLong = now - mDownMs > kMaxFingerDownMs;
            if (!stuck && !tooLong) return;
            LOG(WARNING) << "Watchdog: clearing finger-down state ("
                         << (tooLong ? "held too long" : "sensor released") << ")";
            mDeferredUp = false;
            endFingerDownLocked();
            updateFodScanLocked();
        }
        syncHalFingerState();
    }

    void fodPressMonitorThread() {
        LOG(INFO) << "FOD press monitor thread started";
        android::base::unique_fd fd(open(FOD_PRESS_STATUS_PATH, O_RDONLY | O_CLOEXEC));
        if (fd.get() < 0) {
            LOG(ERROR) << "Failed to open " << FOD_PRESS_STATUS_PATH << ": " << strerror(errno);
            return;
        }
        readBool(fd.get());  // arm sysfs_notify

        int64_t firstPressMs = 0;
        bool pressHandled = false;
        int64_t releaseDeadline = 0;

        while (isRunning.load()) {
            int64_t now = nowMs();
            int timeout = -1;
            auto consider = [&](int64_t ms) {
                ms = std::max<int64_t>(ms, 0);
                if (timeout < 0 || ms < timeout) timeout = static_cast<int>(ms);
            };
            if (mRawPressed.load() && !pressHandled && releaseDeadline == 0) {
                consider(firstPressMs + kPressConfirmMs - now);
            }
            if (releaseDeadline) consider(releaseDeadline - now);
            if (int64_t r = mLhbmRecheckMs.load()) consider(r - now);
            if (int64_t h = mLhbmWaitAtMs.load()) consider(h - now);
            if (int64_t w = mWakeContinueAtMs.load()) consider(w - now);
            if (int64_t l = mLockoutUntilMs.load()) consider(l - now);
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (mFingerDown) consider(kWatchdogPollMs);
            }

            struct pollfd fds[] = {
                    {.fd = fd.get(), .events = POLLERR | POLLPRI, .revents = 0},
                    {.fd = stop_fd_.get(), .events = POLLIN, .revents = 0},
                    {.fd = kick_fd_.get(), .events = POLLIN, .revents = 0},
            };
            int rc = poll(fds, 3, timeout);
            if (rc < 0) {
                if (errno == EINTR) continue;
                LOG(ERROR) << "Poll failed: " << strerror(errno);
                break;
            }
            if (fds[1].revents) break;
            if (fds[2].revents) {
                eventfd_t v;
                eventfd_read(kick_fd_.get(), &v);
            }
            if (fds[0].revents & (POLLHUP | POLLNVAL)) {
                LOG(ERROR) << "Poll error on " << FOD_PRESS_STATUS_PATH;
                break;
            }

            now = nowMs();
            if (fds[0].revents & (POLLERR | POLLPRI)) {
                if (readBool(fd.get())) {
                    releaseDeadline = 0;
                    if (!mRawPressed.load()) {
                        mRawPressed.store(true);
                        mRawPressStartMs.store(now);
                        mRawPendingWake.store(false);
                        firstPressMs = now;
                        pressHandled = false;
                    }
                } else if (mRawPressed.load() && releaseDeadline == 0) {
                    releaseDeadline = now + kReleaseHoldMs;
                }
            }

            // Press confirmation: the node must still read high after the
            // confirm delay (a brush over the sensor does not). If it reads
            // low, start the release timer instead of re-checking in a loop.
            if (mRawPressed.load() && !pressHandled && releaseDeadline == 0 &&
                now - firstPressMs >= kPressConfirmMs) {
                if (readBool(fd.get())) {
                    pressHandled = true;
                    onRawPressConfirmed();
                } else {
                    releaseDeadline = now + kReleaseHoldMs;
                }
            }

            // Release confirmation.
            if (releaseDeadline && now >= releaseDeadline) {
                releaseDeadline = 0;
                if (!readBool(fd.get())) {
                    mRawPressed.store(false);
                    bool hadPendingWake = mRawPendingWake.exchange(false);
                    mRawReleasedMs.store(now);
                    onRawReleased();
                    if (hadPendingWake) {
                        // The scan was kept on for this finger; re-evaluate.
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        updateFodScanLocked();
                    }
                }
            }

            // One-touch unlock after a wake-up (see onScreenStateChanged).
            if (int64_t w = mWakeContinueAtMs.load(); w && now >= w) {
                mWakeContinueAtMs.store(0);
                tryWakeContinue();
            }

            // Lockout guard expired: allow screen-off scanning again.
            if (int64_t l = mLockoutUntilMs.load(); l && now >= l) {
                mLockoutUntilMs.store(0);
                LOG(INFO) << "Lockout guard ended";
                std::lock_guard<std::mutex> lock(state_mutex_);
                updateFodScanLocked();
            }

            // Local HBM waiting for the backlight (see requestLocalHbmOn).
            if (int64_t h = mLhbmWaitAtMs.load(); h && now >= h) checkLhbmWait(now);

            // Repeated HBM-off (see endFingerDownLocked).
            if (int64_t r = mLhbmRecheckMs.load(); r && now >= r) {
                mLhbmRecheckMs.store(0);
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (!mFingerDown) setLocalHbm(false);
            }

            runWatchdog(now);
        }
        LOG(INFO) << "FOD press monitor thread stopped";
    }

    // ---- display events --------------------------------------------------

    bool registerDispEvent(int fd, int type) {
        disp_event_req req;
        req.base.flag = 0;
        req.base.disp_id = MI_DISP_PRIMARY;
        req.type = type;
        if (ioctl(fd, MI_DISP_IOCTL_REGISTER_EVENT, &req) < 0) {
            LOG(ERROR) << "Failed to register display event " << type << ": " << strerror(errno);
            return false;
        }
        return true;
    }

    void displayEventMonitorThread() {
        LOG(INFO) << "Display event monitor thread started";
        android::base::unique_fd fd(open(DISP_FEATURE_PATH, O_RDWR | O_CLOEXEC));
        if (fd.get() < 0) {
            LOG(ERROR) << "Failed to open " << DISP_FEATURE_PATH << ": " << strerror(errno);
            startFallbackScreenPoll();
            return;
        }
        if (!registerDispEvent(fd.get(), MI_DISP_EVENT_FOD)) {
            startFallbackScreenPoll();
            return;
        }
        if (!registerDispEvent(fd.get(), MI_DISP_EVENT_POWER)) {
            startFallbackScreenPoll();
        }

        char buf[1024];
        while (isRunning.load()) {
            struct pollfd fds[] = {
                    {.fd = fd.get(), .events = POLLIN, .revents = 0},
                    {.fd = stop_fd_.get(), .events = POLLIN, .revents = 0},
            };
            int rc = poll(fds, 2, -1);
            if (rc < 0) {
                if (errno == EINTR) continue;
                LOG(ERROR) << "Display poll failed: " << strerror(errno);
                break;
            }
            if (fds[1].revents) break;
            if (!(fds[0].revents & POLLIN)) {
                if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    LOG(ERROR) << "Display poll error: " << fds[0].revents;
                    break;
                }
                continue;
            }

            ssize_t size = read(fd.get(), buf, sizeof(buf));
            if (size < static_cast<ssize_t>(sizeof(struct disp_event) + 1)) {
                LOG(ERROR) << "Invalid display event size " << size;
                continue;
            }
            auto* response = reinterpret_cast<struct disp_event_resp*>(buf);
            int value = response->data[0];

            switch (response->base.type) {
                case MI_DISP_EVENT_FOD: {
                    bool localHbmUiReady = value & LOCAL_HBM_UI_READY;
                    LOG(DEBUG) << "FOD display event 0x" << std::hex << value;
                    std::lock_guard<std::mutex> lock(hal_mutex_);
                    if (mDevice != nullptr) {
                        mDevice->extCmd(mDevice, COMMAND_NIT,
                                        localHbmUiReady ? PARAM_NIT_FOD : PARAM_NIT_NONE);
                    }
                    break;
                }
                case MI_DISP_EVENT_POWER:
                    if (value != mPowerState.exchange(value)) onScreenStateChanged(value);
                    break;
                default:
                    LOG(DEBUG) << "Unhandled display event " << response->base.type;
                    break;
            }
        }
        LOG(INFO) << "Display event monitor thread stopped";
    }

    // Used only if the display driver does not deliver power events.
    void startFallbackScreenPoll() {
        if (fallbackThread_.joinable()) return;
        LOG(WARNING) << "No display power events; polling brightness every "
                     << kFallbackScreenPollMs << " ms";
        fallbackThread_ = std::thread([this]() {
            bool lastOn = isScreenOn(screenState());
            while (isRunning.load()) {
                struct pollfd pfd = {.fd = stop_fd_.get(), .events = POLLIN, .revents = 0};
                if (poll(&pfd, 1, kFallbackScreenPollMs) != 0) break;
                bool on = isScreenOn(screenState());
                if (on != lastOn) {
                    lastOn = on;
                    onScreenStateChanged(on ? MI_DISP_POWER_ON : MI_DISP_POWER_OFF);
                }
            }
        });
    }

    // ---- low-level controls ---------------------------------------------

    void setFodStatus(int value) {
        std::lock_guard<std::mutex> lock(touch_mutex_);
        if (touch_fd_.get() < 0) return;
        int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Fod_Enable, value};
        if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf) < 0) {
            LOG(ERROR) << "Failed to set FOD status: " << strerror(errno);
        } else {
            LOG(DEBUG) << "FOD scan " << (value ? "on" : "off");
        }
    }

    void setTouchFingerDown(bool pressed) {
        std::lock_guard<std::mutex> lock(touch_mutex_);
        if (touch_fd_.get() < 0) return;
        int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, THP_FOD_DOWNUP_CTL, pressed ? 1 : 0};
        if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf) < 0) {
            LOG(ERROR) << "Failed to set finger down: " << strerror(errno);
        }
    }

    void setLocalHbm(bool on) {
        if (!on) mLhbmWaitAtMs.store(0);  // a pending HBM-on is no longer wanted
        std::lock_guard<std::mutex> lock(disp_mutex_);
        mLhbmOn = on;
        if (disp_fd_.get() < 0) return;
        disp_local_hbm_req req;
        req.base.flag = 0;
        req.base.disp_id = MI_DISP_PRIMARY;
        req.local_hbm_value =
                on ? LHBM_TARGET_BRIGHTNESS_WHITE_1000NIT : LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP;
        if (ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req) < 0) {
            LOG(ERROR) << "Failed to set local HBM: " << strerror(errno);
        }
    }

    void shutdownThreads() {
        isRunning.store(false);
        if (stop_fd_.get() >= 0) eventfd_write(stop_fd_.get(), 1);
        if (rawThread_.joinable()) rawThread_.join();
        if (dispThread_.joinable()) dispThread_.join();
        if (fallbackThread_.joinable()) fallbackThread_.join();
    }
};

static UdfpsHandler* create() {
    return new XiaomiSm6225UdfpsHandler();
}

static void destroy(UdfpsHandler* handler) {
    delete handler;
}

extern "C" UdfpsHandlerFactory UDFPS_HANDLER_FACTORY = {
        .create = create,
        .destroy = destroy,
};

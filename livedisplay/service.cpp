/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "vendor.lineage.livedisplay-service.xiaomi"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include "SunlightEnhancement.h"

using ::aidl::vendor::lineage::livedisplay::xiaomi::SunlightEnhancement;

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(0);

    std::shared_ptr<SunlightEnhancement> se = ndk::SharedRefBase::make<SunlightEnhancement>();
    const std::string instance = std::string() + SunlightEnhancement::descriptor + "/default";
    binder_status_t status = AServiceManager_addService(se->asBinder().get(), instance.c_str());
    CHECK_EQ(status, STATUS_OK) << "Failed to register " << instance;

    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;  // should not reach
}

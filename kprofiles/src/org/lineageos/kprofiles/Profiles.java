/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.kprofiles;

import android.os.SystemProperties;
import android.util.Log;

/**
 * The profile lives in persist.vendor.kprofile.mode. The power HAL watches it
 * and holds the matching powerhint.json hint (PROFILE_BATTERY or
 * PROFILE_PERFORMANCE; Balanced is the stock tuning) while the screen is on,
 * so nothing in this app has to run for a profile to stay in effect.
 */
final class Profiles {

    private static final String TAG = "KProfiles";
    private static final String PROP = "persist.vendor.kprofile.mode";

    static final String BATTERY = "battery";
    static final String BALANCED = "balanced";
    static final String PERFORMANCE = "performance";
    static final String[] ALL = {BATTERY, BALANCED, PERFORMANCE};

    private Profiles() {}

    static String get() {
        String mode = SystemProperties.get(PROP, BALANCED);
        for (String m : ALL) {
            if (m.equals(mode)) return mode;
        }
        return BALANCED;
    }

    static boolean set(String mode) {
        try {
            SystemProperties.set(PROP, mode);
            return true;
        } catch (RuntimeException e) {
            Log.e(TAG, "Failed to set " + PROP + " to " + mode, e);
            return false;
        }
    }

    /** Battery, Balanced, Performance, then back to Battery. */
    static String next(String mode) {
        for (int i = 0; i < ALL.length; i++) {
            if (ALL[i].equals(mode)) return ALL[(i + 1) % ALL.length];
        }
        return BALANCED;
    }

    static int titleRes(String mode) {
        switch (mode) {
            case BATTERY: return R.string.profile_battery_title;
            case PERFORMANCE: return R.string.profile_performance_title;
            default: return R.string.profile_balanced_title;
        }
    }

    static int iconRes(String mode) {
        switch (mode) {
            case BATTERY: return R.drawable.ic_kprofile_battery;
            case PERFORMANCE: return R.drawable.ic_kprofile_performance;
            default: return R.drawable.ic_kprofile_balanced;
        }
    }
}

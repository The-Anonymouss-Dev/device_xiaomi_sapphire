/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.chargingmodes;

import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemProperties;
import android.util.Log;

import androidx.preference.Preference;
import androidx.preference.SwitchPreferenceCompat;

import com.android.settingslib.widget.SelectorWithWidgetPreference;
import com.android.settingslib.widget.SettingsBasePreferenceFragment;

import java.util.Locale;

/**
 * Settings > Battery > Charging modes.
 *
 * The choice goes to two vendor properties; init (init.xiaomi.rc) applies them
 * to the charger driver:
 *   persist.vendor.charge.mode     super | fast | slow
 *     fast and slow cap the charging current through the qcom-battery
 *     restrict_chg / restrict_cur nodes (4.5 A / 2.5 A, about 18 W / 10 W
 *     at the battery), super removes the cap.
 *   persist.vendor.charge.limit80  true | false
 *     the battery's night_charging node: pause at 80%, resume below 75%.
 * Both nodes only lower the current; the thermal limits still apply.
 *
 * While the page is open the top shows the "Ember Halo" (EmberHaloView) with
 * the level, plus the power going into the battery, the temperature and the
 * time left, so a mode change can be seen right away. The halo only turns
 * for a few seconds after the page opens, the mode changes or the charger
 * state changes, then rests, so the page does not keep the GPU busy.
 */
public class ChargingModesFragment extends SettingsBasePreferenceFragment
        implements SelectorWithWidgetPreference.OnClickListener,
                Preference.OnPreferenceChangeListener {

    private static final String TAG = "ChargingModes";

    static final String PROP_MODE = "persist.vendor.charge.mode";
    static final String PROP_LIMIT80 = "persist.vendor.charge.limit80";

    static final String MODE_SUPER = "super";
    static final String MODE_FAST = "fast";
    static final String MODE_SLOW = "slow";
    private static final String[] MODES = {MODE_SUPER, MODE_FAST, MODE_SLOW};

    private static final String KEY_STATUS = "status";
    private static final String KEY_LIMIT80 = "limit80";

    private static final long STATUS_INTERVAL_MS = 1000;

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private final Runnable mStatusUpdater = new Runnable() {
        @Override
        public void run() {
            updateStatus();
            mHandler.postDelayed(this, STATUS_INTERVAL_MS);
        }
    };

    private HeroPreference mStatus;
    // Charging state as shown, and how many polls the new state has held;
    // near full a weak charger flips it every few seconds.
    private Boolean mShownCharging;
    private int mStateRun;
    private SwitchPreferenceCompat mLimit80;
    private BatteryManager mBatteryManager;

    @Override
    public void onCreatePreferences(Bundle savedInstanceState, String rootKey) {
        addPreferencesFromResource(R.xml.charging_modes);

        mBatteryManager = requireContext().getSystemService(BatteryManager.class);
        mStatus = findPreference(KEY_STATUS);

        for (String mode : MODES) {
            SelectorWithWidgetPreference pref = findPreference(mode);
            if (pref != null) pref.setOnClickListener(this);
        }

        mLimit80 = findPreference(KEY_LIMIT80);
        if (mLimit80 != null) mLimit80.setOnPreferenceChangeListener(this);
    }

    @Override
    public void onResume() {
        super.onResume();
        updateModes(currentMode());
        if (mStatus != null) mStatus.wake();
        if (mLimit80 != null) {
            mLimit80.setChecked(SystemProperties.getBoolean(PROP_LIMIT80, false));
        }
        mHandler.post(mStatusUpdater);
    }

    @Override
    public void onPause() {
        super.onPause();
        mHandler.removeCallbacks(mStatusUpdater);
    }

    @Override
    public void onRadioButtonClicked(SelectorWithWidgetPreference pref) {
        String mode = pref.getKey();
        if (!setProp(PROP_MODE, mode)) {
            // Keep showing what is actually in effect
            mode = currentMode();
        }
        updateModes(mode);
        if (mStatus != null) mStatus.wake();
    }

    @Override
    public boolean onPreferenceChange(Preference preference, Object newValue) {
        if (preference == mLimit80) {
            // init.xiaomi.rc triggers on "true" / "false"
            boolean ok = setProp(PROP_LIMIT80, (Boolean) newValue ? "true" : "false");
            if (ok) mHandler.post(this::updateStatus);
            return ok;
        }
        return false;
    }

    private void updateStatus() {
        Context context = getContext();
        if (mStatus == null || context == null) return;

        // Sticky broadcast: no receiver is registered, nothing keeps running
        Intent battery = context.registerReceiver(null,
                new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        if (battery == null) return;

        int level = battery.getIntExtra(BatteryManager.EXTRA_LEVEL, -1);
        int scale = battery.getIntExtra(BatteryManager.EXTRA_SCALE, 100);
        int percent = level >= 0 && scale > 0 ? Math.round(level * 100f / scale) : -1;
        int status = battery.getIntExtra(BatteryManager.EXTRA_STATUS, -1);
        boolean plugged = battery.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) != 0;
        boolean charging = status == BatteryManager.BATTERY_STATUS_CHARGING;
        boolean limit = mLimit80 != null && mLimit80.isChecked();
        boolean paused = limit && plugged && !charging && percent >= 80;

        // EXTRA_VOLTAGE is in mV, CURRENT_NOW in uA, EXTRA_TEMPERATURE in 0.1 °C
        double volts = battery.getIntExtra(BatteryManager.EXTRA_VOLTAGE, 0) / 1000.0;
        long microAmps = mBatteryManager.getLongProperty(
                BatteryManager.BATTERY_PROPERTY_CURRENT_NOW);
        // Long.MIN_VALUE means unsupported, and Math.abs() keeps it negative.
        double amps = microAmps == Long.MIN_VALUE ? 0 : Math.abs(microAmps) / 1000000.0;
        double temp = battery.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0) / 10.0;

        CharSequence state, power, details;
        if (charging) {
            state = getString(R.string.hero_charging);
            power = String.format(Locale.getDefault(), "%.1f W", volts * amps);
            details = getString(R.string.hero_details,
                    String.format(Locale.getDefault(), "%.2f", amps),
                    String.format(Locale.getDefault(), "%.2f", volts));
        } else if (paused) {
            state = getString(R.string.hero_paused);
            power = getString(R.string.hero_paused_short);
            details = getString(R.string.hero_paused_details);
        } else if (status == BatteryManager.BATTERY_STATUS_FULL) {
            state = getString(R.string.hero_full);
            power = "100%";
            details = plugged ? getString(R.string.hero_connected) : "";
        } else {
            state = getString(R.string.status_not_charging);
            power = "0 W";
            details = plugged ? getString(R.string.hero_connected) : "";
        }

        // Time left: the framework's time to full, or below the 80% limit the
        // time to 80% from the charge counter (constant-current part, linear).
        boolean to80 = limit && percent >= 0 && percent < 80;
        long seconds = -1;
        if (charging) {
            if (to80) {
                long counter = mBatteryManager.getLongProperty(
                        BatteryManager.BATTERY_PROPERTY_CHARGE_COUNTER);
                if (counter > 0 && percent > 0 && amps > 0.05) {
                    double full = counter * 100.0 / percent;
                    seconds = Math.round(Math.max(0, full * 0.8 - counter) / 1e6 / amps * 3600);
                }
            } else {
                long ms = mBatteryManager.computeChargeTimeRemaining();
                if (ms > 0) seconds = ms / 1000;
            }
        }
        CharSequence etaLabel = getString(to80 ? R.string.hero_to80 : R.string.hero_full_in);
        CharSequence eta = seconds > 0 ? formatMinutes(seconds) : "--";

        mStatus.setSpeed(speedFor(currentMode()));
        mStatus.update(percent, charging, state, power,
                String.format(Locale.getDefault(), "%.1f°C", temp), etaLabel, eta, details);

        // Wake the halo when the charging state changes and holds for 3 polls
        if (mShownCharging == null) {
            mShownCharging = charging;
        } else if (charging != mShownCharging) {
            if (++mStateRun >= 3) {
                mShownCharging = charging;
                mStateRun = 0;
                mStatus.wake();
            }
        } else {
            mStateRun = 0;
        }
    }

    private String formatMinutes(long seconds) {
        long minutes = Math.max(1, Math.round(seconds / 60.0));
        return minutes >= 60
                ? getString(R.string.hero_hours, minutes / 60, minutes % 60)
                : getString(R.string.hero_minutes, minutes);
    }

    private static float speedFor(String mode) {
        switch (mode) {
            case MODE_FAST: return 1f;
            case MODE_SLOW: return 0.45f;
            default: return 2.2f;
        }
    }

    private static String currentMode() {
        String mode = SystemProperties.get(PROP_MODE, MODE_SUPER);
        for (String m : MODES) {
            if (m.equals(mode)) return mode;
        }
        return MODE_SUPER;
    }

    private void updateModes(String selected) {
        for (String mode : MODES) {
            SelectorWithWidgetPreference pref = findPreference(mode);
            if (pref != null) pref.setChecked(mode.equals(selected));
        }
    }

    private static boolean setProp(String key, String value) {
        try {
            SystemProperties.set(key, value);
            return true;
        } catch (RuntimeException e) {
            Log.e(TAG, "Failed to set " + key + " to " + value, e);
            return false;
        }
    }
}

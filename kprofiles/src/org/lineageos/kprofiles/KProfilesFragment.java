/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.kprofiles;

import android.content.ComponentName;
import android.os.Bundle;
import android.service.quicksettings.TileService;

import com.android.settingslib.widget.SelectorWithWidgetPreference;
import com.android.settingslib.widget.SettingsBasePreferenceFragment;

/** Settings > Battery > Performance profile. */
public class KProfilesFragment extends SettingsBasePreferenceFragment
        implements SelectorWithWidgetPreference.OnClickListener {

    @Override
    public void onCreatePreferences(Bundle savedInstanceState, String rootKey) {
        addPreferencesFromResource(R.xml.kprofiles);
        for (String mode : Profiles.ALL) {
            SelectorWithWidgetPreference pref = findPreference(mode);
            if (pref != null) pref.setOnClickListener(this);
        }
    }

    @Override
    public void onResume() {
        super.onResume();
        // The tile may have changed it meanwhile
        updateChecked(Profiles.get());
    }

    @Override
    public void onRadioButtonClicked(SelectorWithWidgetPreference pref) {
        String mode = pref.getKey();
        if (!Profiles.set(mode)) {
            // Keep showing what is actually in effect
            mode = Profiles.get();
        }
        updateChecked(mode);
        TileService.requestListeningState(requireContext(),
                new ComponentName(requireContext(), KProfilesTileService.class));
    }

    private void updateChecked(String selected) {
        for (String mode : Profiles.ALL) {
            SelectorWithWidgetPreference pref = findPreference(mode);
            if (pref != null) pref.setChecked(mode.equals(selected));
        }
    }
}

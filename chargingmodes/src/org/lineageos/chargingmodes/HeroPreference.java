/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.chargingmodes;

import android.content.Context;
import android.util.AttributeSet;
import android.widget.TextView;

import androidx.preference.Preference;
import androidx.preference.PreferenceViewHolder;

/** The top of the page: the halo plus power, temperature and time chips. */
public class HeroPreference extends Preference {

    private EmberHaloView mHalo;
    private TextView mState, mPower, mTemp, mEtaLabel, mEta, mDetails;

    private int mLevel = -1;
    private boolean mCharging;
    private float mSpeed = 1f;
    private CharSequence mStateText, mPowerText, mTempText, mEtaLabelText, mEtaText, mDetailsText;
    private boolean mWakePending = true;

    public HeroPreference(Context context, AttributeSet attrs) {
        super(context, attrs);
        setLayoutResource(R.layout.charging_hero);
        setSelectable(false);
        setPersistent(false);
    }

    @Override
    public void onBindViewHolder(PreferenceViewHolder holder) {
        super.onBindViewHolder(holder);
        holder.setDividerAllowedAbove(false);
        holder.setDividerAllowedBelow(false);
        mHalo = (EmberHaloView) holder.findViewById(R.id.halo);
        mState = (TextView) holder.findViewById(R.id.state);
        mPower = (TextView) holder.findViewById(R.id.power);
        mTemp = (TextView) holder.findViewById(R.id.temp);
        mEtaLabel = (TextView) holder.findViewById(R.id.eta_label);
        mEta = (TextView) holder.findViewById(R.id.eta);
        mDetails = (TextView) holder.findViewById(R.id.details);
        apply();
        if (mWakePending) mHalo.post(this::wake);
    }

    /** Updates the shown values; cheap when nothing changed. */
    public void update(int level, boolean charging, CharSequence state, CharSequence power,
            CharSequence temp, CharSequence etaLabel, CharSequence eta, CharSequence details) {
        mLevel = level;
        mCharging = charging;
        mStateText = state;
        mPowerText = power;
        mTempText = temp;
        mEtaLabelText = etaLabel;
        mEtaText = eta;
        mDetailsText = details;
        apply();
    }

    public void setSpeed(float speed) {
        mSpeed = speed;
        if (mHalo != null) mHalo.setSpeed(speed);
    }

    /** Turns the halo for a few seconds (or as soon as it is shown). */
    public void wake() {
        if (mHalo == null || !mHalo.isAttachedToWindow()) {
            mWakePending = true;
            return;
        }
        mWakePending = false;
        mHalo.wake();
    }

    private void apply() {
        if (mHalo == null) return;
        mHalo.setSpeed(mSpeed);
        mHalo.setState(mLevel, mCharging);
        setText(mState, mStateText);
        setText(mPower, mPowerText);
        setText(mTemp, mTempText);
        setText(mEtaLabel, mEtaLabelText);
        setText(mEta, mEtaText);
        setText(mDetails, mDetailsText);
    }

    private static void setText(TextView view, CharSequence text) {
        if (view == null || text == null) return;
        // Avoid relayouts every second when the value is the same
        if (!text.toString().contentEquals(view.getText())) view.setText(text);
    }
}

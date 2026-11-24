/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.chargingmodes;

import android.animation.ValueAnimator;
import android.content.Context;
import android.content.res.Configuration;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RadialGradient;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.os.Handler;
import android.os.Looper;
import android.util.AttributeSet;
import android.view.View;
import android.view.animation.LinearInterpolator;

/**
 * The "Ember Halo" hero: two rings of dots turning around a glowing core that
 * shows the battery level.
 *
 * Colours come from the wallpaper (Monet) accent. To keep the GPU idle the
 * rings only turn for a few seconds after {@link #wake()} (page shown, mode
 * changed, charger plugged or unplugged); then the halo stays still.
 */
public class EmberHaloView extends View {

    private static final long SHOW_MS = 8000;
    // Ring radius (fraction of the view's half size), dot count, dot radius (dp),
    // turns per base period, alpha.
    private static final float[][] RINGS = {
            {0.62f, 20, 3.4f, 1f, 0.9f},
            {0.82f, 28, 2.8f, -0.7f, 0.55f},
    };

    private final Paint mDot = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mGlow = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mCore = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mText = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mUnit = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mBolt = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path mBoltPath = new Path();
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private final Runnable mRest = this::rest;
    private final float mDp;

    private ValueAnimator mAnimator;
    private float mPhase;
    private float mSpeed = 1f;
    private int mLevel = -1;
    private boolean mCharging;
    private int mAccent, mAccentLight, mAccentDark, mOnCore;
    private int mLastW, mLastH;

    public EmberHaloView(Context context) {
        this(context, null);
    }

    public EmberHaloView(Context context, AttributeSet attrs) {
        super(context, attrs);
        mDp = getResources().getDisplayMetrics().density;
        mText.setTextAlign(Paint.Align.CENTER);
        mText.setTypeface(Typeface.create("sans-serif", Typeface.BOLD));
        mUnit.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        loadColors();
    }

    private void loadColors() {
        Context c = getContext();
        boolean night = (c.getResources().getConfiguration().uiMode
                & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES;
        mAccent = c.getColor(night ? android.R.color.system_accent1_300
                : android.R.color.system_accent1_500);
        mAccentLight = c.getColor(night ? android.R.color.system_accent1_100
                : android.R.color.system_accent1_200);
        mAccentDark = c.getColor(night ? android.R.color.system_accent1_700
                : android.R.color.system_accent1_800);
        mOnCore = c.getColor(night ? android.R.color.system_accent1_900
                : android.R.color.system_accent1_1000);
        mDot.setColor(mAccent);
        mText.setColor(mOnCore);
        mUnit.setColor(mOnCore);
        mBolt.setColor(mOnCore);
        mLastW = mLastH = 0;  // rebuild the shaders
    }

    /** Level in percent and whether the battery is charging. */
    public void setState(int level, boolean charging) {
        if (level == mLevel && charging == mCharging) return;
        mLevel = level;
        mCharging = charging;
        invalidate();
    }

    /** Super turns fastest, Slow slowest. */
    public void setSpeed(float speed) {
        mSpeed = speed;
    }

    /** Turns the rings for a few seconds, then stops them. */
    public void wake() {
        if (!ValueAnimator.areAnimatorsEnabled() || !isAttachedToWindow()) return;
        if (mAnimator == null) {
            mAnimator = ValueAnimator.ofFloat(0f, 1f);
            mAnimator.setDuration(24000);
            mAnimator.setRepeatCount(ValueAnimator.INFINITE);
            mAnimator.setInterpolator(new LinearInterpolator());
            mAnimator.addUpdateListener(a -> {
                mPhase = (Float) a.getAnimatedValue();
                invalidate();
            });
        }
        if (mAnimator.isPaused()) {
            mAnimator.resume();
        } else if (!mAnimator.isStarted()) {
            mAnimator.setCurrentFraction(mPhase);
            mAnimator.start();
        }
        mHandler.removeCallbacks(mRest);
        mHandler.postDelayed(mRest, SHOW_MS);
    }

    private void rest() {
        if (mAnimator != null && mAnimator.isRunning()) mAnimator.pause();
    }

    @Override
    protected void onDetachedFromWindow() {
        super.onDetachedFromWindow();
        mHandler.removeCallbacks(mRest);
        if (mAnimator != null) {
            mAnimator.cancel();
            mAnimator = null;
        }
    }

    @Override
    protected void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        loadColors();
        invalidate();
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int w = MeasureSpec.getSize(widthMeasureSpec);
        int h = Math.round(260 * mDp);
        setMeasuredDimension(w, h);
    }

    private void buildShaders(int w, int h) {
        float cx = w / 2f, cy = h / 2f, half = Math.min(w, h) / 2f;
        float core = half * 0.42f;
        mGlow.setShader(new RadialGradient(cx, cy, half,
                new int[] {withAlpha(mAccent, 0.45f), withAlpha(mAccent, 0.12f), Color.TRANSPARENT},
                new float[] {0.3f, 0.6f, 1f}, Shader.TileMode.CLAMP));
        mCore.setShader(new RadialGradient(cx, cy - core * 0.45f, core * 1.6f,
                new int[] {mAccentLight, mAccent, mAccentDark},
                new float[] {0f, 0.5f, 1f}, Shader.TileMode.CLAMP));
        mText.setTextSize(core * 0.62f);
        mUnit.setTextSize(core * 0.24f);
        mLastW = w;
        mLastH = h;
    }

    @Override
    protected void onDraw(Canvas canvas) {
        int w = getWidth(), h = getHeight();
        if (w == 0 || h == 0) return;
        if (w != mLastW || h != mLastH) buildShaders(w, h);
        float cx = w / 2f, cy = h / 2f, half = Math.min(w, h) / 2f;

        // Soft glow behind everything (dimmer when not charging)
        mGlow.setAlpha(mCharging ? 255 : 150);
        canvas.drawCircle(cx, cy, half, mGlow);

        // Rings of dots
        for (float[] ring : RINGS) {
            float r = half * ring[0];
            int n = (int) ring[1];
            float dot = ring[2] * mDp;
            float turn = (mPhase * ring[3] * mSpeed * 360f) % 360f;
            mDot.setAlpha(Math.round(255 * ring[4] * (mCharging ? 1f : 0.6f)));
            canvas.save();
            canvas.rotate(turn, cx, cy);
            for (int i = 0; i < n; i++) {
                double a = 2 * Math.PI * i / n;
                // Every third dot a little bigger, like the web mockup's twinkle
                float s = i % 3 == 0 ? dot * 1.3f : dot;
                canvas.drawCircle(cx + (float) (r * Math.cos(a)), cy + (float) (r * Math.sin(a)),
                        s, mDot);
            }
            canvas.restore();
        }

        // Core
        float core = half * 0.42f;
        canvas.drawCircle(cx, cy, core, mCore);

        String num = mLevel >= 0 ? String.valueOf(mLevel) : "--";
        float numW = mText.measureText(num);
        float unitW = mUnit.measureText("%");
        float baseline = cy + mText.getTextSize() * 0.36f;
        canvas.drawText(num, cx - unitW / 2f, baseline, mText);
        canvas.drawText("%", cx - unitW / 2f + numW / 2f + 2 * mDp, baseline, mUnit);

        if (mCharging) {
            float bs = core * 0.22f, bx = cx, by = baseline - mText.getTextSize() - bs * 0.1f;
            mBoltPath.reset();
            mBoltPath.moveTo(bx + bs * 0.15f, by - bs);
            mBoltPath.lineTo(bx - bs * 0.55f, by + bs * 0.15f);
            mBoltPath.lineTo(bx - bs * 0.05f, by + bs * 0.15f);
            mBoltPath.lineTo(bx - bs * 0.2f, by + bs);
            mBoltPath.lineTo(bx + bs * 0.55f, by - bs * 0.2f);
            mBoltPath.lineTo(bx + bs * 0.05f, by - bs * 0.2f);
            mBoltPath.close();
            canvas.drawPath(mBoltPath, mBolt);
        }
    }

    private static int withAlpha(int color, float alpha) {
        return Color.argb(Math.round(255 * alpha), Color.red(color), Color.green(color),
                Color.blue(color));
    }
}

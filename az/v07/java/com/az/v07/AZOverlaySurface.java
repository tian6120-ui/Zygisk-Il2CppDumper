package com.az.v07;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.graphics.PixelFormat;
import android.util.Log;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.widget.FrameLayout;
import java.lang.ref.WeakReference;

public final class AZOverlaySurface {
    private static final String TAG = "AZSURF07";
    private static WeakReference<Activity> activity = new WeakReference<>(null);
    private static SurfaceView renderView;
    private static View touchView;
    private static boolean dragging = false;
    private static final float[] TOUCH = new float[] {0,0,0,0};

    private AZOverlaySurface() {}

    public static native void nativeSurfaceCreated(Surface surface);
    public static native void nativeSurfaceChanged(int width, int height);
    public static native void nativeSurfaceDestroyed();
    public static native boolean nativeTouch(int action, float x, float y);
    public static native void nativeScroll(float dy);
    public static native void nativeLongPress(float x, float y);
    public static native void nativeGetTouchRegion(float[] out4);
    public static native void nativeAddChar(int codepoint);

    public static void show(final Activity a) {
        if (a == null) return;
        a.runOnUiThread(() -> attach(a));
    }

    public static void hide(final Activity a) {
        if (a == null) return;
        a.runOnUiThread(() -> {
            Activity cur = activity.get();
            if (cur == a) detach();
        });
    }

    private static void attach(Activity a) {
        Activity old = activity.get();
        if (old != null && old != a) detach();
        if (renderView != null && renderView.isAttachedToWindow()) return;

        Window w = a.getWindow();
        if (w == null) return;
        View decor = w.getDecorView();
        if (!(decor instanceof ViewGroup)) return;
        ViewGroup root = (ViewGroup) decor;

        activity = new WeakReference<>(a);

        SurfaceView sv = new SurfaceView(a);
        sv.setZOrderOnTop(true);
        sv.setZOrderMediaOverlay(true);
        sv.getHolder().setFormat(PixelFormat.TRANSLUCENT);
        sv.setClickable(false);
        sv.setFocusable(false);
        sv.setBackgroundColor(0x00000000);
        sv.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder h) {
                Log.i(TAG, "surfaceCreated");
                nativeSurfaceCreated(h.getSurface());
            }
            @Override public void surfaceChanged(SurfaceHolder h, int format, int width, int height) {
                nativeSurfaceChanged(width, height);
            }
            @Override public void surfaceDestroyed(SurfaceHolder h) {
                nativeSurfaceDestroyed();
            }
        });

        View tv = new View(a);
        tv.setBackgroundColor(0x00000000);
        tv.setClickable(true);
        tv.setFocusable(false);
        tv.setOnTouchListener((v, e) -> {
            float x = e.getX(), y = e.getY();
            int action = e.getActionMasked();
            nativeGetTouchRegion(TOUCH);
            boolean inside = x >= TOUCH[0] && y >= TOUCH[1] && x <= TOUCH[2] && y <= TOUCH[3];
            if (action == MotionEvent.ACTION_DOWN) dragging = inside;
            boolean capture = inside || dragging;
            if (!capture) return false;
            boolean handled = nativeTouch(action, x, y);
            if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) dragging = false;
            return handled || capture;
        });
        tv.setOnGenericMotionListener((v, e) -> {
            if (e.getActionMasked() == MotionEvent.ACTION_SCROLL) {
                float x=e.getX(), y=e.getY();
                nativeGetTouchRegion(TOUCH);
                if (x >= TOUCH[0] && y >= TOUCH[1] && x <= TOUCH[2] && y <= TOUCH[3]) {
                    nativeScroll(e.getAxisValue(MotionEvent.AXIS_VSCROLL));
                    return true;
                }
            }
            return false;
        });

        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT, Gravity.TOP|Gravity.LEFT);
        root.addView(sv, lp);
        root.addView(tv, lp);
        renderView = sv;
        touchView = tv;
        Log.i(TAG, "overlay attached");
    }

    private static void detach() {
        try {
            if (renderView != null) {
                removeFromParent(renderView);
                renderView = null;
            }
            if (touchView != null) {
                removeFromParent(touchView);
                touchView = null;
            }
        } catch (Throwable t) {
            Log.e(TAG, "detach failed", t);
        }
        activity = new WeakReference<>(null);
    }

    private static void removeFromParent(View v) {
        if (v == null || v.getParent() == null) return;
        if (v.getParent() instanceof ViewGroup) ((ViewGroup)v.getParent()).removeView(v);
    }

    public static String clipboardGet() {
        Activity a = activity.get();
        if (a == null) return "";
        try {
            ClipboardManager cm = (ClipboardManager)a.getSystemService(Context.CLIPBOARD_SERVICE);
            if (cm == null || !cm.hasPrimaryClip() || cm.getPrimaryClip() == null || cm.getPrimaryClip().getItemCount() == 0) return "";
            CharSequence cs = cm.getPrimaryClip().getItemAt(0).coerceToText(a);
            return cs == null ? "" : cs.toString();
        } catch (Throwable t) { return ""; }
    }

    public static void clipboardSet(String s) {
        Activity a = activity.get();
        if (a == null) return;
        try {
            ClipboardManager cm = (ClipboardManager)a.getSystemService(Context.CLIPBOARD_SERVICE);
            if (cm != null) cm.setPrimaryClip(ClipData.newPlainText("AZ", s == null ? "" : s));
        } catch (Throwable ignored) {}
    }
}

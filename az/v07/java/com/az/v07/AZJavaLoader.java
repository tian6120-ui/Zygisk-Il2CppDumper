package com.az.v07;

import android.app.Activity;
import android.app.Application;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import java.lang.ref.WeakReference;

public final class AZJavaLoader {
    private static final String TAG = "AZJ07";
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static volatile WeakReference<Activity> resumed = new WeakReference<>(null);
    private static volatile int generation = 0;
    private static volatile boolean registered = false;

    private AZJavaLoader() {}

    public static native boolean nativeLoadCore(Object activity);

    public static void init(Application app) {
        if (app == null || registered) return;
        registered = true;
        Log.i(TAG, "init application=" + app.getPackageName());
        app.registerActivityLifecycleCallbacks(new Application.ActivityLifecycleCallbacks() {
            @Override public void onActivityCreated(Activity a, Bundle b) {}
            @Override public void onActivityStarted(Activity a) {}
            @Override public void onActivitySaveInstanceState(Activity a, Bundle b) {}
            @Override public void onActivityStopped(Activity a) {}

            @Override public void onActivityResumed(Activity a) {
                int g = ++generation;
                resumed = new WeakReference<>(a);
                Log.i(TAG, "resumed generation=" + g + " activity=" + a);
                scheduleInject(a, g, 650);
            }

            @Override public void onActivityPaused(Activity a) {
                Activity cur = resumed.get();
                if (cur == a) {
                    ++generation;
                    AZOverlaySurface.hide(a);
                }
            }

            @Override public void onActivityDestroyed(Activity a) {
                Activity cur = resumed.get();
                if (cur == a) {
                    ++generation;
                    resumed = new WeakReference<>(null);
                    AZOverlaySurface.hide(a);
                }
            }
        });
    }

    private static void scheduleInject(final Activity a, final int g, long delayMs) {
        MAIN.postDelayed(() -> tryInject(a, g), delayMs);
    }

    private static void tryInject(final Activity a, final int g) {
        if (a == null || g != generation || resumed.get() != a) {
            Log.i(TAG, "inject skip: stale activity generation=" + g);
            return;
        }
        a.runOnUiThread(() -> {
            try {
                if (g != generation || resumed.get() != a || a.isFinishing() || a.isDestroyed()) {
                    Log.i(TAG, "inject skip: activity no longer resumed");
                    return;
                }
                if (a.getWindow() == null || a.getWindow().getDecorView() == null) {
                    Log.i(TAG, "inject wait: decor unavailable");
                    scheduleInject(a, g, 350);
                    return;
                }
                View decor = a.getWindow().getDecorView();
                if (!decor.isAttachedToWindow()) {
                    Log.i(TAG, "inject wait: decor not attached");
                    scheduleInject(a, g, 350);
                    return;
                }
                Log.i(TAG, "nativeLoadCore begin");
                if (!nativeLoadCore(a)) {
                    Log.e(TAG, "nativeLoadCore returned false");
                    scheduleInject(a, g, 700);
                    return;
                }
                if (g != generation || resumed.get() != a) return;
                AZOverlaySurface.show(a);
                Log.i(TAG, "overlay requested");
            } catch (Throwable t) {
                Log.e(TAG, "inject failed", t);
                scheduleInject(a, g, 1000);
            }
        });
    }
}

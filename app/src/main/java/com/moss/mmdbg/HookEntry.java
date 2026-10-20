package com.moss.mmdbg;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.WeakHashMap;

import de.robv.android.xposed.IXposedHookLoadPackage;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import de.robv.android.xposed.XposedHelpers;
import de.robv.android.xposed.callbacks.XC_LoadPackage;

/**
 * MmdDbg - 调试外壳
 *
 * 这个 APK 只干一件事：把 /data/data/<目标包>/files/libmmdbg_latest.so
 * 复制成一个带序号的新名字再 dlopen 进去。
 *
 * 为什么要复制改名：dlopen 同一路径会直接返回缓存的句柄，改了 so 文件也没用。
 * 每次换新名字（libmmdbg_1.so、libmmdbg_2.so ...）就能热重载，
 * 改完 C++ 只要把新 so 推进去、点一下「重载 so」，连游戏都不用重启。
 *
 * 真正的逻辑全在 libmmdbg.so 里，模块本体不用再动。
 */
public class HookEntry implements IXposedHookLoadPackage {

    static final String TAG = "MmdDbg";
    static final String PKG = "com.hermes.h1game";
    static final String SO_NAME = "libmmdbg_latest.so";
    static final String[] DIRS = {
            "/data/data/com.hermes.h1game/files/",
            "/data/user/0/com.hermes.h1game/files/",
    };

    private static native void nativeTick();
    private static native String nativeStatus();

    static int loadSeq = 0;
    static Handler tickHandler = null;
    static int tickCount = 0;
    static String lastStatus = "尚未加载";
    static final WeakHashMap<Activity, LinearLayout> overlays = new WeakHashMap<>();
    static TextView statusView = null;

    @Override
    public void handleLoadPackage(XC_LoadPackage.LoadPackageParam lp) {
        if (!PKG.equals(lp.packageName)) return;
        XposedBridge.log(TAG + ": hooked " + lp.packageName + " pid=" + Process.myPid());
        try {
            Class<?> actCls = XposedHelpers.findClass("android.app.Activity", lp.classLoader);
            XposedBridge.hookAllMethods(actCls, "onCreate", new XC_MethodHook() {
                @Override
                protected void afterHookedMethod(MethodHookParam param) {
                    final Activity a = (Activity) param.thisObject;
                    try {
                        installOverlay(a);
                        // 第一个 Activity 出现时自动加载一次
                        if (loadSeq == 0) {
                            lastStatus = doLoad();
                            refreshStatus();
                        }
                    } catch (Throwable t) {
                        XposedBridge.log(TAG + ": overlay fail " + t);
                    }
                }
            });
        } catch (Throwable t) {
            XposedBridge.log(TAG + ": hook fail " + t);
        }
    }

    // ---------------- so 加载 ----------------

    static synchronized String doLoad() {
        File src = null;
        String dir = null;
        for (String d : DIRS) {
            File f = new File(d, SO_NAME);
            if (f.exists() && f.canRead()) { src = f; dir = d; break; }
        }
        if (src == null) {
            return "找不到 " + SO_NAME + "（应放在 " + DIRS[0] + "）";
        }
        String dstName = "libmmdbg_" + (++loadSeq) + ".so";
        File dst = new File(dir, dstName);
        try {
            copy(src, dst);
        } catch (Throwable t) {
            return "复制失败: " + t;
        }
        try {
            System.load(dst.getAbsolutePath());
            startTick();
            return "已加载 " + dstName + "  " + dst.length() + " 字节";
        } catch (Throwable t) {
            return "dlopen 失败: " + t;
        }
    }

    // 每帧调一次 nativeTick()。so 里所有 Unity 操作都在这个回调里做，
    // 天然就在主线程，不需要 inline hook。
    static void startTick() {
        if (tickHandler != null) return;
        tickHandler = new Handler(Looper.getMainLooper());
        tickHandler.post(new Runnable() {
            @Override public void run() {
                tickCount++;
                try { nativeTick(); } catch (Throwable ignored) { }
                if (tickCount % 60 == 0) {
                    try { lastStatus = nativeStatus(); refreshStatus(); } catch (Throwable ignored) { }
                }
                tickHandler.postDelayed(this, 16);
            }
        });
    }

    static void copy(File src, File dst) throws Exception {
        InputStream in = new FileInputStream(src);
        OutputStream out = new FileOutputStream(dst);
        try {
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        } finally {
            try { in.close(); } catch (Throwable ignored) { }
            try { out.close(); } catch (Throwable ignored) { }
        }
        try { dst.setReadable(true, false); } catch (Throwable ignored) { }
    }

    // ---------------- 悬浮窗 ----------------

    static void installOverlay(final Activity a) {
        if (overlays.containsKey(a)) return;

        LinearLayout root = new LinearLayout(a);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(14, 10, 14, 10);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0xE6121219);
        bg.setCornerRadius(16f);
        bg.setStroke(2, 0x66FF8A65);
        root.setBackground(bg);

        TextView title = new TextView(a);
        title.setText("MmdDbg  调试壳");
        title.setTextColor(0xFFFF8A65);
        title.setTextSize(12);
        root.addView(title);

        final TextView status = new TextView(a);
        status.setTextColor(0xFF7FD4FF);
        status.setTextSize(10);
        status.setText(lastStatus);
        status.setPadding(0, 4, 0, 6);
        root.addView(status);
        statusView = status;

        LinearLayout row = new LinearLayout(a);
        row.setOrientation(LinearLayout.HORIZONTAL);

        TextView reload = mkBtn(a, "重载 so");
        reload.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                lastStatus = doLoad();
                refreshStatus();
            }
        });
        row.addView(reload, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        TextView where = mkBtn(a, "路径");
        where.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                StringBuilder sb = new StringBuilder();
                for (String d : DIRS) {
                    File f = new File(d, SO_NAME);
                    sb.append(f.exists() ? "有 " : "无 ").append(f.getAbsolutePath()).append('\n');
                }
                lastStatus = sb.toString();
                refreshStatus();
            }
        });
        row.addView(where, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        root.addView(row);

        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.leftMargin = 36;
        lp.topMargin = 300;

        // 挂到 DecorView，避免被 content view 裁剪
        ViewGroup decor = null;
        try { decor = (ViewGroup) a.getWindow().getDecorView(); } catch (Throwable ignored) { }
        if (decor != null) decor.addView(root, lp);
        else a.addContentView(root, lp);

        overlays.put(a, root);
    }

    static void refreshStatus() {
        if (statusView == null) return;
        statusView.post(new Runnable() {
            @Override public void run() {
                try { statusView.setText(lastStatus); } catch (Throwable ignored) { }
            }
        });
    }

    static TextView mkBtn(Activity a, String text) {
        TextView tv = new TextView(a);
        tv.setText(text);
        tv.setTextColor(0xFFFFD54F);
        tv.setTextSize(12);
        tv.setGravity(Gravity.CENTER);
        tv.setPadding(18, 14, 18, 14);
        GradientDrawable b = new GradientDrawable();
        b.setColor(0x33FF8A65);
        b.setCornerRadius(12f);
        b.setStroke(1, 0xAAFF8A65);
        tv.setBackground(b);
        return tv;
    }
}

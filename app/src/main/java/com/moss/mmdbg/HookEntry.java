package com.moss.mmdbg;

import android.app.Activity;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.view.View;
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
    static final String SO_PREFIX = "libmmdbg_";   // 扫描这个前缀的 .so，丢掉 __NAME__ 里都不需要改 APK
    static final String SO_NAME = "libmmdbg_latest.so";   // 旧的单文件模式，兼容保留
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
    static final WeakHashMap<Activity, FloatPanel> panels = new WeakHashMap<>();

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
                    if (a == null) return;
                    try { if (a.getWindow() == null) return; } catch (Throwable ignored) { return; }

                    if (loadSeq == 0 && isMainProcess()) {
                        lastStatus = doLoadAll();
                    }

                    try {
                        FloatPanel.Theme th = new FloatPanel.Theme();
                        th.title = "MmdDbg";
                        th.accent = 0xFFFF8A65;
                        th.accentDim = 0x66FF8A65;
                        th.textMain = 0xFFFFD54F;
                        th.textSub = 0xFF7FD4FF;
                        th.btnText = 0xFFFFD54F;
                        final FloatPanel panel = FloatPanel.get(a, th);
                        panels.put(a, panel);
                        panel.setLog(lastStatus);

                        if (panels.size() <= 1) {
                            panel.clearButtons();
                            TextView reload = panel.addButton("重载 so");
                            reload.setOnClickListener(new View.OnClickListener() {
                                @Override public void onClick(View v) {
                                    lastStatus = doLoadAll();
                                    panel.setLog(lastStatus);
                                }
                            });
                            TextView where = panel.addButton("路径");
                            where.setOnClickListener(new View.OnClickListener() {
                                @Override public void onClick(View v) {
                                    StringBuilder sb = new StringBuilder();
                                    File d0 = new File(DIRS[0]);
                                    File[] fs = d0.listFiles();
                                    if (fs == null) {
                                        sb.append("目录不可读: ").append(DIRS[0]);
                                    } else {
                                        for (File f : fs) {
                                            String n = f.getName();
                                            if (n.startsWith(SO_PREFIX) && n.endsWith(".so"))
                                                sb.append(n).append("  ").append(f.length()).append('\n');
                                        }
                                        if (sb.length() == 0) sb.append("空: ").append(DIRS[0]);
                                    }
                                    lastStatus = sb.toString();
                                    panel.setLog(lastStatus);
                                }
                            });
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

    static boolean isMainProcess() {
        try {
            java.io.BufferedReader r = new java.io.BufferedReader(
                    new java.io.FileReader("/proc/self/cmdline"));
            String s = r.readLine();
            r.close();
            return s != null && !s.contains(":");
        } catch (Throwable t) {
            return true;
        }
    }

    // 扫描 <dir>/libmmdbg_<模块名>.so，一个个复制成带序号的新名字再 load。
    // 改名是为了绕开 dlopen 的路径缓存，这样同一个模块也能重载。
    static synchronized String doLoadAll() {
        String dir = null;
        for (String d : DIRS) {
            File dd = new File(d);
            if (dd.isDirectory() && dd.canRead()) { dir = d; break; }
        }
        if (dir == null) return "目录不可读: " + DIRS[0];

        File d = new File(dir);
        File[] all = d.listFiles();
        if (all == null) return "列目录失败";

        java.util.ArrayList<File> mods = new java.util.ArrayList<>();
        for (File f : all) {
            String n = f.getName();
            if (!f.isFile()) continue;
            if (!n.startsWith(SO_PREFIX) || !n.endsWith(".so")) continue;
            // 只认 libmmdbg_<纯字母>.so，跳过我们自己生成的 libmmdbg_x_1.so
            String mid = n.substring(SO_PREFIX.length(), n.length() - 3);
            if (mid.isEmpty()) continue;
            boolean plain = true;
            for (int i = 0; i < mid.length(); i++) {
                char c = mid.charAt(i);
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) { plain = false; break; }
            }
            if (!plain) continue;
            mods.add(f);
        }
        // 兼容旧的单文件
        File legacy = new File(dir, SO_NAME);
        if (mods.isEmpty() && legacy.exists()) mods.add(legacy);
        if (mods.isEmpty()) return "目录里没有 " + SO_PREFIX + "*.so";

        java.util.Collections.sort(mods, new java.util.Comparator<File>() {
            @Override public int compare(File a, File b) { return a.getName().compareTo(b.getName()); }
        });

        int seq = ++loadSeq;
        StringBuilder sb = new StringBuilder();
        int ok = 0;
        for (File f : mods) {
            String base = f.getName().substring(0, f.getName().length() - 3);
            String dstName = base + "_" + seq + ".so";
            File dst = new File(dir, dstName);
            try {
                copy(f, dst);
                System.load(dst.getAbsolutePath());
                ok++;
                sb.append("OK ").append(base).append("\n");
            } catch (Throwable t) {
                sb.append("FAIL ").append(base).append(": ").append(t).append("\n");
            }
        }
        if (ok > 0) startTick();
        return "第 " + seq + " 轮 " + ok + "/" + mods.size() + " 个模块\n" + sb;
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

    static void refreshStatus() {
        for (FloatPanel p : panels.values()) p.setLog(lastStatus);
    }
}

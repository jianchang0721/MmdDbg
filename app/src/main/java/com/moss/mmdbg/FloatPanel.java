package com.moss.mmdbg;

import android.app.Activity;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewParent;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.WeakHashMap;

/**
 * 通用悬浮面板（两个模块共用这套逻辑，MmdDbg 侧是同样代码换包名）。
 *
 * 结构：磁吸球（可拖拽、松手吸附屏幕边缘、点一下展开）
 *      → 展开后是一块深色圆角面板：标题 + 日志区 + 按钮行
 *      → 面板标题栏可拖走、可点「—」收回成球。
 *
 * 交互抄的是 EnFloatingView（leotyndale/EnFloatingView，1.2k star）
 * 的磁力吸附思路：DOWN 记位置、MOVE 跟手、UP 按当前速度吸附最近的边。
 * 没有用它的库本身 —— 那个库带 Gradle 依赖和资源文件，塞不进注入式模块。
 *
 * 用途约束：挂在 Activity 的 DecorView 上，应用内显示，零权限。
 */
public class FloatPanel {

    // ---------------- 主题（两个模块各自传入品牌色） ----------------
    public static class Theme {
        public int accent   = 0xFF7C4DFF;   // 强调色（标题/边框/球）
        public int accentDim = 0x667C4DFF;  // 半透明边框
        public int textMain = 0xFFD8CCFF;   // 主要文字
        public int textSub  = 0xFF7FD4FF;   // 日志文字
        public int btnText  = 0xFFE0B9FF;   // 按钮文字
        public String title = "Panel";
        public Theme() {}
    }

    // ---------------- 状态 ----------------
    private final Activity mActivity;
    private final Theme mTheme;
    private FrameLayout mBall;          // 收起态
    private LinearLayout mPanel;        // 展开态
    private TextView mLogView;
    private ScrollView mScroller;
    private boolean mExpanded = false;

    private static final WeakHashMap<Activity, FloatPanel> INSTANCES = new WeakHashMap<>();

    public static FloatPanel get(Activity a, Theme t) {
        FloatPanel p = INSTANCES.get(a);
        if (p == null) { p = new FloatPanel(a, t); INSTANCES.put(a, p); }
        return p;
    }

    private FloatPanel(Activity a, Theme t) {
        mActivity = a;
        mTheme = t;
        buildBall();
    }

    // =====================================================================
    // 磁吸球
    // =====================================================================
    private void buildBall() {
        mBall = new FrameLayout(mActivity);

        int s = dp(44);
        GradientDrawable g = new GradientDrawable();
        g.setShape(GradientDrawable.OVAL);
        g.setColor(0xE614141E);
        g.setStroke(dp(2), mTheme.accent);
        mBall.setBackground(g);

        TextView dot = new TextView(mActivity);
        dot.setText(mTheme.title.length() > 0 ? mTheme.title.substring(0, 1) : "M");
        dot.setTextColor(mTheme.accent);
        dot.setTextSize(16);
        dot.setGravity(Gravity.CENTER);
        mBall.addView(dot, new FrameLayout.LayoutParams(-1, -1));

        mBall.setOnTouchListener(new DragTouch(mBall, new Runnable() {
            @Override public void run() { toggle(); }
        }));

        attach(mBall, dp(20), dp(320), s, s);
    }

    // =====================================================================
    // 展开面板
    // =====================================================================
    private void buildPanel() {
        mPanel = new LinearLayout(mActivity);
        mPanel.setOrientation(LinearLayout.VERTICAL);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0xF010101A);
        bg.setCornerRadius(dp(16));
        bg.setStroke(dp(1), mTheme.accentDim);
        mPanel.setBackground(bg);

        // —— 标题栏（拖拽手柄 + 收起按钮）——
        LinearLayout head = new LinearLayout(mActivity);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setPadding(dp(12), dp(8), dp(8), dp(8));
        head.setGravity(Gravity.CENTER_VERTICAL);

        TextView title = new TextView(mActivity);
        title.setText(mTheme.title);
        title.setTextColor(mTheme.accent);
        title.setTextSize(13);
        head.addView(title, new LinearLayout.LayoutParams(0, -2, 1f));

        TextView close = mkBtn("—", dp(34), dp(28));
        close.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { toggle(); }
        });
        head.addView(close);

        mPanel.addView(head, new LinearLayout.LayoutParams(-1, -2));

        // —— 日志区 ——
        mLogView = new TextView(mActivity);
        mLogView.setTextColor(mTheme.textSub);
        mLogView.setTextSize(10);
        mLogView.setText("...");
        mLogView.setPadding(dp(12), 0, dp(12), dp(8));

        mScroller = new ScrollView(mActivity);
        mScroller.addView(mLogView);
        LinearLayout.LayoutParams logLp = new LinearLayout.LayoutParams(-1, 0, 1f);
        mPanel.addView(mScroller, logLp);

        attach(mPanel, dp(16), dp(120), dp(270), dp(300));
    }

    /** 展开态按钮由调用方通过 addButton 往面板里塞。 */
    public void clearButtons() {
        if (mPanel == null) return;
        // 标题栏是 child 0，日志区是 child 1；按钮区从 2 开始全删
        while (mPanel.getChildCount() > 2) mPanel.removeViewAt(2);
    }

    public TextView addButton(String text) {
        if (mPanel == null) buildPanel();
        LinearLayout row = null;
        int n = mPanel.getChildCount();
        if (n > 2 && mPanel.getChildAt(n - 1) instanceof LinearLayout) {
            row = (LinearLayout) mPanel.getChildAt(n - 1);
        } else {
            row = new LinearLayout(mActivity);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setPadding(dp(8), 0, dp(8), dp(10));
            mPanel.addView(row, new LinearLayout.LayoutParams(-1, -2));
        }
        TextView b = mkBtn(text, 0, dp(32));
        b.setOnClickListener(null);   // 调用方自己 set
        row.addView(b, new LinearLayout.LayoutParams(0, -2, 1f));
        ((LinearLayout.LayoutParams) b.getLayoutParams()).leftMargin = dp(6);
        return b;
    }

    // =====================================================================
    // 展开 / 收起
    // =====================================================================
    private void toggle() {
        if (mExpanded) {
            // 记住面板位置，把球放回原处
            remove(mPanel); mPanel = null; mLogView = null; mScroller = null;
            mExpanded = false;
            buildBall();
            mBall.setX(mLastX); mBall.setY(mLastY);
        } else {
            float x = mBall.getX(), y = mBall.getY();
            remove(mBall); mBall = null;
            mExpanded = true;
            buildPanel();
            mLastX = x; mLastY = y;
            mPanel.setX(Math.max(dp(8), Math.min(x - dp(100), screenW() - dp(278))));
            mPanel.setY(Math.max(dp(40), Math.min(y, screenH() - dp(310))));
        }
    }

    // =====================================================================
    // 日志
    // =====================================================================
    public void setLog(String s) {
        if (mLogView == null) return;
        mLogView.post(new Runnable() {
            @Override public void run() {
                try {
                    mLogView.setText(s);
                    mScroller.fullScroll(View.FOCUS_DOWN);
                } catch (Throwable ignored) { }
            }
        });
    }

    public void appendLog(String s) {
        if (mLogView == null) return;
        mLogView.post(new Runnable() {
            @Override public void run() {
                try {
                    mLogView.append(s + "\n");
                    mScroller.fullScroll(View.FOCUS_DOWN);
                } catch (Throwable ignored) { }
            }
        });
    }

    // =====================================================================
    // 拖拽 + 点击判定（EnFloatingView 的思路，重写）
    // =====================================================================
    private float mLastX = dp(20), mLastY = dp(320);

    private class DragTouch implements View.OnTouchListener {
        private final View mV;
        private final Runnable mOnClick;
        private float mDownRawX, mDownRawY, mDownX, mDownY;
        private long mDownTime;
        private boolean mMoved;

        DragTouch(View v, Runnable onClick) { mV = v; mOnClick = onClick; }

        @Override public boolean onTouch(View v, MotionEvent e) {
            switch (e.getAction()) {
                case MotionEvent.ACTION_DOWN:
                    mDownRawX = e.getRawX(); mDownRawY = e.getRawY();
                    mDownX = mV.getX();      mDownY = mV.getY();
                    mDownTime = System.currentTimeMillis();
                    mMoved = false;
                    return true;
                case MotionEvent.ACTION_MOVE: {
                    float nx = mDownX + e.getRawX() - mDownRawX;
                    float ny = mDownY + e.getRawY() - mDownRawY;
                    if (Math.abs(nx - mDownX) > dp(3) || Math.abs(ny - mDownY) > dp(3)) mMoved = true;
                    mV.setX(clamp(nx, 0, screenW() - mV.getWidth()));
                    mV.setY(clamp(ny, dp(24), screenH() - mV.getHeight() - dp(24)));
                    return true;
                }
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_CANCEL: {
                    if (!mMoved && System.currentTimeMillis() - mDownTime < 400) {
                        if (mOnClick != null) mOnClick.run();
                        return true;
                    }
                    // 吸附到最近的左右边缘
                    float targetX = (mV.getX() + mV.getWidth() / 2f) < screenW() / 2f
                            ? dp(8) : screenW() - mV.getWidth() - dp(8);
                    mLastX = targetX; mLastY = mV.getY();
                    mV.animate().x(targetX).setDuration(180).start();
                    return true;
                }
            }
            return false;
        }
    }

    // =====================================================================
    // 按钮工厂 + 工具
    // =====================================================================
    private TextView mkBtn(String text, int w, int h) {
        TextView tv = new TextView(mActivity);
        tv.setText(text);
        tv.setTextColor(mTheme.btnText);
        tv.setTextSize(11);
        tv.setGravity(Gravity.CENTER);
        tv.setPadding(dp(14), dp(6), dp(14), dp(6));
        GradientDrawable g = new GradientDrawable();
        g.setColor(0x26FFFFFF);
        g.setCornerRadius(dp(10));
        g.setStroke(dp(1), mTheme.accentDim);
        tv.setBackground(g);
        if (w > 0) tv.setMinWidth(w);
        if (h > 0) tv.setMinHeight(h);
        return tv;
    }

    private void attach(View v, int x, int y, int w, int h) {
        ViewGroup decor = null;
        try { decor = (ViewGroup) mActivity.getWindow().getDecorView(); } catch (Throwable ignored) { }
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(w, h);
        if (decor != null) decor.addView(v, lp);
        else mActivity.addContentView(v, lp);
        v.setX(x); v.setY(y);
        v.setClipToOutline(false);
        // 沿父链关掉裁剪，免得拖出 content 区域就消失
        try {
            ViewParent p = v.getParent();
            int d = 0;
            while (p instanceof ViewGroup && d++ < 10) {
                ((ViewGroup) p).setClipChildren(false);
                ((ViewGroup) p).setClipToPadding(false);
                p = p.getParent();
            }
        } catch (Throwable ignored) { }
    }

    private void remove(View v) {
        if (v == null) return;
        try { ((ViewGroup) v.getParent()).removeView(v); } catch (Throwable ignored) { }
    }

    private float clamp(float v, float lo, float hi) {
        return Math.max(lo, Math.min(v, hi));
    }

    private int screenW() {
        return mActivity.getResources().getDisplayMetrics().widthPixels;
    }

    private int screenH() {
        return mActivity.getResources().getDisplayMetrics().heightPixels;
    }

    private int dp(int v) {
        try {
            return (int) (v * mActivity.getResources().getDisplayMetrics().density + 0.5f);
        } catch (Throwable t) {
            return (int) (v * 1.5f);
        }
    }
}

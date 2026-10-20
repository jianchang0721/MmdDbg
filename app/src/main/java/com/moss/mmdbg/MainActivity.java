package com.moss.mmdbg;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;

public class MainActivity extends Activity {
    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        TextView tv = new TextView(this);
        tv.setText("MmdDbg 调试壳\n\n"
                + "这是个空壳模块，真正的逻辑在外部 so 里。\n\n"
                + "用法：\n"
                + "1. 把 libmmdbg.so 放到\n"
                + "   /data/data/com.hermes.h1game/files/libmmdbg_latest.so\n"
                + "2. 启动游戏，悬浮窗会自动加载\n"
                + "3. 改了 so 之后，推进去同名文件，点「重载 so」\n"
                + "   不需要重启游戏，也不需要重装这个模块\n\n"
                + "日志在 /data/data/com.hermes.h1game/files/mmdbg.log\n");
        tv.setTextSize(14);
        tv.setPadding(40, 80, 40, 40);
        setContentView(tv);
    }
}

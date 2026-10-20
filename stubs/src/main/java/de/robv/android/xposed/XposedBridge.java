package de.robv.android.xposed;

public final class XposedBridge {

    private XposedBridge() {
    }

    public static void log(String text) {
    }

    public static void log(Throwable t) {
    }

    // ---- 编译期占位：运行时由真正的 XposedBridge 提供 ----
    public static java.util.Set<XC_MethodHook.Unhook> hookAllMethods(
            Class<?> hookClass, String methodName, XC_MethodHook callback) {
        return null;
    }

    public static XC_MethodHook.Unhook hookMethod(
            java.lang.reflect.Member hookMethod, XC_MethodHook callback) {
        return null;
    }

    public static Object invokeOriginalMethod(java.lang.reflect.Member method, Object thisObject,
                                              Object[] args) throws Throwable {
        return null;
    }
}

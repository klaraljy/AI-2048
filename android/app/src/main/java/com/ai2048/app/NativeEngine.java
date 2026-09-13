package com.ai2048.app;

import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

/**
 * C++ 引擎在 WebView 里的入口。
 *
 * <h2>为什么把原生调用藏在一个注入对象后面</h2>
 *
 * 页面本身是**零依赖、无构建**的前端，它不该知道 JNI 的存在。做法是把一个
 * 普通 Java 对象注入成 {@code window.AI2048Native}，页面按"有就用、没有就降级"
 * 的方式探测 —— 于是同一份前端在浏览器里走 WebSocket、在 APK 里走 JNI，
 * 页面代码一行都不用改。见 web/js/transport.js 的 createTransport。
 *
 * <h2>线程</h2>
 *
 * {@code addJavascriptInterface} 暴露的方法在 WebView 的 **JavaBridge 线程**
 * 上执行，不阻塞 WebView 主线程 —— 这点很重要：搜索一次可能要几百毫秒，
 * 跑在主线程上会直接表现为界面卡住。
 *
 * <h2>会话</h2>
 *
 * 引擎的置换表要在一局之内**跨步复用**（见 engine/src/ai/search.h），
 * 所以这里持有一个原生句柄，NewGame 时才重置。
 */
public final class NativeEngine {

    private static final String TAG = "AI2048Native";

    /** 加载失败时为 false，此后所有调用都返回 null，页面自动降级到 JS AI。 */
    private static final boolean AVAILABLE;

    static {
        boolean ok;
        try {
            System.loadLibrary("ai2048_jni");
            ok = true;
        } catch (UnsatisfiedLinkError e) {
            // 不抛：库没编出来时页面仍要能用（降级到内置 JS AI）。
            Log.w(TAG, "native library unavailable, falling back to JS AI", e);
            ok = false;
        }
        AVAILABLE = ok;
    }

    /** 本次会话的原生句柄；0 表示尚未初始化。
     *
     *  static：注入给 WebView 的 Bridge 是独立对象（static 嵌套类），
     *  拿不到外部实例的字段，所以句柄放在类上。
     *  进程内只有一个 WebView / 一个引擎会话，这样最简单，
     *  也避免"重建 Activity 后旧句柄还在用"这类问题（onDestroy 会 dispose）。 */
    private static long handle = 0;

    // ---- native 方法（实现见 src/main/cpp/jni_bridge.cpp）----
    private static native long nativeInit(int baseDepth, int timeBudgetMs, String difficulty);
    private static native void nativeDispose(long handle);
    private static native String nativeBestMove(long handle, int[] board, String lastMove);
    private static native void nativeNewGame(long handle);
    private static native String nativeVersion();
    private static native int nativeRulesetVersion();
    private static native String nativeSelfCheckMove(int[] board, int baseDepth, String difficulty);

    /**
     * 自检：在一组写死的局面上跑真实决策，返回 "up,left,down" 这样的结果串。
     * 与桌面引擎（ai2048-cli 的同一组局面）比对，用来证明 JNI 这一层没有把
     * 参数传错。详见 jni_bridge.cpp 的说明。
     */
    private static native String nativeSelfTest(int baseDepth, String difficulty);

    public static boolean isAvailable() {
        return AVAILABLE;
    }

    /**
     * 供页面探测用的对象。方法名与 web/js/transport.js 里约定的完全一致。
     *
     * 做成 static 嵌套类：注入给 WebView 的是一个独立对象，不需要外部实例。
     */
    public static final class Bridge {

        /**
         * 配置（强度 / 时间预算 / 难度）。
         *
         * @param configJson {"baseDepth":8,"timeBudgetMs":2500,"difficulty":"hard"}
         * @return 成功与否；失败时页面会降级
         */
        @android.webkit.JavascriptInterface
        public boolean configure(String configJson) {
            if (!AVAILABLE) return false;
            try {
                JSONObject config = new JSONObject(configJson);
                final int baseDepth = config.optInt("baseDepth", 8);
                final int timeBudgetMs = config.optInt("timeBudgetMs", 0);
                final String difficulty = config.optString("difficulty", "normal");

                dispose();
                handle = nativeInit(baseDepth, timeBudgetMs, difficulty);
                return handle != 0;
            } catch (Throwable t) {
                Log.w(TAG, "configure failed", t);
                return false;
            }
        }

        /**
         * 算一步。
         *
         * @param boardJson 长度 16 的指数数组（0 = 空格，1 = 2，2 = 4 …），
         *                  顺序 row * 4 + col，与 web/js/game.js 的表示一致
         * @param lastMove  上一步方向名，可为 null
         * @return {"move":"left","engine":"0.0.1","ruleset":3}；无合法走子时 move 为 null
         */
        @android.webkit.JavascriptInterface
        public String bestMove(String boardJson, String lastMove) {
            if (!AVAILABLE || handle == 0) return null;
            try {
                JSONArray array = new JSONArray(boardJson);
                if (array.length() != 16) return null;

                int[] board = new int[16];
                for (int i = 0; i < 16; i++) board[i] = array.optInt(i, 0);

                final String move = nativeBestMove(handle, board, lastMove);

                JSONObject out = new JSONObject();
                // move 为 null 是**有意义**的：表示 AI 无步可走，调用方据此停止演示。
                out.put("move", move == null ? JSONObject.NULL : move);
                out.put("engine", nativeVersion());
                out.put("ruleset", nativeRulesetVersion());
                return out.toString();
            } catch (Throwable t) {
                Log.w(TAG, "bestMove failed", t);
                return null;
            }
        }

        /** 新开一局：清置换表，但保留配置。 */
        @android.webkit.JavascriptInterface
        public void newGame() {
            if (AVAILABLE && handle != 0) nativeNewGame(handle);
        }

        /** 引擎信息，供页面显示"引擎已接上"。 */
        @android.webkit.JavascriptInterface
        public String engineInfo() {
            if (!AVAILABLE) return null;
            try {
                JSONObject out = new JSONObject();
                out.put("engine", nativeVersion());
                out.put("ruleset", nativeRulesetVersion());
                out.put("source", "jni");
                return out.toString();
            } catch (Throwable t) {
                return null;
            }
        }

        /**
         * 自检入口：不算置换表、不限时，同一局面必定同一结果。
         * 给测试用 —— JNI 这一层最容易出的错是参数传错（深度单位、棋盘编码顺序），
         * 那类错不会崩，只会让 AI 悄悄变弱。
         */
        @android.webkit.JavascriptInterface
        public String selfCheckMove(String boardJson, int baseDepth, String difficulty) {
            if (!AVAILABLE) return null;
            try {
                JSONArray array = new JSONArray(boardJson);
                if (array.length() != 16) return null;
                int[] board = new int[16];
                for (int i = 0; i < 16; i++) board[i] = array.optInt(i, 0);
                return nativeSelfCheckMove(board, baseDepth, difficulty);
            } catch (Throwable t) {
                Log.w(TAG, "selfCheckMove failed", t);
                return null;
            }
        }

        /**
         * 整层自检：跑一组写死的局面，返回 "up,left,down"。
         * 与桌面引擎的同一组结果比对，用来抓"参数传错但程序不崩"这类问题。
         */
        @android.webkit.JavascriptInterface
        public String selfTest(int baseDepth, String difficulty) {
            if (!AVAILABLE) return null;
            try {
                return nativeSelfTest(baseDepth, difficulty);
            } catch (Throwable t) {
                Log.w(TAG, "selfTest failed", t);
                return null;
            }
        }
    }

    /**
     * 释放原生会话。Activity 销毁时调用。
     *
     * static：句柄本身是静态的（见上面 handle 的说明），
     * 而桥对象是独立的静态嵌套类，两边必须一致 —— 否则
     * "无法从静态上下文中引用非静态方法"。
     */
    public static void dispose() {
        if (handle != 0) {
            nativeDispose(handle);
            handle = 0;
        }
    }}

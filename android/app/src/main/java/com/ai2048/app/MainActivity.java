package com.ai2048.app;

import android.os.Bundle;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import androidx.activity.ComponentActivity;
import androidx.webkit.WebViewAssetLoader;

/**
 * 整个 App 就是一个 WebView，加载打包在 APK 里的前端。
 *
 * <h2>为什么不用 file:// 直接开 assets/index.html</h2>
 *
 * 前端是 **ES module**（{@code <script type="module">}）加 Web Audio。
 * 在 {@code file://} 源下，浏览器把每个文件都当成独立的不透明源：
 * <ul>
 *   <li>module 的 import 会被 CORS 挡掉，页面直接白屏</li>
 *   <li>Web Audio 的 decodeAudioData 拿不到本地文件（音效全废）</li>
 *   <li>localStorage 在部分实现下不可用（最高分存不住）</li>
 * </ul>
 *
 * {@link WebViewAssetLoader} 把 assets 映射到
 * {@code https://appassets.androidplatform.net/} 这个**正常源**上，
 * 上面三件事就都正常了。代价只是多一个依赖（androidx.webkit）。
 *
 * <h2>离线可玩</h2>
 *
 * C++ 引擎（走子 AI）现在**已经编进 APK**：见 {@link NativeEngine} 与
 * {@code src/main/cpp/jni_bridge.cpp}。做法是把它编成 .so 并注入
 * {@code window.AI2048Native}，前端探测到它就走 JNI，否则降级到内置的 JS AI。
 * 所以即使 .so 因为某个 ABI 缺失而没装上，页面仍然可用。
 */
public class MainActivity extends ComponentActivity {

    private static final String ASSET_HOST = "appassets.androidplatform.net";
    private static final String START_URL = "https://" + ASSET_HOST + "/assets/web/index.html";

    private WebView webView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        final WebViewAssetLoader assetLoader = new WebViewAssetLoader.Builder()
                .addPathHandler("/assets/", new WebViewAssetLoader.AssetsPathHandler(this))
                .build();

        webView = new WebView(this);
        setContentView(webView);

        // 把 C++ 引擎注入成 window.AI2048Native。
        // 名字里的 NativeEngine 只是调试用的标签，页面不读它。
        // 库加载失败时 addJavascriptInterface 照常执行，但对象里的方法全部返回
        // null —— 页面据此降级到内置 JS AI（见 transport.js）。
        webView.addJavascriptInterface(new NativeEngine.Bridge(), "AI2048Native");

        WebSettings settings = webView.getSettings();
        settings.setJavaScriptEnabled(true);
        // 最高分与设置存在 localStorage 里，不开这个就存不住。
        settings.setDomStorageEnabled(true);
        // 音效是用户点「开始」之后才播的，不需要额外手势门槛；
        // 但 WebView 默认要求手势，这行免掉那个要求，否则第一次点按没声音。
        settings.setMediaPlaybackRequiresUserGesture(false);
        // 让页面按设备宽度排版，不要用桌面视口。
        settings.setUseWideViewPort(true);
        settings.setLoadWithOverviewMode(true);

        webView.setWebViewClient(new WebViewClient() {
            @Override
            public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request) {
                // 所有请求都走 assetLoader：把 https://appassets.../assets/...
                // 映射到 APK 里的 assets/...，不碰网络。
                return assetLoader.shouldInterceptRequest(request.getUrl());
            }
        });

        if (savedInstanceState == null) {
            webView.loadUrl(START_URL);
        } else {
            webView.restoreState(savedInstanceState);
        }
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        webView.saveState(outState);
    }

    @Override
    protected void onDestroy() {
        // 释放原生会话（置换表等）。不释放会随每次重建 Activity 泄漏一份。
        NativeEngine.dispose();
        super.onDestroy();
    }

    /**
     * 返回键先给页面处理（比如关掉弹层），页面不处理才退出 App。
     * 2048 本身没有弹层，但撤销/重开这类交互以后可能会加。
     */
    @Override
    public void onBackPressed() {
        if (webView != null && webView.canGoBack()) {
            webView.goBack();
            return;
        }
        super.onBackPressed();
    }
}

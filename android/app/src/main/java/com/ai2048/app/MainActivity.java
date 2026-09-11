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
 * 引擎（走子 AI）没有编进 APK —— 见 android/README.md 的说明。
 * 前端在连不上引擎时会自己降级到内置的本地 AI，并显示提示条，
 * 所以这个 APK 是**完全离线可玩**的，只是 AI 弱一档。
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

# 混淆规则。
#
# minifyEnabled 目前是 false（见 app/build.gradle 的说明），所以这个文件
# 现在不生效。留着是为了将来真要开混淆时不用重新想一遍：
# WebView 里的 JS 通过 @JavascriptInterface 调用的方法必须保留，
# 否则混淆后方法名变了，JS 那边就调不到了。
#
# 目前没有任何 @JavascriptInterface 方法（前端全部逻辑在 assets 里自己跑），
# 所以下面这条是预防性的，等真的加了桥接再打开。
#
# -keepclassmembers class com.ai2048.app.** {
#     @android.webkit.JavascriptInterface <methods>;
# }

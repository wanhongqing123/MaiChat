// 文件作用：让普通 Android 页面共用真实主题和字号，不改变各页面业务生命周期。
package com.kongshang.maichat;

import android.content.Context;
import android.content.res.Configuration;
import android.os.Bundle;
import androidx.activity.ComponentActivity;

public abstract class MaiChatBaseActivity extends ComponentActivity {
    @Override protected void attachBaseContext(Context base) {
        super.attachBaseContext(MaiChatPreferences.wrap(base));
    }
    @Override protected void onCreate(Bundle state) {
        MaiChatTheme.configure(this);
        MaiChatTypography.configure(this);
        setTheme(MaiChatTheme.isDark() ? R.style.AppThemeDark : R.style.AppTheme);
        super.onCreate(state);
    }
    @Override public void onConfigurationChanged(Configuration configuration) {
        Configuration applied = new Configuration(configuration);
        applied.fontScale = MaiChatTypographyTokens.scale(MaiChatPreferences.font(this), configuration.fontScale);
        getResources().updateConfiguration(applied, getResources().getDisplayMetrics());
        super.onConfigurationChanged(applied);
        MaiChatTheme.configure(this);
        MaiChatTypography.configure(this);
        setTheme(MaiChatTheme.isDark() ? R.style.AppThemeDark : R.style.AppTheme);
        MaiChatTypography.refresh(getWindow().getDecorView());
        MarkdownRenderer.initialize(this);
    }
}

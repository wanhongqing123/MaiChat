// Debug-only native text host. Does not initialize an Agent, account or network.
package com.kongshang.maichat;

import android.app.Activity;
import android.os.Bundle;

public final class MarkdownPresentationTestActivity extends Activity {
    MarkdownContentView content;
    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        MarkdownRenderer.initialize(this);
        content = new MarkdownContentView(this);
        setContentView(content);
        MarkdownRenderer.bind(content, getIntent().getStringExtra("markdown_fixture"));
    }
}

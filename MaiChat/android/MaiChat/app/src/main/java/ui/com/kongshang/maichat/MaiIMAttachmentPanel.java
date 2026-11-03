// 文件作用：IM 编辑器下方的两行附件动作；仅呈现和分派，不读取媒体或启动录音。
package com.kongshang.maichat;

import android.content.Context;
import android.graphics.Color;
import android.graphics.PorterDuff;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.util.EnumMap;
import java.util.Objects;
import java.util.function.Consumer;

/** Mirrors the iOS IM attachment grid; the host owns dismissal, permissions and account checks. */
public final class MaiIMAttachmentPanel extends LinearLayout {
    public enum Action { ALBUM, CAMERA, FILE, VOICE_INPUT, AI_REPLY }

    private final EnumMap<Action, View> tiles = new EnumMap<>(Action.class);
    private final Consumer<Action> listener;

    public static View create(Context context, Consumer<Action> listener) {
        return new MaiIMAttachmentPanel(context, listener);
    }

    private MaiIMAttachmentPanel(Context context, Consumer<Action> listener) {
        super(context);
        this.listener = Objects.requireNonNull(listener);
        setTag("im-attachment-panel");
        setOrientation(VERTICAL);
        setBackgroundColor(MaiChatTheme.PAGE);
        // Cell margins complete the iOS 18dp outer inset and 12dp column spacing.
        setPadding(dp(12), dp(18), dp(12), dp(20));
        addView(row(Action.ALBUM, Action.CAMERA, Action.FILE, Action.VOICE_INPUT),
            new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT));
        LayoutParams second = new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT);
        second.topMargin = dp(14);
        addView(row(Action.AI_REPLY), second);
    }

    private LinearLayout row(Action... actions) {
        LinearLayout row = new LinearLayout(getContext());
        row.setOrientation(HORIZONTAL);
        row.setGravity(Gravity.TOP);
        row.setBaselineAligned(false);
        for (int column = 0; column < 4; column++) {
            View cell;
            if (column < actions.length) {
                cell = tile(actions[column]);
            } else {
                cell = new View(getContext());
                cell.setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
            }
            // A plain spacer View uses the full AT_MOST height by default;
            // keep empty cells at zero height so the panel cannot consume the
            // conversation viewport when measured by its weighted parent.
            LayoutParams params = new LayoutParams(0,
                column < actions.length ? LayoutParams.WRAP_CONTENT : 0, 1);
            params.setMargins(dp(6), 0, dp(6), 0);
            row.addView(cell, params);
        }
        return row;
    }

    private View tile(Action action) {
        LinearLayout tile = new LinearLayout(getContext());
        tile.setTag(action);
        tile.setOrientation(VERTICAL);
        tile.setGravity(Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        tile.setMinimumWidth(dp(44));
        tile.setMinimumHeight(dp(60));
        tile.setContentDescription(title(action));
        tile.setFocusable(true);
        tile.setOnClickListener(view -> {
            if (view.isEnabled()) listener.accept(action);
        });
        int color = color(action);
        FrameLayout iconBox = new FrameLayout(getContext());
        iconBox.setBackground(MaiChatTheme.rounded(Color.argb(23, Color.red(color),
            Color.green(color), Color.blue(color)), 13, getContext()));
        iconBox.setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
        ImageView icon = new ImageView(getContext());
        icon.setImageResource(icon(action));
        icon.setColorFilter(color, PorterDuff.Mode.SRC_IN);
        icon.setPadding(dp(2), dp(2), dp(2), dp(2));
        icon.setScaleType(ImageView.ScaleType.FIT_CENTER);
        icon.setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
        iconBox.addView(icon, new FrameLayout.LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
        tile.addView(iconBox, new LayoutParams(dp(36), dp(36)));
        TextView label = MaiChatTheme.text(getContext(), title(action), 12, MaiChatTheme.SECONDARY);
        label.setGravity(Gravity.CENTER);
        label.setSingleLine(true);
        label.setEllipsize(TextUtils.TruncateAt.END);
        label.setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
        LayoutParams caption = new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT);
        caption.topMargin = dp(8);
        tile.addView(label, caption);
        tiles.put(action, tile);
        return tile;
    }

    @Override public void setEnabled(boolean enabled) {
        super.setEnabled(enabled);
        if (tiles == null) return;
        for (View tile : tiles.values()) {
            tile.setEnabled(enabled);
            tile.setAlpha(enabled ? 1f : 0.5f);
        }
    }

    private static String title(Action action) {
        switch (action) {
            case ALBUM: return "相册";
            case CAMERA: return "拍摄";
            case FILE: return "文件";
            case VOICE_INPUT: return "语音输入";
            case AI_REPLY: return "AI 帮我回复";
            default: throw new IllegalArgumentException("Unknown IM attachment action");
        }
    }

    private static int icon(Action action) {
        switch (action) {
            case ALBUM: return R.drawable.ic_ui_photo;
            case CAMERA: return R.drawable.ic_feature_camera;
            case FILE: return R.drawable.ic_ai_file;
            case VOICE_INPUT: return R.drawable.ic_im_attachment_voice;
            case AI_REPLY: return R.drawable.ic_ai_sparkles;
            default: throw new IllegalArgumentException("Unknown IM attachment action");
        }
    }

    private static int color(Action action) {
        switch (action) {
            case ALBUM: return Color.rgb(15, 143, 224);
            case CAMERA: return Color.rgb(20, 166, 125);
            case FILE: return Color.rgb(232, 143, 59);
            case VOICE_INPUT: return Color.rgb(10, 161, 184);
            case AI_REPLY: return Color.rgb(130, 92, 224);
            default: throw new IllegalArgumentException("Unknown IM attachment action");
        }
    }

    private int dp(float value) { return MaiChatTheme.dp(getContext(), value); }
}

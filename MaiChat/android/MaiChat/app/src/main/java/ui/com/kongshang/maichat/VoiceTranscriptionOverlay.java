// 文件作用：IM 与创作中心共用的语音转文字浮层，与 iOS 保持布局、命中区域和完成文案一致。
// 生命周期：只附加当前窗口 decor，不创建 Dialog、不获取焦点；宿主负责录音与最终结果提交。
package com.kongshang.maichat;

import android.app.Activity;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.drawable.GradientDrawable;
import android.text.Layout;
import android.text.StaticLayout;
import android.text.TextDirectionHeuristics;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.util.function.Consumer;

public final class VoiceTranscriptionOverlay {
    public enum Action { SEND, CANCEL, EDIT }

    private static final int BLUE = Color.rgb(15, 141, 221);
    private final Activity activity;
    private final float density, startRawY;
    private final VoiceTranscriptionGesture gesture;
    private Consumer<Action> onExplicitAction;
    private FrameLayout layer;
    private LinearLayout card;
    private TextView status, transcript;
    private View cancelButton, editButton;
    private Action action = Action.SEND;
    private boolean finishing, dismissed;
    private String text = "";
    private String measuredSource;
    private int measuredWidth = -1;
    private final View.OnLayoutChangeListener layoutListener =
        (view, left, top, right, bottom, oldLeft, oldTop, oldRight, oldBottom) -> layout();
    private final View.OnLayoutChangeListener transcriptLayoutListener =
        (view, left, top, right, bottom, oldLeft, oldTop, oldRight, oldBottom) -> {
            // 只在文字可用宽度变化时重排；文本高度变化不能触发反复裁剪和布局循环。
            if (right - left != oldRight - oldLeft) updateTranscript();
        };
    private final View.OnAttachStateChangeListener attachListener = new View.OnAttachStateChangeListener() {
        @Override public void onViewAttachedToWindow(View view) { }
        @Override public void onViewDetachedFromWindow(View view) { dismiss(); }
    };

    public VoiceTranscriptionOverlay(Activity activity, boolean assistant, float startRawX,
                                     float startRawY, Consumer<Action> onExplicitAction) {
        this.activity = activity;
        this.density = activity.getResources().getDisplayMetrics().density;
        this.startRawY = startRawY;
        this.onExplicitAction = onExplicitAction;
        this.gesture = new VoiceTranscriptionGesture(assistant, density, startRawX, startRawY);
    }

    public void show() {
        if (dismissed || layer != null || activity.isFinishing() || activity.isDestroyed()) return;
        View decor = activity.getWindow().getDecorView();
        if (!(decor instanceof ViewGroup)) return;
        layer = new FrameLayout(activity);
        layer.setBackgroundColor(0x40000000);
        layer.setFocusable(false);
        layer.setClickable(false);
        layer.setClipChildren(false);
        // 空白区域不消费触摸；已开始的 editor 主手指继续由原手势识别器跟踪。
        card = new LinearLayout(activity);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(16), dp(16), dp(16), dp(16));
        GradientDrawable background = new GradientDrawable();
        background.setColor(BLUE); background.setCornerRadius(dp(18));
        card.setBackground(background);
        LinearLayout heading = new LinearLayout(activity);
        heading.setGravity(Gravity.CENTER_VERTICAL);
        heading.addView(new Glyph(activity, null), new LinearLayout.LayoutParams(dp(14), dp(14)));
        status = label(14, 0xD6FFFFFF);
        LinearLayout.LayoutParams statusParams = new LinearLayout.LayoutParams(-2, -2);
        statusParams.leftMargin = dp(7);
        heading.addView(status, statusParams);
        card.addView(heading, new LinearLayout.LayoutParams(-1, -2));
        transcript = label(24, Color.WHITE);
        transcript.setMaxLines(8);
        transcript.setBreakStrategy(Layout.BREAK_STRATEGY_SIMPLE);
        transcript.setHyphenationFrequency(Layout.HYPHENATION_FREQUENCY_NONE);
        transcript.setTextDirection(View.TEXT_DIRECTION_FIRST_STRONG_LTR);
        transcript.addOnLayoutChangeListener(transcriptLayoutListener);
        LinearLayout.LayoutParams textParams = new LinearLayout.LayoutParams(-1, -2);
        textParams.topMargin = dp(8);
        card.addView(transcript, textParams);
        layer.addView(card, new FrameLayout.LayoutParams(-2, -2, Gravity.CENTER));
        cancelButton = button(Action.CANCEL, "取消");
        editButton = button(Action.EDIT, "编辑");
        layer.addView(cancelButton, new FrameLayout.LayoutParams(dp(52), dp(52)));
        layer.addView(editButton, new FrameLayout.LayoutParams(dp(52), dp(52)));
        layer.addOnLayoutChangeListener(layoutListener);
        layer.addOnAttachStateChangeListener(attachListener);
        layer.setOnApplyWindowInsetsListener((view, insets) -> { view.post(this::layout); return insets; });
        ((ViewGroup) decor).addView(layer, new ViewGroup.LayoutParams(-1, -1));
        layer.requestApplyInsets();
        updatePresentation();
    }

    public void dismiss() {
        if (dismissed) return;
        dismissed = true;
        onExplicitAction = null;
        if (transcript != null) transcript.removeOnLayoutChangeListener(transcriptLayoutListener);
        if (layer != null) {
            layer.removeOnLayoutChangeListener(layoutListener);
            layer.removeOnAttachStateChangeListener(attachListener);
            layer.setOnApplyWindowInsetsListener(null);
            if (layer.getParent() instanceof ViewGroup) ((ViewGroup) layer.getParent()).removeView(layer);
            layer.removeAllViews();
        }
        layer = null; card = null; status = null; transcript = null;
        cancelButton = null; editButton = null;
        text = ""; measuredSource = null;
    }

    public void updateText(String value) {
        if (dismissed) return;
        text = VoiceTranscriptionGesture.preview(value);
        updatePresentation();
    }

    public Action move(float rawX, float rawY) {
        if (dismissed || finishing) return action;
        VoiceTranscriptionGesture.Target target = gesture.move(rawX, rawY, bounds(cancelButton), bounds(editButton));
        action = Action.valueOf(target.name());
        updatePresentation();
        return action;
    }

    public void finishing(Action selected) {
        if (dismissed) return;
        action = selected; finishing = true;
        updatePresentation();
    }

    private View button(Action target, String description) {
        Glyph view = new Glyph(activity, target);
        view.setContentDescription(description);
        view.setOnClickListener(clicked -> {
            if (dismissed || finishing) return;
            action = target; updatePresentation();
            Consumer<Action> callback = onExplicitAction;
            if (callback != null) callback.accept(target);
        });
        return view;
    }

    private void layout() {
        if (layer == null || layer.getWidth() == 0 || layer.getHeight() == 0) return;
        FrameLayout.LayoutParams cardParams = (FrameLayout.LayoutParams) card.getLayoutParams();
        int width = Math.max(1, Math.min(layer.getWidth() - dp(32), dp(520)));
        if (cardParams.width != width) { cardParams.width = width; card.setLayoutParams(cardParams); }
        int[] origin = new int[2]; layer.getLocationOnScreen(origin);
        Rect safe = new Rect(); layer.getWindowVisibleDisplayFrame(safe);
        float top = Math.max(0, safe.top - origin[1]);
        float bottom = Math.min(layer.getHeight(), safe.bottom - origin[1]);
        if (bottom - top < dp(52)) { top = 0; bottom = layer.getHeight(); }
        float centerY = Math.max(top + dp(26), Math.min(startRawY - origin[1] - dp(160), bottom - dp(26)));
        float centerX = layer.getWidth() / 2f;
        float left = Math.max(0, safe.left - origin[0]);
        float right = Math.min(layer.getWidth(), safe.right - origin[0]);
        if (right - left < dp(52)) { left = 0; right = layer.getWidth(); }
        // X 固定在窗口中心两侧，只有 Y 跟随按下位置，避免左右起点改变可见按钮位置。
        cancelButton.setX(Math.max(left, Math.min(centerX - dp(72) - dp(26), right - dp(52))));
        editButton.setX(Math.max(left, Math.min(centerX + dp(72) - dp(26), right - dp(52))));
        cancelButton.setY(centerY - dp(26)); editButton.setY(centerY - dp(26));
    }

    private VoiceTranscriptionGesture.Bounds bounds(View button) {
        if (button == null || button.getWidth() == 0 || !button.isAttachedToWindow()) return null;
        int[] location = new int[2]; layer.getLocationOnScreen(location);
        // 选中缩放只改变外观，扩大命中框使用未缩放布局，避免边缘高亮来回切换。
        float left = location[0] + button.getX(), top = location[1] + button.getY();
        return new VoiceTranscriptionGesture.Bounds(left, top, left + button.getWidth(), top + button.getHeight());
    }

    private void updatePresentation() {
        if (layer == null) return;
        updateTranscript();
        status.setText(finishing ? (action == Action.EDIT ? "正在准备编辑…" : "正在完成识别…")
            : action == Action.CANCEL ? "松手取消" : action == Action.EDIT ? "松手编辑"
            : text.isEmpty() ? "正在听，松手发送" : "松手发送");
        styleButton(cancelButton, action == Action.CANCEL, Color.RED);
        styleButton(editButton, action == Action.EDIT, BLUE);
    }

    private void updateTranscript() {
        if (transcript == null) return;
        String source = text.isEmpty() ? "正在听，请说话…" : text;
        int width = transcript.getWidth() - transcript.getCompoundPaddingLeft() - transcript.getCompoundPaddingRight();
        if (width == measuredWidth && source.equals(measuredSource)) return;
        String visible = width > 0 ? tailLines(source, width) : source;
        measuredWidth = width; measuredSource = source;
        if (!TextUtils.equals(transcript.getText(), visible)) transcript.setText(visible);
    }

    private String tailLines(String source, int width) {
        StaticLayout measured = measureText(source, width);
        if (measured.getLineCount() <= 8) return source;
        int start = measured.getLineStart(measured.getLineCount() - 8);
        if (start > 0 && start < source.length() && Character.isLowSurrogate(source.charAt(start))) start++;
        String visible = "…" + source.substring(start);
        // 多行 TextView 不支持 START 省略：按实际字体/宽度先取末八行，再为省略号腾出空间。
        // 原始末 180 codepoint 始终保存在 text 中，变宽时可以恢复；最多前进 180 次即结束。
        while (start < source.length() && measureText(visible, width).getLineCount() > 8) {
            start = source.offsetByCodePoints(start, 1);
            visible = "…" + source.substring(start);
        }
        return visible;
    }

    private StaticLayout measureText(String source, int width) {
        return StaticLayout.Builder.obtain(source, 0, source.length(), transcript.getPaint(), width)
            .setIncludePad(transcript.getIncludeFontPadding())
            .setLineSpacing(transcript.getLineSpacingExtra(), transcript.getLineSpacingMultiplier())
            .setBreakStrategy(transcript.getBreakStrategy())
            .setHyphenationFrequency(transcript.getHyphenationFrequency())
            .setTextDirection(TextDirectionHeuristics.FIRSTSTRONG_LTR)
            .build();
    }

    private void styleButton(View button, boolean selected, int selectedColor) {
        GradientDrawable circle = new GradientDrawable();
        circle.setShape(GradientDrawable.OVAL);
        circle.setColor(selected ? selectedColor : 0xA6000000);
        circle.setStroke(dp(1.5f), selected ? 0xE6FFFFFF : 0x4DFFFFFF);
        button.setBackground(circle);
        button.setScaleX(selected ? 1.12f : 1); button.setScaleY(selected ? 1.12f : 1);
        button.setEnabled(!finishing);
    }

    private TextView label(int size, int color) {
        TextView view = new TextView(activity);
        view.setTextSize(size); view.setTextColor(color);
        view.setTypeface(android.graphics.Typeface.create("sans-serif-medium", android.graphics.Typeface.NORMAL));
        view.setIncludeFontPadding(false);
        return view;
    }

    private int dp(float value) { return Math.round(value * density); }

    /** 本地矢量路径避免跨字体字符差异；空 action 为主卡片的麦克风。 */
    private static final class Glyph extends View {
        private final Action action;
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        Glyph(Activity activity, Action action) {
            super(activity); this.action = action;
            paint.setColor(Color.WHITE); paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(1.8f); paint.setStrokeCap(Paint.Cap.ROUND); paint.setStrokeJoin(Paint.Join.ROUND);
        }
        @Override protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            float size = action == null ? Math.min(getWidth(), getHeight()) : 20 * getResources().getDisplayMetrics().density;
            canvas.save(); canvas.translate((getWidth() - size) / 2, (getHeight() - size) / 2);
            canvas.scale(size / 20, size / 20);
            if (action == Action.CANCEL) {
                canvas.drawLine(4, 4, 16, 16, paint); canvas.drawLine(16, 4, 4, 16, paint);
            } else if (action == Action.EDIT) {
                Path pencil = new Path(); pencil.moveTo(4, 13); pencil.lineTo(13, 4);
                pencil.lineTo(17, 8); pencil.lineTo(8, 17); pencil.lineTo(3, 18); pencil.close();
                canvas.drawPath(pencil, paint); canvas.drawLine(11, 6, 15, 10, paint);
            } else {
                canvas.drawRoundRect(7, 2, 13, 12, 3, 3, paint);
                canvas.drawArc(4, 5, 16, 16, 0, 180, false, paint);
                canvas.drawLine(10, 16, 10, 19, paint); canvas.drawLine(7, 19, 13, 19, paint);
            }
            canvas.restore();
        }
    }
}

// 文件作用：Android 跨页面复用的界面组件；MaiChatTypography 负责界面字体、字号和文本层级，本文件给出其实现。
// 主要入口：semibold、body。
package com.kongshang.maichat;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.os.Build;
import android.text.Layout;
import android.text.Spanned;
import android.text.TextPaint;
import android.text.style.LeadingMarginSpan;
import android.text.style.MetricAffectingSpan;
import android.widget.TextView;

/** Adapts the shared type scale and font catalog to native views and Markdown. */
public final class MaiChatTypography {
    static final int BODY_SP = (int) MaiChatTypographyTokens.Role.BODY.points;
    static final int DATE_SP = (int) MaiChatTypographyTokens.Role.METADATA.points;
    static final int INLINE_CODE_SP = (int) MaiChatTypographyTokens.Role.DETAIL.points;
    static final int CODE_BLOCK_SP = (int) MaiChatTypographyTokens.Role.DETAIL.points;

    // 缓存只有字体资产；字号在使用时求值。切换字体由既有外观重建保留草稿。
    private static volatile String familyId = "system";
    private static volatile Typeface[] faces = systemFaces();
    private static final java.util.Map<String, Typeface[]> loaded = new java.util.HashMap<>();
    private static Typeface[] systemFaces() {
        return new Typeface[]{Typeface.SANS_SERIF, Typeface.create("sans-serif-medium", 0),
            Build.VERSION.SDK_INT >= 28 ? Typeface.create(Typeface.SANS_SERIF, 600, false)
                : Typeface.create("sans-serif-medium", 0), Typeface.create(Typeface.SANS_SERIF, Typeface.BOLD)};
    }
    public static synchronized void configure(Context context) {
        MaiChatFontFamilyTokens.Family requested = MaiChatFontFamilyTokens.Family.fromId(MaiChatPreferences.fontFamily(context));
        if (requested.windowsOnly) requested = MaiChatFontFamilyTokens.Family.SYSTEM;
        if (requested.id.equals(familyId)) return;
        Typeface[] selected = loaded.get(requested.id);
        if (selected == null) {
            try {
                selected = requested == MaiChatFontFamilyTokens.Family.SYSTEM ? systemFaces() : loadFaces(context, requested);
                loaded.put(requested.id, selected);
            } catch (java.io.IOException | RuntimeException error) {
                android.util.Log.w("MaiChat.font", "Bundled typeface is unavailable", error);
                requested = MaiChatFontFamilyTokens.Family.SYSTEM; selected = systemFaces();
            }
        }
        faces = selected; familyId = requested.id;
    }
    private static Typeface[] loadFaces(Context context, MaiChatFontFamilyTokens.Family family) throws java.io.IOException {
        String[] names = {family.regular, family.medium, family.semibold, family.bold};
        Typeface[] result = new Typeface[4];
        if (Build.VERSION.SDK_INT >= 29) {
            android.graphics.fonts.FontFamily.Builder builder = new android.graphics.fonts.FontFamily.Builder(
                new android.graphics.fonts.Font.Builder(context.getAssets(), "fonts/" + family.regular + ".otf").setWeight(400).build());
            builder.addFont(new android.graphics.fonts.Font.Builder(context.getAssets(), "fonts/" + family.medium + ".otf").setWeight(500).build());
            if (!family.semibold.equals(family.medium)) builder.addFont(new android.graphics.fonts.Font.Builder(
                context.getAssets(), "fonts/" + family.semibold + ".otf").setWeight(600).build());
            builder.addFont(new android.graphics.fonts.Font.Builder(context.getAssets(), "fonts/" + family.bold + ".otf").setWeight(700).build());
            Typeface base = new Typeface.CustomFallbackBuilder(builder.build()).build();
            int[] weights = {400, 500, family.semibold.equals(family.medium) ? 500 : 600, 700};
            for (int index = 0; index < 4; ++index) result[index] = Typeface.create(base, weights[index], false);
        } else {
            for (int index = 0; index < 4; ++index)
                result[index] = Typeface.createFromAsset(context.getAssets(), "fonts/" + names[index] + ".otf");
        }
        return result;
    }
    static String familyId() { return familyId; }
    public static Typeface normal() { return faces[0]; }
    public static Typeface semibold() { return faces[2]; }
    private static Typeface forView(TextView view) {
        Typeface old = view.getTypeface();
        if (old == null) return normal();
        if (old.equals(faces[2])) return faces[2];
        int weight = Build.VERSION.SDK_INT >= 28 ? old.getWeight() : old.isBold() ? 700 : 400;
        Typeface selected = faces[weight >= 700 ? 3 : weight >= 600 ? 2 : weight >= 500 ? 1 : 0];
        return old.isItalic() ? Typeface.create(selected, Typeface.ITALIC) : selected;
    }
    /** PX is deliberate: Android nonlinear SP conversion must not scale a role twice. */
    public static float pixels(Context context, MaiChatTypographyTokens.Role role) {
        return role.points * density(context) * context.getResources().getConfiguration().fontScale;
    }
    public static float density(Context context) { return context.getResources().getDisplayMetrics().density; }
    public static void apply(TextView view, MaiChatTypographyTokens.Role role) {
        view.setTag(R.id.typography_role, role);
        view.setTypeface(forView(view));
        view.setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX, pixels(view.getContext(), role));
    }
    public static TextView text(Context context) {
        TextView view = new TextView(context); apply(view, MaiChatTypographyTokens.Role.BODY); return view;
    }
    public static android.widget.EditText input(Context context) {
        android.widget.EditText view = new android.widget.EditText(context);
        apply(view, MaiChatTypographyTokens.Role.BODY); return view;
    }
    public static android.widget.Button button(Context context) {
        android.widget.Button view = new android.widget.Button(context);
        apply(view, MaiChatTypographyTokens.Role.BODY); return view;
    }
    public static android.widget.CheckBox checkBox(Context context) {
        android.widget.CheckBox view = new android.widget.CheckBox(context);
        apply(view, MaiChatTypographyTokens.Role.BODY); return view;
    }
    public static void refresh(android.view.View view) {
        Object role = view.getTag(R.id.typography_role);
        if (view instanceof TextView && role instanceof MaiChatTypographyTokens.Role)
            apply((TextView) view, (MaiChatTypographyTokens.Role) role);
        if (view instanceof android.view.ViewGroup) {
            android.view.ViewGroup group = (android.view.ViewGroup) view;
            for (int index = 0; index < group.getChildCount(); ++index) refresh(group.getChildAt(index));
        }
    }
    static TextView body(Context context) {
        TextView view = MaiChatTheme.text(context, "", MaiChatTypographyTokens.Role.BODY, MaiChatTheme.TEXT);
        view.setTypeface(normal());
        view.setLineSpacing(pixels(context, MaiChatTypographyTokens.Role.BODY) * MaiChatTypographyTokens.LINEGAP_RATIO, 1);
        view.setTextIsSelectable(true);
        return view;
    }
    static final class SemiboldSpan extends MetricAffectingSpan {
        @Override public void updateDrawState(TextPaint paint) { paint.setTypeface(semibold()); }
        @Override public void updateMeasureState(TextPaint paint) { paint.setTypeface(semibold()); }
    }
    static final class BlueBulletSpan implements LeadingMarginSpan {
        private final int margin;
        private final float radius, gap;
        BlueBulletSpan(float density, int level) {
            margin = Math.round((level == 0 ? 24 : level > 4 ? 0 : 12) * density);
            radius = 1.5f * density;
            gap = 9 * density;
        }
        @Override public int getLeadingMargin(boolean first) { return margin; }
        @Override
        public void drawLeadingMargin(Canvas canvas, Paint paint, int x, int direction, int top, int baseline,
                int bottom, CharSequence text, int start, int end, boolean first, Layout layout) {
            if (!first || !(text instanceof Spanned) || ((Spanned) text).getSpanStart(this) != start) return;
            int color = paint.getColor(); Paint.Style style = paint.getStyle();
            paint.setColor(MaiChatTheme.BLUE); paint.setStyle(Paint.Style.FILL);
            canvas.drawCircle(
                    x + direction * (margin - gap), baseline + (paint.ascent() + paint.descent()) / 2, radius, paint);
            paint.setColor(color); paint.setStyle(style);
        }
    }
}

// Inline contents of the existing top-right action popup; the controller owns persistence.
package com.kongshang.maichat;

import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Build;
import android.transition.AutoTransition;
import android.transition.TransitionManager;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.animation.DecelerateInterpolator;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.List;

final class AIAssistantActionMenu extends LinearLayout {
    // This popup follows the neutral iOS action panel, independently of chat's blue-gray palette.
    private static final int PRIMARY = Color.rgb(10, 10, 10);
    private static final int SECONDARY = Color.rgb(132, 132, 133);
    private static final int DESTRUCTIVE = Color.rgb(255, 59, 48);
    private static final int CHECK = Color.rgb(0, 122, 255);
    private static final int MODEL_SECTION = 1, EFFORT_SECTION = 2, POLICY_SECTION = 3;
    interface Listener {
        void selectModelSource(boolean local);
        void selectEffort(String effort);
        void selectPolicy(String policy);
        void clearConversation();
    }

    private final Listener listener;
    private final Runnable sizeChanged;
    private final LinearLayout modelGroup, effortGroup, policyGroup;
    private final View modelHeading, effortHeading, policyHeading, clear;
    private final TextView modelValue, effortValue, policyValue;
    private final ImageView modelChevron, effortChevron, policyChevron;
    private final List<View> modelChoices = new ArrayList<>();
    private final List<ImageView> modelChecks = new ArrayList<>();
    private final List<View> effortChoices = new ArrayList<>(), policyChoices = new ArrayList<>();
    private final List<ImageView> effortChecks = new ArrayList<>(), policyChecks = new ArrayList<>();
    private static final String[] EFFORTS = {"low", "high", "max"};
    private static final String[] POLICIES = {"on-request", "unless-trusted", "never"};
    private static final String[] MODEL_SOURCES = {"cloud", "llama"};
    private String effort = "high", policy = "on-request";
    private boolean localModel, changingModelSource, changeModelAllowed, changeAllowed, clearAllowed;
    private int expandedSection;

    AIAssistantActionMenu(Context context, Listener listener, Runnable sizeChanged) {
        super(context);
        this.listener = listener;
        this.sizeChanged = sizeChanged;
        setOrientation(VERTICAL);
        // The PopupWindow owns the single material background and outline. A second opaque
        // background here would hide its frost and make expanded groups look like flat blocks.
        setContentDescription("创作中心操作菜单");

        modelValue = text("", 11, SECONDARY);
        modelChevron = icon(R.drawable.ic_ai_menu_chevron, PRIMARY);
        modelHeading = heading("主模型", R.drawable.ic_ai_menu_cpu, modelValue, modelChevron,
            ignored -> toggle(MODEL_SECTION));
        addView(modelHeading, match(dp(52)));
        modelGroup = options(new String[]{"云端模型", "本地模型"}, MODEL_SOURCES,
            modelChoices, modelChecks, MODEL_SECTION);
        addView(modelGroup, match(LayoutParams.WRAP_CONTENT));
        addDivider();

        effortValue = text("", 11, SECONDARY);
        effortChevron = icon(R.drawable.ic_ai_menu_chevron, PRIMARY);
        effortHeading = heading("思考强度", R.drawable.ic_ai_menu_sparkles, effortValue, effortChevron,
            ignored -> toggle(EFFORT_SECTION));
        addView(effortHeading, match(dp(52)));
        effortGroup = options(new String[]{"简单", "标准", "深度"}, EFFORTS,
            effortChoices, effortChecks, EFFORT_SECTION);
        addView(effortGroup, match(LayoutParams.WRAP_CONTENT));
        addDivider();

        policyValue = text("", 11, SECONDARY);
        policyChevron = icon(R.drawable.ic_ai_menu_chevron, PRIMARY);
        policyHeading = heading("操作权限", R.drawable.ic_ai_menu_shield, policyValue, policyChevron,
            ignored -> toggle(POLICY_SECTION));
        addView(policyHeading, match(dp(52)));
        policyGroup = options(new String[]{"请求批准", "帮我批准", "完全访问"}, POLICIES,
            policyChoices, policyChecks, POLICY_SECTION);
        TextView explanation = text("完全访问免除一般操作确认；付费任务仍需批准。", 11,
            SECONDARY);
        explanation.setPadding(dp(14), 0, dp(14), dp(6));
        policyGroup.addView(explanation, match(LayoutParams.WRAP_CONTENT));
        addView(policyGroup, match(LayoutParams.WRAP_CONTENT));
        addDivider();

        clear = heading("清空当前对话", R.drawable.ic_ai_menu_trash, null, null, ignored -> {
            if (clearAllowed) listener.clearConversation();
        });
        addView(clear, match(dp(44)));
        render();
    }

    void update(boolean localModel, boolean changingModelSource, String effort, String policy,
                boolean changeAllowed, boolean changeModelAllowed, boolean clearAllowed) {
        this.localModel = localModel;
        this.changingModelSource = changingModelSource;
        this.effort = effort;
        this.policy = policy;
        this.changeAllowed = changeAllowed;
        this.changeModelAllowed = changeModelAllowed;
        this.clearAllowed = clearAllowed;
        render();
    }

    private void toggle(int section) {
        if (!(section == MODEL_SECTION ? changeModelAllowed : changeAllowed)) return;
        // Match the iOS panel's small inline expansion; keep this content and PopupWindow alive.
        if (isLaidOut()) {
            AutoTransition transition = new AutoTransition();
            transition.setDuration(140);
            transition.setInterpolator(new DecelerateInterpolator());
            TransitionManager.beginDelayedTransition(this, transition);
        }
        expandedSection = expandedSection == section ? 0 : section;
        render();
        sizeChanged.run();
    }

    private void render() {
        // Preserve the committed source/check until the controller finishes preparing and saving.
        modelValue.setText(changingModelSource ? "正在切换…" : localModel ? "本地模型" : "云端模型");
        effortValue.setText(effort.equals("low") ? "简单" : effort.equals("max") ? "深度" : "标准");
        policyValue.setText(policy.equals("never") ? "完全访问"
            : policy.equals("unless-trusted") ? "帮我批准" : "请求批准");
        modelHeading.setEnabled(changeModelAllowed);
        modelHeading.setAlpha(changeModelAllowed ? 1f : .5f);
        effortHeading.setEnabled(changeAllowed);
        policyHeading.setEnabled(changeAllowed);
        effortHeading.setAlpha(changeAllowed ? 1f : .5f);
        policyHeading.setAlpha(changeAllowed ? 1f : .5f);
        modelChevron.setRotation(expandedSection == MODEL_SECTION ? 90f : 0f);
        effortChevron.setRotation(expandedSection == EFFORT_SECTION ? 90f : 0f);
        policyChevron.setRotation(expandedSection == POLICY_SECTION ? 90f : 0f);
        modelGroup.setVisibility(expandedSection == MODEL_SECTION ? VISIBLE : GONE);
        effortGroup.setVisibility(expandedSection == EFFORT_SECTION ? VISIBLE : GONE);
        policyGroup.setVisibility(expandedSection == POLICY_SECTION ? VISIBLE : GONE);
        refreshChoices(modelChoices, modelChecks, MODEL_SOURCES, localModel ? "llama" : "cloud",
            changeModelAllowed);
        refreshChoices(effortChoices, effortChecks, EFFORTS, effort, changeAllowed);
        refreshChoices(policyChoices, policyChecks, POLICIES, policy, changeAllowed);
        clear.setEnabled(clearAllowed);
        clear.setAlpha(clearAllowed ? 1f : .5f);
    }

    private void refreshChoices(List<View> choices, List<ImageView> checks, String[] values,
                                String selected, boolean allowed) {
        for (int i = 0; i < values.length; i++) {
            choices.get(i).setEnabled(allowed);
            choices.get(i).setSelected(values[i].equals(selected));
            choices.get(i).setAlpha(allowed ? 1f : .5f);
            checks.get(i).setVisibility(values[i].equals(selected) ? VISIBLE : INVISIBLE);
        }
    }

    private LinearLayout options(String[] titles, String[] values, List<View> choices,
                                 List<ImageView> checks, int section) {
        LinearLayout group = new LinearLayout(getContext());
        group.setOrientation(VERTICAL);
        group.setPadding(0, dp(4), 0, dp(4));
        // The user's iOS reference has a rounded inset surface for the expanded choices.
        group.setBackground(MaiChatTheme.rounded(Color.rgb(242, 242, 247), 14, getContext()));
        group.setClipToOutline(true);
        for (int i = 0; i < values.length; i++) {
            String value = values[i];
            LinearLayout row = new LinearLayout(getContext());
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setPadding(dp(44), 0, dp(44), 0);
            row.setContentDescription(titles[i]);
            row.setFocusable(true);
            TextView title = text(titles[i], 13, PRIMARY);
            title.setTypeface(medium());
            row.addView(title, new LayoutParams(0, -2, 1));
            ImageView check = icon(R.drawable.ic_ai_menu_check, CHECK);
            row.addView(check, new LayoutParams(dp(12), dp(12)));
            row.setOnClickListener(ignored -> {
                if (section == MODEL_SECTION) {
                    boolean nextLocal = value.equals("llama");
                    if (changeModelAllowed && nextLocal != localModel)
                        listener.selectModelSource(nextLocal);
                    return;
                }
                if (!changeAllowed) return;
                if (section == EFFORT_SECTION) {
                    if (!value.equals(effort)) listener.selectEffort(value);
                } else if (!value.equals(policy)) listener.selectPolicy(value);
            });
            choices.add(row);
            checks.add(check);
            group.addView(row, match(dp(38)));
        }
        return group;
    }

    private View heading(String title, int drawable, TextView value, ImageView chevron,
                         OnClickListener clicked) {
        LinearLayout row = new LinearLayout(getContext());
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(14), 0, dp(14), 0);
        row.setContentDescription(title);
        row.setFocusable(true);
        row.setOnClickListener(clicked);
        int color = value == null ? DESTRUCTIVE : PRIMARY;
        // SF symbols share an 18pt column but are not forced into the same square: sparkles
        // and trash are taller. Menu-only vectors preserve their actual visible path size.
        int iconHeight = drawable == R.drawable.ic_ai_menu_sparkles ? 22
            : drawable == R.drawable.ic_ai_menu_trash ? 20 : 18;
        row.addView(icon(drawable, color), new LayoutParams(dp(18), dp(iconHeight)));
        LinearLayout labels = new LinearLayout(getContext());
        labels.setOrientation(VERTICAL);
        labels.setGravity(Gravity.CENTER_VERTICAL);
        TextView caption = text(title, 14, color);
        caption.setTypeface(medium());
        labels.addView(caption, match(LayoutParams.WRAP_CONTENT));
        if (value != null) {
            value.setSingleLine(true);
            value.setEllipsize(android.text.TextUtils.TruncateAt.END);
            LayoutParams detail = match(LayoutParams.WRAP_CONTENT);
            detail.topMargin = dp(2);
            labels.addView(value, detail);
        }
        LayoutParams labelParams = new LayoutParams(0, LayoutParams.WRAP_CONTENT, 1);
        labelParams.leftMargin = dp(12);
        row.addView(labels, labelParams);
        if (chevron != null) row.addView(chevron, new LayoutParams(dp(11), dp(11)));
        return row;
    }

    private ImageView icon(int drawable, int tint) {
        ImageView icon = new ImageView(getContext());
        icon.setImageResource(drawable);
        icon.setColorFilter(tint);
        icon.setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
        return icon;
    }

    private void addDivider() {
        View divider = new View(getContext());
        divider.setBackgroundColor(Color.rgb(198, 198, 200));
        LayoutParams params = match(1);
        params.leftMargin = dp(42);
        addView(divider, params);
    }

    private TextView text(String text, int size, int color) {
        return MaiChatTheme.text(getContext(), text, size, color);
    }

    private static Typeface medium() {
        return Build.VERSION.SDK_INT >= 28 ? Typeface.create(Typeface.SANS_SERIF, 500, false)
            : Typeface.create("sans-serif-medium", Typeface.NORMAL);
    }

    private LayoutParams match(int height) { return new LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, height); }
    private int dp(float value) { return Math.round(value * getResources().getDisplayMetrics().density); }
}

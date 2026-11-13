// 文件作用：桌面端共用界面；ComposerTextEdit 负责纯文本输入、内联附件及编辑操作，本文件给出其声明。
// 类型关系：ComposerTextEdit : public QTextEdit；审阅时可据此追踪父类、接口或回调契约。
// 对外边界：本文件给出 ComposerTextEdit 的类型和函数声明；参数、返回值和对象生命周期以此为准。
// 公开入口：hasLocalFile。
#pragma once

#include <QTextEdit>
#include <functional>

class QMimeData;
class QResizeEvent;
class QWidget;

// IM 与 AI 助手共用的输入框外壳，统一拖放、粘贴和角落按钮位置。
// 来源文字只取纯文本；Markdown-only 按 UTF-8 字面输入，无纯文本的 HTML 不插入。
// 业务 MIME handler 优先接收完整来源，文档 API 插入内联附件不受纯文本策略影响。
// 文档图片资源由普通 FFmpeg 核心解码；失败返回 typed null，阻止 Qt 按路径重试。
// 附件如何入库、如何发送仍由各自页面的业务层决定。
class ComposerTextEdit : public QTextEdit {
    Q_OBJECT
public:
    explicit ComposerTextEdit(QWidget* parent = nullptr);

    // 返回 true 表示附件已被业务层消费，不再执行 QTextEdit 的默认插入。
    void setMimeHandler(std::function<bool(const QMimeData*)> handler);
    void setCornerAction(QWidget* action);
    void setLeadingAction(QWidget* action);
    void setSecondaryLeadingAction(QWidget* action);
    void setLeadingHint(QWidget* hint);
    void positionCornerAction();

protected:
    void resizeEvent(QResizeEvent* event) override;
    bool canInsertFromMimeData(const QMimeData* source) const override;
    void insertFromMimeData(const QMimeData* source) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;

private:
    static bool hasLocalFile(const QMimeData* source);

    QWidget* cornerAction_ = nullptr;
    QWidget* leadingAction_ = nullptr;
    QWidget* secondaryLeadingAction_ = nullptr;
    QWidget* leadingHint_ = nullptr;
    std::function<bool(const QMimeData*)> mimeHandler_;
};

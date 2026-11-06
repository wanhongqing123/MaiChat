// Shared local/remote tool disclosure. The panel owns the transcript position;
// this widget keeps the same compact presentation and detail layout in both.
#pragma once

#include <QFrame>
#include <QString>
#include <QStringList>

class QPushButton;
class QVBoxLayout;

QString agentToolStateText(const QString& state);
QString agentToolGroupTitle(const QStringList& states);

class AgentToolGroupWidget : public QFrame {
public:
    explicit AgentToolGroupWidget(const QString& id, QWidget* parent = nullptr);

    const QString& id() const;
    void addDetail(QWidget* detail);
    // Removed details follow the existing local card lifecycle: hide now and
    // delete after the current event, so delayed UI callbacks remain safe.
    void removeDetail(QWidget* detail);
    int detailCount() const;
    void setExpanded(bool expanded);
    bool isExpanded() const;
    void setTitle(const QString& title);
    QPushButton* disclosureButton() const;

protected:
    // Runs before a user toggle. Local groups clear their approval auto-expand
    // marker here without duplicating the common disclosure click handler.
    virtual void disclosureClicked();

private:
    void refreshTitle();

    QString id_;
    QString title_;
    QPushButton* summary_ = nullptr;
    QWidget* details_ = nullptr;
    QVBoxLayout* detailsLayout_ = nullptr;
    bool expanded_ = false;
};

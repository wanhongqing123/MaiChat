#include "agent/StarterCatalog.h"

#include <QSet>

#include <cstdio>
#include <initializer_list>

namespace {
int checks = 0;
int failures = 0;
#define CHECK(value)                                                          \
    do {                                                                      \
        ++checks;                                                             \
        if (!(value)) {                                                       \
            std::fprintf(stderr, "check failed: %d: %s\n", __LINE__, #value); \
            ++failures;                                                       \
        }                                                                     \
    } while (false)

std::vector<MaiToolSpec> specs(std::initializer_list<const char*> names) {
    std::vector<MaiToolSpec> result;
    for (const auto* name : names) result.push_back({name, "public fixture capability", "{}"});
    return result;
}

const StarterEntry* named(const QList<StarterEntry>& entries, const QString& name) {
    for (const auto& entry : entries)
        if (entry.name == name) return &entry;
    return nullptr;
}

QSet<QString> publicNames(const QList<StarterEntry>& entries) {
    QSet<QString> names;
    for (const auto& entry : entries) {
        for (const auto& name : entry.toolNames) {
            CHECK(!names.contains(name));
            names.insert(name);
        }
    }
    return names;
}

void screenAndInputHaveOneTruthfulCard() {
    for (const bool computer : {false, true}) {
        for (const bool screenshot : {false, true}) {
            std::vector<MaiToolSpec> available;
            QSet<QString> expected;
            if (computer) {
                available.push_back({"desktop_computer", "ordinary input", "{}"});
                expected.insert(QStringLiteral("desktop_computer"));
            }
            if (screenshot) {
                available.push_back({"screenshot", "ordinary capture", "{}"});
                expected.insert(QStringLiteral("screenshot"));
            }
            const auto entries = desktopStarterCatalog(available);
            CHECK(!named(entries, QStringLiteral("AI 问答")));
            CHECK(!named(entries, QStringLiteral("桌面截屏")));
            CHECK(publicNames(entries) == expected);
            int related = 0;
            for (const auto& entry : entries)
                if (entry.toolNames.contains(QStringLiteral("desktop_computer")) ||
                    entry.toolNames.contains(QStringLiteral("screenshot")))
                    ++related;
            CHECK(related == (computer || screenshot ? 1 : 0));
            const auto* card = named(entries, QStringLiteral("电脑操作"));
            CHECK(bool(card) == (computer || screenshot));
            if (!card) {
                CHECK(entries.isEmpty());
                continue;
            }
            CHECK(card->toolNames.size() == (computer ? 1 : 0) + (screenshot ? 1 : 0));
            CHECK(card->toolNames.contains(QStringLiteral("desktop_computer")) == computer);
            CHECK(card->toolNames.contains(QStringLiteral("screenshot")) == screenshot);
            CHECK(!card->example.contains(QStringLiteral("observe")));
            CHECK(!card->example.contains(QStringLiteral("click")));
            CHECK(!card->example.contains(QStringLiteral("type")));
            if (computer) {
                CHECK(card->purpose == QStringLiteral("像人一样查看屏幕，使用鼠标和键盘操作电脑。"));
                CHECK(card->example.contains(QStringLiteral("desktop_computer")));
                if (!screenshot) CHECK(!card->example.contains(QStringLiteral("screenshot")));
            } else {
                CHECK(card->purpose.contains(QStringLiteral("屏幕")));
                CHECK(!card->purpose.contains(QStringLiteral("鼠标")));
                CHECK(!card->purpose.contains(QStringLiteral("键盘")));
                CHECK(!card->purpose.contains(QStringLiteral("点击")));
                CHECK(!card->purpose.contains(QStringLiteral("拖动")));
                CHECK(card->example.contains(QStringLiteral("screenshot")));
                CHECK(!card->example.contains(QStringLiteral("desktop_computer")));
            }
        }
    }
}

void eachCallUsesOnlyItsPublicDeclarations() {
    for (const auto& available :
         {specs({"screenshot", "desktop_computer", "file_read", "future_public_tool"}), specs({"file_read"}),
          specs({"desktop_computer"}), specs({}), specs({"screenshot"})}) {
        const auto entries = desktopStarterCatalog(available);
        QSet<QString> expected;
        for (const auto& tool : available) expected.insert(QString::fromStdString(tool.name));
        CHECK(publicNames(entries) == expected);
        CHECK(!named(entries, QStringLiteral("AI 问答")));
        CHECK(!named(entries, QStringLiteral("图片创作")));
        CHECK(!named(entries, QStringLiteral("视频创作")));
        for (const auto& entry : entries) {
            CHECK(!entry.toolNames.contains(QStringLiteral("wan_image")));
            CHECK(!entry.toolNames.contains(QStringLiteral("wan_video")));
            CHECK(!entry.toolNames.contains(QStringLiteral("seedance_video")));
        }
    }
    const auto withFutureTool = desktopStarterCatalog(specs({"future_public_tool"}));
    CHECK(withFutureTool.size() == 1);
    CHECK(named(withFutureTool, QStringLiteral("future_public_tool")));
}
void genuinelyPublicVideoToolsKeepTheirExistingCard() {
    const auto entries = desktopStarterCatalog(specs({"minimax_video", "kling_bailian_video", "desktop_computer"}));
    const auto* video = named(entries, QStringLiteral("视频创作"));
    CHECK(video);
    if (video) {
        CHECK(video->toolNames.size() == 2);
        CHECK(video->toolNames.contains(QStringLiteral("minimax_video")));
        CHECK(video->toolNames.contains(QStringLiteral("kling_bailian_video")));
        CHECK(!video->toolNames.contains(QStringLiteral("wan_video")));
        CHECK(!video->toolNames.contains(QStringLiteral("seedance_video")));
    }
    CHECK(!named(entries, QStringLiteral("AI 问答")));
}

void shellSessionsShareOneTerminalCard() {
    const auto entries = desktopStarterCatalog(specs({"shell", "shell_input"}));
    const auto* terminal = named(entries, QStringLiteral("终端与进程"));
    CHECK(terminal);
    CHECK(entries.size() == 1);
    if (!terminal) return;
    CHECK(terminal->toolNames == (QStringList{QStringLiteral("shell"), QStringLiteral("shell_input")}));
    CHECK(terminal->purpose.contains(QStringLiteral("会话")));
    CHECK(terminal->purpose.contains(QStringLiteral("补读")));
    CHECK(terminal->purpose.contains(QStringLiteral("输入")));
    CHECK(terminal->purpose.contains(QStringLiteral("停止")));
    CHECK(terminal->example.contains(QStringLiteral("长时间")));
    CHECK(terminal->example.contains(QStringLiteral("shell_input")));
}

void retiredProcessNamesAreNotTerminalFamilyMembers() {
    // Synthetic retired declarations exercise the catalog's family boundary;
    // production registration is independently checked in AgentPanelSessionTest.
    const auto entries = desktopStarterCatalog(specs({"shell", "shell_input", "desktop_process_start",
        "desktop_process_read", "desktop_process_write", "desktop_process_stop"}));
    const auto* terminal = named(entries, QStringLiteral("终端与进程"));
    CHECK(terminal);
    if (!terminal) return;
    for (const auto* name : {"desktop_process_start", "desktop_process_read", "desktop_process_write", "desktop_process_stop"})
        CHECK(!terminal->toolNames.contains(QString::fromLatin1(name)));
}

void shellInputAloneDescribesAnExistingSession() {
    const auto entries = desktopStarterCatalog(specs({"shell_input"}));
    const auto* terminal = named(entries, QStringLiteral("终端与进程"));
    CHECK(terminal);
    CHECK(entries.size() == 1);
    if (!terminal) return;
    CHECK(terminal->toolNames == QStringList{QStringLiteral("shell_input")});
    CHECK(terminal->purpose.contains(QStringLiteral("已有")));
    CHECK(terminal->example.contains(QStringLiteral("已有")));
    CHECK(terminal->example.contains(QStringLiteral("shell_input")));
}
}  // namespace

int main() {
    screenAndInputHaveOneTruthfulCard();
    eachCallUsesOnlyItsPublicDeclarations();
    genuinelyPublicVideoToolsKeepTheirExistingCard();
    shellSessionsShareOneTerminalCard();
    retiredProcessNamesAreNotTerminalFamilyMembers();
    shellInputAloneDescribesAnExistingSession();
    std::printf("starter catalog checks: %d, failures: %d\n", checks, failures);
    return failures ? 1 : 0;
}

// Test-only Qt controls; never linked into the emulator or game release.
#include <QApplication>
#include <QAction>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include <QTest>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <dlfcn.h>

namespace {
QWidget *Window() {
    if (auto *modal = QApplication::activeModalWidget()) return modal;
    for (auto *widget : QApplication::topLevelWidgets())
        if (QString(widget->metaObject()->className()) == "MainWindow") return widget;
    return nullptr;
}

void Action(const QJsonObject &value) {
    auto *window = Window();
    if (!window) return;
    const auto operation = value["op"].toString();
    if (operation == "shot") window->grab().save(value["path"].toString());
    else if (operation == "key") {
        const auto key = static_cast<Qt::Key>(value["key"].toInt());
        QTest::keyPress(window, key);
        QPointer<QWidget> target(window);
        QTimer::singleShot(100, qApp, [target, key] { if (target) QTest::keyRelease(target, key); });
    } else if (operation == "touch") {
        // Disposable profiles use equally sized, vertical screens without a gap.
        for (auto *panel : window->findChildren<QWidget *>()) {
            if (!panel->inherits("ScreenPanel")) continue;
            const auto scale = std::min(panel->width() / 256.0, panel->height() / 384.0);
            const QPoint position((panel->width() - 256 * scale) / 2 + (value["x"].toInt() + 0.5) * scale,
                                  (panel->height() - 384 * scale) / 2 + (192 + value["y"].toInt() + 0.5) * scale);
            if (value["state"] == "press") QTest::mousePress(panel, Qt::LeftButton, Qt::NoModifier, position);
            else QTest::mouseRelease(panel, Qt::LeftButton, Qt::NoModifier, position);
            return;
        }
    } else if (operation == "menu") {
        for (auto *entry : window->findChildren<QAction *>()) {
            auto *menu = qobject_cast<QMenu *>(entry->parent());
            if (entry->text() == value["text"].toString() &&
                (!value.contains("parent") || (menu && menu->title() == value["parent"].toString()))) {
                entry->trigger(); return;
            }
        }
    } else if (operation == "text") {
        if (auto *entry = qobject_cast<QLineEdit *>(QApplication::focusWidget())) {
            entry->setText(value["text"].toString());
            QTest::keyClick(entry, Qt::Key_Return);
        }
    } else if (operation == "quit") { window->close(); qApp->quit(); }
}
} // namespace

int QApplication::exec() {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    auto *timer = new QTimer(qApp);
    QObject::connect(timer, &QTimer::timeout, qApp, [] {
        QFile input(qEnvironmentVariable("WT_CONTROL"));
        if (!input.open(QIODevice::ReadOnly)) return;
        const auto events = QJsonDocument::fromJson(input.readAll()).array();
        input.close(); input.remove();
        for (const auto event : events) {
            const auto value = event.toObject();
            QTimer::singleShot(value["ms"].toInt(), qApp, [value] { Action(value); });
        }
    });
    timer->start(100);
    const auto original = reinterpret_cast<int (*)()>(dlsym(RTLD_NEXT, "_ZN12QApplication4execEv"));
    return original();
}

// SPDX-License-Identifier: Apache-2.0
#include "find_command.h"

#include <algorithm>

#include <QEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "theme.h"

namespace groomlab::app {

namespace {

constexpr int kMaxShown = 12;
constexpr int kEntryRole = Qt::UserRole;

}  // namespace

FindCommand::FindCommand(std::vector<Entry> entries, QWidget* parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint), entries_(std::move(entries)) {
    setObjectName(QStringLiteral("findCommand"));
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("#findCommand { background: %1; border: 1px solid %2; border-radius: %3px; }")
                      .arg((theme::uiColor("field").isValid() ? theme::uiColor("field") : theme::card()).name(),
                           (theme::uiColor("popupEdge").isValid() ? theme::uiColor("popupEdge") : theme::divider()).name())
                      .arg(theme::cardRadius()));
    setFixedWidth(460);
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(8, 8, 8, 8);
    column->setSpacing(6);
    box_ = new QLineEdit(this);
    box_->setObjectName(QStringLiteral("findCommandBox"));
    box_->setPlaceholderText(tr("Find a command: alembic, comb, bake, preset..."));
    box_->setClearButtonEnabled(true);
    box_->installEventFilter(this);
    connect(box_, &QLineEdit::textChanged, this, &FindCommand::filter);
    column->addWidget(box_);
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("findCommandList"));
    list_->setFrameShape(QFrame::NoFrame);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setUniformItemSizes(true);
    connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) { run(item); });
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) { run(item); });
    column->addWidget(list_);
    foot_ = new QLabel(this);
    foot_->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;").arg(theme::textMuted().name()));
    column->addWidget(foot_);
    filter({});
}

double FindCommand::score(const QString& query, const QString& text) {
    const QString q = query.trimmed().toLower();
    if (q.isEmpty()) return 1.0;
    const QStringList words = q.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.size() > 1) {
        double worst = 1e9;
        for (const QString& w : words) worst = std::min(worst, score(w, text));
        return worst;
    }
    const QString t = text.toLower();
    const int at = t.indexOf(q);
    if (at == 0) return 3.0;
    if (at > 0) return t[at - 1].isLetterOrNumber() ? 2.0 : 2.5;
    int k = 0;
    for (const QChar c : t) {
        if (k < q.size() && c == q[k]) ++k;
    }
    return k == q.size() ? 1.0 : 0.0;
}

void FindCommand::filter(const QString& text) {
    list_->clear();
    std::vector<int> shown;
    if (text.trimmed().isEmpty()) {
        // Nothing typed: a few brushes, then a few commands.
        int brushes = 0, commands = 0;
        for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
            const bool brush = entries_[static_cast<std::size_t>(i)].kind == tr("Brush");
            if (brush && brushes < 4) shown.push_back(i), ++brushes;
            else if (!brush && commands < 6) shown.push_back(i), ++commands;
        }
    } else {
        std::vector<std::pair<double, int>> ranked;
        for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
            const Entry& e = entries_[static_cast<std::size_t>(i)];
            const double s = std::max(score(text, e.label) * 2.0, score(text, e.detail) * 0.6);
            if (s > 0.0) ranked.emplace_back(-s, i);
        }
        std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [s, i] : ranked) {
            if (static_cast<int>(shown.size()) >= kMaxShown) break;
            shown.push_back(i);
        }
    }
    for (const int i : shown) {
        const Entry& e = entries_[static_cast<std::size_t>(i)];
        auto* item = new QListWidgetItem(QStringLiteral("%1    · %2").arg(e.label, e.kind), list_);
        item->setToolTip(e.detail);
        item->setData(kEntryRole, i);
    }
    if (list_->count() > 0) list_->setCurrentRow(0);
    const int rows = std::clamp(list_->count(), 1, kMaxShown);
    const int row = std::max({list_->sizeHintForRow(0), list_->fontMetrics().height() + 8, 22});
    list_->setFixedHeight(rows * row + 2 * list_->frameWidth() + 6);
    foot_->setText(text.trimmed().isEmpty() ? tr("%1 commands, brushes and presets · Enter runs · Esc closes").arg(entries_.size())
                                            : tr("%n found", "", list_->count()));
    adjustSize();
}

bool FindCommand::runCurrent() {
    QListWidgetItem* item = list_->currentItem();
    if (!item) return false;
    run(item);
    return true;
}

void FindCommand::run(QListWidgetItem* item) {
    if (!item) return;
    const int i = item->data(kEntryRole).toInt();
    if (i < 0 || i >= static_cast<int>(entries_.size())) return;
    const std::function<void()> fn = entries_[static_cast<std::size_t>(i)].run;
    close();
    // (After the popup is gone: a command may open a dialog of its own.)
    if (!fn) return;
    if (QWidget* host = parentWidget()) QTimer::singleShot(0, host, fn);
    else QTimer::singleShot(0, fn);
}

bool FindCommand::eventFilter(QObject* watched, QEvent* event) {
    if (watched == box_ && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Down || key == Qt::Key_Up) {
            const int row = list_->currentRow() + (key == Qt::Key_Down ? 1 : -1);
            list_->setCurrentRow(std::clamp(row, 0, std::max(0, list_->count() - 1)));
            return true;
        }
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            runCurrent();
            return true;
        }
        if (key == Qt::Key_Escape) {
            close();
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}

void FindCommand::openOver(QWidget* window) {
    if (window) {
        const QPoint at = window->mapToGlobal(QPoint((window->width() - width()) / 2, 60));
        move(at);
    }
    show();
    box_->setFocus();
}

}  // namespace groomlab::app

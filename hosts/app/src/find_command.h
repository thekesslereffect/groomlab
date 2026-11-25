// SPDX-License-Identifier: Apache-2.0
#pragma once

// Find Command (GroomLab > Find Command..., Ctrl+K, the Groom Tools' search): one box over every
// command the app runs (the menu bar's actions, from the shared definitions), every brush and every
// graph preset, as Maya's find_command.py. Type to filter (words in any order; letters in order
// match too), Up / Down to move, Enter runs, Esc closes. It is how the menus and panels can stay
// short: anything not on screen is a few keys away. The window (MainWindow::openFindCommand) hands
// it the entries.

#include <functional>
#include <vector>

#include <QFrame>
#include <QString>

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;

namespace groomlab::app {

class FindCommand : public QFrame {
    Q_OBJECT
public:
    struct Entry {
        QString label;   // "Export Alembic"
        QString detail;  // its tooltip: what it does
        QString kind;    // where it lives: a menu ("Export"), "Brush" or "Preset"
        std::function<void()> run;
    };

    explicit FindCommand(std::vector<Entry> entries, QWidget* parent = nullptr);

    // How well `text` matches `query` (0: not at all): a word start best, then anywhere, then the
    // query's letters in order; every word of a query of several must match.
    static double score(const QString& query, const QString& text);
    // Opened over `window` (centered near its top), the box focused.
    void openOver(QWidget* window);
    void filter(const QString& text);  // the list for what is typed
    bool runCurrent();                 // Enter: the current row run (the popup closed first)
    QLineEdit* box() const { return box_; }
    QListWidget* list() const { return list_; }
    const std::vector<Entry>& entries() const { return entries_; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void run(QListWidgetItem* item);

    std::vector<Entry> entries_;
    QLineEdit* box_ = nullptr;
    QListWidget* list_ = nullptr;
    QLabel* foot_ = nullptr;
};

}  // namespace groomlab::app

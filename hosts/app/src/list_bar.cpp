// SPDX-License-Identifier: Apache-2.0
#include "list_bar.h"

#include <algorithm>

#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QToolButton>

#include "card_parts.h"
#include "glass.h"
#include "theme.h"

namespace groomlab::app {

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

}  // namespace

QIcon glyphIcon(const std::string& glyph, qreal dpr, int size) {
    const QString resource = QStringLiteral(":/groomlab/icons/ui_glyph_%1.svg").arg(qs(glyph));
    QIcon icon;
    icon.addPixmap(tintedIcon(resource, theme::text(), size, dpr), QIcon::Normal, QIcon::Off);
    icon.addPixmap(tintedIcon(resource, theme::textMuted(), size, dpr), QIcon::Disabled, QIcon::Off);
    return icon;
}

QLabel* aboutLabel(const std::string& html, QWidget* parent) {
    auto* label = new QLabel(qs(html), parent);
    label->setProperty("role", "about");
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    return label;
}

ListBar::ListBar(QWidget* parent) : QWidget(parent) { setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed); }

namespace {
// A header's glyph for a labeled entry (+ New..., Import..., Fix Frame); none: a chip.
QString headerIcon(const std::string& id) {
    auto ends = [&id](const char* tail) {
        const std::string t(tail);
        return id.size() >= t.size() && id.compare(id.size() - t.size(), t.size(), t) == 0;
    };
    if (ends(".new")) return QStringLiteral("plus");
    if (ends(".import")) return QStringLiteral("image_plus");
    if (ends(".fixFrame")) return QStringLiteral("key");
    return {};
}
}  // namespace

void ListBar::build(const ui::json& entries, const ui::Bindings& bindings, const ui::FillOptions& given, Part part) {
    for (auto& [id, b] : buttons_) b->deleteLater();
    buttons_.clear();
    enables_.clear();
    ui::FillOptions options = given;
    options.liveShortcuts = false;
    const ui::Definitions& d = ui::Definitions::shipped();
    ui::ResolveOptions resolve = options.resolve;
    resolve.state.reset();  // a bar shows every entry; the bindings say what is enabled
    resolve.keep = [&bindings](const std::string& id) { return bindings.count(id) > 0; };
    resolve.dropGenerators = true;
    std::vector<QToolButton*> chips, glyphs;
    for (const ui::Item& item : d.resolve(entries, resolve)) {
        if (item.kind != ui::Item::Kind::Action && item.kind != ui::Item::Kind::Submenu) continue;
        const bool drawn = !item.glyph.empty();
        if ((part == Part::Labeled && drawn) || (part == Part::Glyphs && !drawn)) continue;
        auto* b = new QToolButton(this);
        b->setObjectName(QStringLiteral("listBar_") + qs(item.id));
        b->setToolTip(qs(item.tip));
        const QString header = part == Part::Labeled ? headerIcon(item.id) : QString();
        if (drawn) {
            // A drawn glyph on the right, in the theme's text color.
            b->setProperty("role", "glyph");
            b->setAutoRaise(true);
            b->setFixedSize(24, 24);
            b->setIcon(glyphIcon(item.glyph, devicePixelRatioF(), 16));
            b->setIconSize(QSize(16, 16));
            b->setProperty("glyph", qs(item.glyph));
            glyphs.push_back(b);
        } else if (!header.isEmpty()) {
            // A list header's: the entry as a line glyph, its name in the tip (and its text).
            b->setProperty("role", "glyph");
            b->setAutoRaise(true);
            b->setFixedSize(24, 24);
            b->setText(qs(item.label));
            b->setToolButtonStyle(Qt::ToolButtonIconOnly);
            b->setIcon(lineIcon(header, devicePixelRatioF(), 15));
            b->setIconSize(QSize(15, 15));
            b->setProperty("glyph", header);
            QString name = qs(item.label);
            name.remove(QStringLiteral("+ ")).remove(QString::fromUtf8("\u25C6 "));
            b->setToolTip(item.tip.empty() ? name : QStringLiteral("<b>%1</b><br>%2").arg(name.toHtmlEscaped(), qs(item.tip).toHtmlEscaped()));
            glyphs.push_back(b);
        } else {
            b->setProperty("role", "chip");
            b->setText(qs(item.label));
            chips.push_back(b);
        }
        if (item.kind == ui::Item::Kind::Submenu) {
            // The submenu's own entries (its "ref"), the bindings running them.
            auto* menu = new QMenu(b);
            menu->setObjectName(QStringLiteral("listMenu_") + qs(item.id));
            const std::string ref = item.entry.value("ref", std::string());
            ui::FillOptions sub = options;
            sub.resolve.state.reset();
            if (!ref.empty() && d.find(ref)) ui::fillMenu(menu, d.get(ref), bindings, sub);
            else if (item.entry.contains("items")) ui::fillMenu(menu, item.entry["items"], bindings, sub);
            b->setMenu(menu);
            b->setPopupMode(QToolButton::InstantPopup);
            if (const auto it = bindings.find(item.id); it != bindings.end() && it->second.enabled) enables_.emplace_back(b, it->second.enabled);
        } else {
            const ui::Binding binding = bindings.at(item.id);
            connect(b, &QToolButton::clicked, this, [binding, value = item.value] {
                if (binding.trigger) binding.trigger(value, false);
            });
            if (binding.enabled) enables_.emplace_back(b, binding.enabled);
        }
        buttons_.emplace_back(item.id, b);
    }
    for (auto& [id, b] : buttons_) b->show();
    (void)chips;
    (void)glyphs;
    updateGeometry();
    place();
    refresh();
}

namespace {
constexpr int kSpacing = 4;
bool isGlyph(const QToolButton* b) { return b->property("role").toString() == QLatin1String("glyph"); }
QSize sizeOf(const QToolButton* b) { return b->minimumSize() == b->maximumSize() ? b->minimumSize() : b->sizeHint(); }
}  // namespace

int ListBar::rowWidth(bool chips) const {
    int w = 0, n = 0;
    for (const auto& [id, b] : buttons_) {
        if (isGlyph(b) == chips) continue;
        w += sizeOf(b).width();
        ++n;
    }
    return n ? w + (n - 1) * kSpacing : 0;
}

int ListBar::rowHeight() const {
    int h = 0;
    for (const auto& [id, b] : buttons_) h = std::max(h, sizeOf(b).height());
    return h;
}

int ListBar::heightForWidth(int width) const {
    const int chips = rowWidth(true), glyphs = rowWidth(false);
    const bool one = chips + glyphs + (chips && glyphs ? 3 * kSpacing : 0) <= width;
    return one || !chips || !glyphs ? rowHeight() : 2 * rowHeight() + kSpacing;
}

QSize ListBar::sizeHint() const {
    const int chips = rowWidth(true), glyphs = rowWidth(false);
    return {chips + glyphs + 3 * kSpacing, rowHeight()};
}

QSize ListBar::minimumSizeHint() const { return {std::max(rowWidth(true), rowWidth(false)), rowHeight()}; }

void ListBar::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    place();
}

void ListBar::setTitle(const QString& text) {
    if (!title_) {
        title_ = new QLabel(this);
        title_->setObjectName(QStringLiteral("listBarTitle"));
        QFont font = title_->font();
        font.setWeight(QFont::DemiBold);
        title_->setFont(font);
    }
    titleText_ = text;
    title_->setToolTip(text);
    title_->setVisible(!text.isEmpty());
    place();
}

void ListBar::place() {
    const int width = this->width();
    const int h = rowHeight();
    const int chips = rowWidth(true), glyphs = rowWidth(false);
    if (title_ && !titleText_.isEmpty() && !chips) {
        // The picked item's name in the room left of the glyphs.
        const int room = std::max(0, width - glyphs - 2 * kSpacing);
        title_->setText(title_->fontMetrics().elidedText(titleText_, Qt::ElideRight, room));
        const int lh = title_->sizeHint().height();
        title_->setGeometry(0, (std::max(h, lh) - lh) / 2, room, lh);
    }
    const bool one = chips + glyphs + (chips && glyphs ? 3 * kSpacing : 0) <= width;
    int x = 0;
    for (const auto& [id, b] : buttons_) {
        if (isGlyph(b)) continue;
        const QSize s = sizeOf(b);
        b->setGeometry(x, (h - s.height()) / 2, s.width(), s.height());
        x += s.width() + kSpacing;
    }
    // The glyphs right-aligned, beside the chips or on a row of their own.
    const int top = one || !chips ? 0 : h + kSpacing;
    x = std::max(0, width - glyphs);
    for (const auto& [id, b] : buttons_) {
        if (!isGlyph(b)) continue;
        const QSize s = sizeOf(b);
        b->setGeometry(x, top + (h - s.height()) / 2, s.width(), s.height());
        x += s.width() + kSpacing;
    }
    const int wanted = heightForWidth(width);
    if (height() != wanted) {
        setFixedHeight(wanted);
        updateGeometry();
    }
}

QToolButton* ListBar::button(const std::string& id) const {
    for (const auto& [i, b] : buttons_) {
        if (i == id) return b;
    }
    return nullptr;
}

std::vector<std::string> ListBar::ids() const {
    std::vector<std::string> out;
    for (const auto& [i, b] : buttons_) out.push_back(i);
    return out;
}

void ListBar::refresh() {
    for (auto& [button, enabled] : enables_) {
        if (button) button->setEnabled(enabled());
    }
}

QMenu* listMenu(QWidget* parent, const ui::json& entries, const ui::Bindings& bindings, const ui::FillOptions& given,
                const std::set<std::string>& state) {
    auto* menu = new QMenu(parent);
    menu->setObjectName(QStringLiteral("listContextMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    ui::FillOptions options = given;
    options.liveShortcuts = false;
    std::set<std::string> all = options.resolve.state.value_or(std::set<std::string>{});
    all.insert(state.begin(), state.end());
    options.resolve.state = all;
    ui::fillMenu(menu, entries, bindings, options);
    return menu;
}

}  // namespace groomlab::app

#include "FontPickerDialog.h"
#include "AppFonts.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

constexpr int kItemPointSize = 11;
constexpr int kPreviewPointSize = 11;

const QString kPreviewText = QStringLiteral(
    "TASK TITLE\nRancang ulang halaman login\nSpesifikasi menunggu review · 11:22:30 · 2 notifikasi\n"
    "AaBbCc 0123456789 — il1 O0");

}

FontPickerDialog::FontPickerDialog(const QStringList &families, const QString &current, QWidget *parent)
    : QDialog(parent) {
    setObjectName(QStringLiteral("fontPickerDialog"));
    setWindowTitle(QStringLiteral("Ganti font"));

    auto *title = new QLabel(QStringLiteral("Font antarmuka"), this);
    title->setObjectName(QStringLiteral("fontPickerTitle"));

    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("fontList"));

    // Baris pertama: font yang sekarang dipakai aplikasi (bawaan styles.qss)
    auto *defaultItem = new QListWidgetItem(AppFonts::defaultLabel(), m_list);
    defaultItem->setData(Qt::UserRole, QString());
    QFont defaultFont(QStringLiteral("Garamond"), kItemPointSize);
    defaultItem->setFont(defaultFont);

    // Tiap font ditulis dengan dirinya sendiri, jadi daftar ini sekaligus pratinjau
    for (const QString &family : families) {
        auto *item = new QListWidgetItem(family, m_list);
        item->setData(Qt::UserRole, family);
        item->setFont(QFont(family, kItemPointSize));
        if (family == current) {
            m_list->setCurrentItem(item);
        }
    }
    if (!m_list->currentItem()) {
        m_list->setCurrentItem(defaultItem);
    }

    m_preview = new QLabel(kPreviewText, this);
    m_preview->setObjectName(QStringLiteral("fontPreview"));
    m_preview->setWordWrap(true);
    m_preview->setMinimumHeight(90);

    auto *btnCancel = new QPushButton(QStringLiteral("Batal"), this);
    btnCancel->setObjectName(QStringLiteral("btnFontCancel"));
    btnCancel->setCursor(Qt::PointingHandCursor);
    auto *btnApply = new QPushButton(QStringLiteral("Terapkan"), this);
    btnApply->setObjectName(QStringLiteral("btnFontApply"));
    btnApply->setCursor(Qt::PointingHandCursor);
    btnApply->setDefault(true);

    connect(m_list, &QListWidget::currentItemChanged, this, &FontPickerDialog::refreshPreview);
    connect(m_list, &QListWidget::itemDoubleClicked, btnApply, &QPushButton::click);
    connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(btnApply, &QPushButton::clicked, this, [this]() {
        emit fontChosen(selectedFamily());
        accept();
    });

    auto *actions = new QHBoxLayout();
    actions->setSpacing(8);
    actions->addStretch(1);
    actions->addWidget(btnCancel);
    actions->addWidget(btnApply);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 14);
    root->setSpacing(10);
    root->addWidget(title);
    root->addWidget(m_list, 1);
    root->addWidget(m_preview);
    root->addLayout(actions);

    resize(420, 520);
    refreshPreview();
}

QString FontPickerDialog::selectedFamily() const {
    const QListWidgetItem *item = m_list->currentItem();
    return item ? item->data(Qt::UserRole).toString() : QString();
}

void FontPickerDialog::refreshPreview() {
    const QString family = selectedFamily();
    const QFont font(family.isEmpty() ? QStringLiteral("Garamond") : family, kPreviewPointSize);
    // Stylesheet aplikasi ("*" font-family) mengalahkan setFont; pratinjau memakai stylesheet sendiri
    m_preview->setStyleSheet(QStringLiteral("QLabel#fontPreview { font-family: \"%1\"; }").arg(font.family()));
    m_preview->setFont(font);
}

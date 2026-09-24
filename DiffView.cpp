#include "DiffView.h"
#include "WorkspaceDiff.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyle>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

// Diff raksasa (mis. hasil formatter di seluruh project) dipotong supaya drawer tetap responsif
constexpr int kMaxRenderedLines = 20000;
constexpr int kMaxListRows = 6;

enum Column { StatusColumn, PathColumn, AddedColumn, RemovedColumn };

// Warna mengikuti palet styles.qss
constexpr QRgb kHeaderBackground = 0xf2e3d1;
constexpr QRgb kHunkBackground = 0xeef2f8;
constexpr QRgb kAddedBackground = 0xe6ffec;
constexpr QRgb kRemovedBackground = 0xffebe9;
constexpr QRgb kNavyText = 0x33517a;
constexpr QRgb kGutterText = 0xa39887;
constexpr QRgb kNoteText = 0x8a7f70;
constexpr QRgb kAddedText = 0x2f7d32;
constexpr QRgb kRemovedText = 0xb3261e;
constexpr QRgb kModifiedText = 0xb7791f;

QString statusLetter(const FileDiff &file) {
    switch (file.status) {
    case FileDiff::Status::Added: return QStringLiteral("A");
    case FileDiff::Status::Deleted: return QStringLiteral("D");
    case FileDiff::Status::Renamed: return QStringLiteral("R");
    case FileDiff::Status::Modified: break;
    }
    return QStringLiteral("M");
}

QRgb statusColor(const FileDiff &file) {
    switch (file.status) {
    case FileDiff::Status::Added: return kAddedText;
    case FileDiff::Status::Deleted: return kRemovedText;
    case FileDiff::Status::Renamed: return kNavyText;
    case FileDiff::Status::Modified: break;
    }
    return kModifiedText;
}

QString statusDescription(const FileDiff &file) {
    switch (file.status) {
    case FileDiff::Status::Added:
        return file.untracked ? QStringLiteral("File baru (belum di-git add)") : QStringLiteral("File baru");
    case FileDiff::Status::Deleted: return QStringLiteral("Dihapus");
    case FileDiff::Status::Renamed: return QStringLiteral("Diganti nama dari %1").arg(file.oldPath);
    case FileDiff::Status::Modified: break;
    }
    return QStringLiteral("Diubah");
}

QString displayPath(const FileDiff &file) {
    return file.status == FileDiff::Status::Renamed ? QStringLiteral("%1 → %2").arg(file.oldPath, file.path)
                                                    : file.path;
}

QString lineNumber(int number) {
    return number > 0 ? QString::number(number) : QString();
}

QTextBlockFormat blockWith(QRgb background) {
    QTextBlockFormat format;
    format.setBackground(QColor(background));
    return format;
}

QTextCharFormat textWith(QRgb color) {
    QTextCharFormat format;
    format.setForeground(QColor(color));
    return format;
}

}

DiffView::DiffView(QWidget *parent)
    : QWidget(parent) {
    setObjectName("diffView");

    m_summary = new QLabel(this);
    m_summary->setObjectName("diffSummary");
    m_summary->setWordWrap(true);
    // Pesan gagal (mis. perintah safe.directory dari git) perlu bisa disalin
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_refresh = new QPushButton(QStringLiteral("Muat ulang"), this);
    m_refresh->setObjectName("btnDiffRefresh");
    m_refresh->setCursor(Qt::PointingHandCursor);
    m_refresh->setToolTip(QStringLiteral("Baca ulang perubahan dari folder kerja"));
    connect(m_refresh, &QPushButton::clicked, this, &DiffView::refreshRequested);

    m_files = new QTreeWidget(this);
    m_files->setObjectName("diffFileList");
    m_files->setColumnCount(4);
    m_files->setHeaderHidden(true);
    m_files->setRootIsDecorated(false);
    m_files->setUniformRowHeights(true);
    m_files->setTextElideMode(Qt::ElideMiddle);
    m_files->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QHeaderView *columns = m_files->header();
    columns->setStretchLastSection(false);
    columns->setSectionResizeMode(StatusColumn, QHeaderView::ResizeToContents);
    columns->setSectionResizeMode(PathColumn, QHeaderView::Stretch);
    columns->setSectionResizeMode(AddedColumn, QHeaderView::ResizeToContents);
    columns->setSectionResizeMode(RemovedColumn, QHeaderView::ResizeToContents);
    // Klik ulang file yang sama tetap melompat; panah keyboard lewat currentItemChanged
    connect(m_files, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
        scrollToFile(m_files->indexOfTopLevelItem(item));
    });
    connect(m_files, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *current) {
        scrollToFile(m_files->indexOfTopLevelItem(current));
    });

    m_text = new QPlainTextEdit(this);
    m_text->setObjectName("diffText");
    m_text->setReadOnly(true);
    m_text->setUndoRedoEnabled(false);
    m_text->setLineWrapMode(QPlainTextEdit::NoWrap);

    auto *header = new QHBoxLayout();
    header->setSpacing(8);
    header->addWidget(m_summary, 1);
    header->addWidget(m_refresh, 0, Qt::AlignTop);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(8);
    root->addLayout(header);
    root->addWidget(m_files);
    root->addWidget(m_text, 1);
    // Saat hanya pesan yang tampil (memuat / gagal / kosong), pesan tetap di atas
    root->addStretch();

    m_files->hide();
    m_text->hide();
}

void DiffView::showLoading() {
    showMessage(QStringLiteral("Membaca perubahan kode di folder kerja…"), false);
    m_refresh->setEnabled(false);
}

void DiffView::showDiff(const WorkspaceDiff &diff) {
    m_refresh->setEnabled(true);
    if (!diff.error.isEmpty()) {
        showMessage(diff.error, true);
        return;
    }
    if (diff.files.isEmpty()) {
        showMessage(QStringLiteral("Tidak ada perubahan yang belum di-commit di folder kerja."), false);
        return;
    }

    QString summary = QStringLiteral("%1 file berubah · +%2 −%3")
                          .arg(diff.files.size())
                          .arg(diff.added())
                          .arg(diff.removed());
    summary += diff.baseCommit.isEmpty() ? QStringLiteral(" · repository belum punya commit")
                                         : QStringLiteral(" · dibanding commit %1").arg(diff.baseCommit);
    if (diff.omittedUntracked > 0) {
        summary += QStringLiteral("\n%1 file baru lain tidak ditampilkan").arg(diff.omittedUntracked);
    }
    showMessage(summary, false);

    fillFileList(diff);
    renderDiff(diff);
    m_files->show();
    m_text->show();
}

void DiffView::showMessage(const QString &text, bool error) {
    m_summary->setText(text);
    if (m_summary->property("error").toBool() != error) {
        m_summary->setProperty("error", error);
        m_summary->style()->unpolish(m_summary);
        m_summary->style()->polish(m_summary);
    }

    const QSignalBlocker blocker(m_files);
    m_files->clear();
    m_files->hide();
    m_text->clear();
    m_text->hide();
    m_fileBlocks.clear();
}

void DiffView::fillFileList(const WorkspaceDiff &diff) {
    const QSignalBlocker blocker(m_files);
    QList<QTreeWidgetItem *> items;
    for (const FileDiff &file : diff.files) {
        auto *item = new QTreeWidgetItem();
        item->setText(StatusColumn, statusLetter(file));
        item->setForeground(StatusColumn, QColor(statusColor(file)));
        item->setToolTip(StatusColumn, statusDescription(file));
        item->setText(PathColumn, displayPath(file));
        item->setToolTip(PathColumn, QStringLiteral("%1\n%2").arg(file.path, statusDescription(file)));
        if (file.added > 0) {
            item->setText(AddedColumn, QStringLiteral("+%1").arg(file.added));
            item->setForeground(AddedColumn, QColor(kAddedText));
        }
        if (file.removed > 0) {
            item->setText(RemovedColumn, QStringLiteral("−%1").arg(file.removed));
            item->setForeground(RemovedColumn, QColor(kRemovedText));
        }
        item->setTextAlignment(AddedColumn, Qt::AlignRight | Qt::AlignVCenter);
        item->setTextAlignment(RemovedColumn, Qt::AlignRight | Qt::AlignVCenter);
        items.append(item);
    }
    m_files->addTopLevelItems(items);

    // Tinggi daftar mengikuti jumlah file sampai kMaxListRows baris, sisanya digulir
    m_files->ensurePolished();
    int rowHeight = m_files->sizeHintForRow(0);
    if (rowHeight <= 0) {
        rowHeight = m_files->fontMetrics().height() + 6;
    }
    const int rows = qMin(int(items.size()), kMaxListRows);
    m_files->setFixedHeight(rows * rowHeight + 2 * m_files->frameWidth() + 2);
}

void DiffView::renderDiff(const WorkspaceDiff &diff) {
    const QTextCharFormat plain;
    QTextCharFormat header;
    header.setFontWeight(QFont::Bold);
    const QTextCharFormat gutter = textWith(kGutterText);
    const QTextCharFormat hunk = textWith(kNavyText);
    QTextCharFormat note = textWith(kNoteText);
    note.setFontItalic(true);

    const QTextBlockFormat plainBlock;
    const QTextBlockFormat headerBlock = blockWith(kHeaderBackground);
    const QTextBlockFormat hunkBlock = blockWith(kHunkBackground);
    const QTextBlockFormat addedBlock = blockWith(kAddedBackground);
    const QTextBlockFormat removedBlock = blockWith(kRemovedBackground);

    QTextCursor cursor(m_text->document());
    cursor.beginEditBlock();
    bool firstBlock = true;
    auto startBlock = [&](const QTextBlockFormat &format) {
        if (firstBlock) {
            cursor.setBlockFormat(format);
            firstBlock = false;
        } else {
            cursor.insertBlock(format, plain);
        }
    };

    int budget = kMaxRenderedLines;
    for (const FileDiff &file : diff.files) {
        if (!firstBlock) {
            startBlock(plainBlock);   // jarak antar file
        }
        startBlock(headerBlock);
        m_fileBlocks.append(cursor.blockNumber());
        cursor.insertText(QStringLiteral(" %1  %2").arg(statusLetter(file), displayPath(file)), header);

        // Kolom nomor baris lama & baru selebar angka terbesar di file ini
        int width = 1;
        for (const DiffLine &line : file.lines) {
            width = qMax(width, int(QString::number(qMax(line.oldLine, line.newLine)).size()));
        }
        const QString blankGutter(width * 2 + 2, QLatin1Char(' '));
        auto addNote = [&](const QString &text) {
            startBlock(plainBlock);
            cursor.insertText(blankGutter + QStringLiteral("  ") + text, note);
        };

        if (!file.note.isEmpty()) {
            addNote(file.note);
        } else if (file.lines.isEmpty()) {
            addNote(QStringLiteral("Tidak ada perubahan isi"));
        }
        if (budget <= 0) {
            if (!file.lines.isEmpty()) {
                addNote(QStringLiteral("Isi tidak ditampilkan: diff melebihi %1 baris").arg(kMaxRenderedLines));
            }
            continue;
        }

        for (qsizetype i = 0; i < file.lines.size(); ++i) {
            if (budget-- == 0) {
                addNote(QStringLiteral("… %1 baris lagi tidak ditampilkan (diff melebihi %2 baris)")
                            .arg(file.lines.size() - i)
                            .arg(kMaxRenderedLines));
                break;
            }
            const DiffLine &line = file.lines.at(i);
            switch (line.kind) {
            case DiffLine::Kind::Hunk:
                startBlock(hunkBlock);
                cursor.insertText(blankGutter + line.text, hunk);
                break;
            case DiffLine::Kind::Note:
                addNote(line.text);
                break;
            case DiffLine::Kind::Added:
            case DiffLine::Kind::Removed:
            case DiffLine::Kind::Context: {
                const bool added = line.kind == DiffLine::Kind::Added;
                const bool removed = line.kind == DiffLine::Kind::Removed;
                startBlock(added ? addedBlock : removed ? removedBlock : plainBlock);
                cursor.insertText(QStringLiteral("%1 %2 ")
                                      .arg(lineNumber(line.oldLine), width)
                                      .arg(lineNumber(line.newLine), width),
                                  gutter);
                const QString marker = added ? QStringLiteral("+ ") : removed ? QStringLiteral("- ")
                                                                              : QStringLiteral("  ");
                cursor.insertText(marker + QString(line.text).replace(QLatin1Char('\t'), QStringLiteral("    ")),
                                  plain);
                break;
            }
            }
        }
    }
    cursor.endEditBlock();

    m_text->verticalScrollBar()->setValue(0);
    m_text->horizontalScrollBar()->setValue(0);
}

void DiffView::scrollToFile(int index) {
    if (index >= 0 && index < m_fileBlocks.size()) {
        // NoWrap: satu blok = satu baris, jadi nilai scrollbar = nomor blok teratas
        m_text->verticalScrollBar()->setValue(m_fileBlocks.at(index));
    }
}

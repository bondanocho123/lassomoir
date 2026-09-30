#include "DiffView.h"

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
// Dua kolom: bagian baris yang berubah, dan baris kosong pengganjal di sisi yang tidak punya pasangan
constexpr QRgb kAddedWordBackground = 0xabf2bc;
constexpr QRgb kRemovedWordBackground = 0xffc0bc;
constexpr QRgb kFillerBackground = 0xf6f1e9;
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

QString expandTabs(const QString &text) {
    return QString(text).replace(QLatin1Char('\t'), QStringLiteral("    "));
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

// Bagian tengah baris yang berbeda antara baris lama dan baris barunya: awal & akhir yang sama
// dilewati. Kosong (length 0 di kedua sisi) bila baris tidak cukup mirip untuk ditandai per bagian.
struct ChangedRange {
    int start = 0;
    int oldLength = 0;
    int newLength = 0;
};

ChangedRange changedRange(const QString &before, const QString &after) {
    const int shorter = int(qMin(before.size(), after.size()));
    int prefix = 0;
    while (prefix < shorter && before.at(prefix) == after.at(prefix)) {
        ++prefix;
    }
    int suffix = 0;
    while (suffix < shorter - prefix
           && before.at(before.size() - 1 - suffix) == after.at(after.size() - 1 - suffix)) {
        ++suffix;
    }
    // Yang sama minimal seperempat baris terpanjang; kalau kurang, barisnya memang lain sama sekali
    const int longer = int(qMax(before.size(), after.size()));
    if ((prefix + suffix) * 4 < longer) {
        return {};
    }
    return {prefix, int(before.size()) - prefix - suffix, int(after.size()) - prefix - suffix};
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

    // Satu kolom / dua kolom: sepasang tombol yang saling meniadakan
    m_modes = new QWidget(this);
    m_modes->setObjectName("diffModes");
    auto modeButton = [this](const QString &text, const char *name, const QString &tip) {
        auto *button = new QPushButton(text, m_modes);
        button->setObjectName(QLatin1String(name));
        button->setToolTip(tip);
        button->setCursor(Qt::PointingHandCursor);
        button->setCheckable(true);
        button->setAutoExclusive(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setAutoDefault(false);
        return button;
    };
    m_unifiedButton = modeButton(QStringLiteral("Satu kolom"), "btnDiffUnified",
                                 QStringLiteral("Baris lama (−) dan baris baru (+) bergantian dalam satu kolom"));
    m_splitButton = modeButton(QStringLiteral("Dua kolom"), "btnDiffSplit",
                               QStringLiteral("Sebelum (kiri) dan sesudah (kanan) berdampingan"));
    m_unifiedButton->setChecked(true);
    connect(m_unifiedButton, &QPushButton::clicked, this, [this]() { setSideBySide(false); });
    connect(m_splitButton, &QPushButton::clicked, this, [this]() { setSideBySide(true); });
    auto *modes = new QHBoxLayout(m_modes);
    modes->setContentsMargins(0, 0, 0, 0);
    modes->setSpacing(0);
    modes->addWidget(m_unifiedButton);
    modes->addWidget(m_splitButton);

    m_refresh = new QPushButton(QStringLiteral("Muat ulang"), this);
    m_refresh->setObjectName("btnDiffRefresh");
    m_refresh->setCursor(Qt::PointingHandCursor);
    m_refresh->setToolTip(QStringLiteral("Baca ulang perubahan dari folder kerja"));
    // Di dalam dialog (jendela branch & commit) Enter di kotak pencarian tidak boleh memicunya
    m_refresh->setAutoDefault(false);
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

    auto codeView = [this](const char *name, QWidget *parent) {
        auto *text = new QPlainTextEdit(parent);
        text->setObjectName(QLatin1String(name));
        text->setReadOnly(true);
        text->setUndoRedoEnabled(false);
        text->setLineWrapMode(QPlainTextEdit::NoWrap);
        return text;
    };
    m_text = codeView("diffText", this);

    // Dua kolom punya jumlah baris yang sama (baris pengganjal di sisi yang tidak punya pasangan),
    // jadi nilai scrollbar kiri dan kanan selalu menunjuk baris yang sejajar
    m_split = new QWidget(this);
    m_split->setObjectName("diffSplit");
    m_oldText = codeView("diffOldText", m_split);
    m_newText = codeView("diffNewText", m_split);
    auto *split = new QHBoxLayout(m_split);
    split->setContentsMargins(0, 0, 0, 0);
    split->setSpacing(6);
    split->addWidget(m_oldText, 1);
    split->addWidget(m_newText, 1);
    linkScrollBars(m_oldText->verticalScrollBar(), m_newText->verticalScrollBar());
    linkScrollBars(m_oldText->horizontalScrollBar(), m_newText->horizontalScrollBar());

    auto *header = new QHBoxLayout();
    header->setSpacing(8);
    header->addWidget(m_summary, 1);
    header->addWidget(m_modes, 0, Qt::AlignTop);
    header->addWidget(m_refresh, 0, Qt::AlignTop);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(8);
    root->addLayout(header);
    root->addWidget(m_files);
    root->addWidget(m_text, 1);
    root->addWidget(m_split, 1);
    // Saat hanya pesan yang tampil (memuat / gagal / kosong), pesan tetap di atas
    root->addStretch();

    m_modes->hide();
    m_files->hide();
    m_text->hide();
    m_split->hide();
}

void DiffView::showLoading(const QString &text) {
    showMessage(text.isEmpty() ? QStringLiteral("Membaca perubahan kode di folder kerja…") : text, false);
    m_refresh->setEnabled(false);
}

void DiffView::showDiff(const WorkspaceDiff &diff) {
    m_refresh->setEnabled(true);
    if (!diff.error.isEmpty()) {
        showMessage(diff.error, true);
        return;
    }
    const bool commits = !diff.targetCommit.isEmpty();
    if (diff.files.isEmpty()) {
        if (!commits) {
            showMessage(QStringLiteral("Tidak ada perubahan yang belum di-commit di folder kerja."), false);
        } else if (diff.baseCommit.isEmpty()) {
            showMessage(QStringLiteral("Commit %1 tidak berisi file di folder kerja.").arg(diff.targetCommit), false);
        } else {
            showMessage(QStringLiteral("Tidak ada perubahan file antara %1 dan %2.").arg(diff.baseCommit, diff.targetCommit),
                        false);
        }
        return;
    }

    QString summary = QStringLiteral("%1 file berubah · +%2 −%3")
                          .arg(diff.files.size())
                          .arg(diff.added())
                          .arg(diff.removed());
    if (commits) {
        summary += diff.baseCommit.isEmpty() ? QStringLiteral(" · commit pertama %1").arg(diff.targetCommit)
                                             : QStringLiteral(" · %1 → %2").arg(diff.baseCommit, diff.targetCommit);
    } else {
        summary += diff.baseCommit.isEmpty() ? QStringLiteral(" · repository belum punya commit")
                                             : QStringLiteral(" · dibanding commit %1").arg(diff.baseCommit);
    }
    if (diff.omittedUntracked > 0) {
        summary += QStringLiteral("\n%1 file baru lain tidak ditampilkan").arg(diff.omittedUntracked);
    }
    showMessage(summary, false);

    m_diff = diff;
    fillFileList(diff);
    render();
    m_modes->show();
    m_files->show();
}

void DiffView::showMessage(const QString &text, bool error) {
    m_summary->setText(text);
    if (m_summary->property("error").toBool() != error) {
        m_summary->setProperty("error", error);
        m_summary->style()->unpolish(m_summary);
        m_summary->style()->polish(m_summary);
    }

    const QSignalBlocker blocker(m_files);
    m_diff = WorkspaceDiff();
    m_modes->hide();
    m_files->clear();
    m_files->hide();
    m_text->clear();
    m_text->hide();
    m_oldText->clear();
    m_newText->clear();
    m_split->hide();
    m_fileBlocks.clear();
}

void DiffView::setRefreshVisible(bool visible) {
    m_refresh->setVisible(visible);
}

void DiffView::setSideBySide(bool sideBySide) {
    {
        const QSignalBlocker unified(m_unifiedButton);
        const QSignalBlocker split(m_splitButton);
        (sideBySide ? m_splitButton : m_unifiedButton)->setChecked(true);
    }
    if (m_sideBySide == sideBySide) {
        return;
    }
    // Setelah berganti tampilan, file yang sedang dibaca tetap di atas
    const int file = visibleFile();
    m_sideBySide = sideBySide;
    if (!m_diff.files.isEmpty()) {
        render();
        scrollToFile(file);
    }
    emit sideBySideChanged(sideBySide);
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

void DiffView::render() {
    m_fileBlocks.clear();
    m_text->clear();
    m_oldText->clear();
    m_newText->clear();
    if (m_sideBySide) {
        renderSideBySide(m_diff);
    } else {
        renderUnified(m_diff);
    }
    m_text->setVisible(!m_sideBySide);
    m_split->setVisible(m_sideBySide);
}

void DiffView::renderUnified(const WorkspaceDiff &diff) {
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
                cursor.insertText(marker + expandTabs(line.text), plain);
                break;
            }
            }
        }
    }
    cursor.endEditBlock();

    m_text->verticalScrollBar()->setValue(0);
    m_text->horizontalScrollBar()->setValue(0);
}

void DiffView::renderSideBySide(const WorkspaceDiff &diff) {
    const QTextCharFormat plain;
    QTextCharFormat header;
    header.setFontWeight(QFont::Bold);
    const QTextCharFormat gutter = textWith(kGutterText);
    const QTextCharFormat hunk = textWith(kNavyText);
    QTextCharFormat note = textWith(kNoteText);
    note.setFontItalic(true);
    QTextCharFormat removedWord;
    removedWord.setBackground(QColor(kRemovedWordBackground));
    QTextCharFormat addedWord;
    addedWord.setBackground(QColor(kAddedWordBackground));

    const QTextBlockFormat plainBlock;
    const QTextBlockFormat headerBlock = blockWith(kHeaderBackground);
    const QTextBlockFormat hunkBlock = blockWith(kHunkBackground);
    const QTextBlockFormat addedBlock = blockWith(kAddedBackground);
    const QTextBlockFormat removedBlock = blockWith(kRemovedBackground);
    const QTextBlockFormat fillerBlock = blockWith(kFillerBackground);

    // Setiap baris tampilan ditulis di kedua sisi sekaligus, jadi nomor bloknya selalu sama
    QTextCursor left(m_oldText->document());
    QTextCursor right(m_newText->document());
    left.beginEditBlock();
    right.beginEditBlock();
    bool firstRow = true;
    auto startRow = [&](const QTextBlockFormat &leftFormat, const QTextBlockFormat &rightFormat) {
        if (firstRow) {
            left.setBlockFormat(leftFormat);
            right.setBlockFormat(rightFormat);
            firstRow = false;
        } else {
            left.insertBlock(leftFormat, plain);
            right.insertBlock(rightFormat, plain);
        }
    };

    int budget = kMaxRenderedLines;
    for (const FileDiff &file : diff.files) {
        if (!firstRow) {
            startRow(plainBlock, plainBlock);   // jarak antar file
        }
        startRow(headerBlock, headerBlock);
        m_fileBlocks.append(left.blockNumber());
        const QString title = QStringLiteral(" %1  %2").arg(statusLetter(file), displayPath(file));
        left.insertText(title, header);
        right.insertText(title, header);

        // Tiap sisi punya satu kolom nomor baris, selebar angka terbesar di sisi itu
        int oldWidth = 1;
        int newWidth = 1;
        for (const DiffLine &line : file.lines) {
            oldWidth = qMax(oldWidth, int(QString::number(line.oldLine).size()));
            newWidth = qMax(newWidth, int(QString::number(line.newLine).size()));
        }
        const QString oldBlank(oldWidth + 1, QLatin1Char(' '));
        const QString newBlank(newWidth + 1, QLatin1Char(' '));
        auto addNote = [&](const QString &text) {
            startRow(plainBlock, plainBlock);
            left.insertText(oldBlank + QStringLiteral("  ") + text, note);
            right.insertText(newBlank + QStringLiteral("  ") + text, note);
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

        // Satu sel: nomor baris, penanda, lalu isi; range = bagian yang berubah (length 0 = tidak ada)
        auto writeCell = [&](QTextCursor &cursor, const DiffLine &line, bool oldSide, int start, int length) {
            if (line.kind == DiffLine::Kind::Note) {
                cursor.insertText((oldSide ? oldBlank : newBlank) + QStringLiteral("  ") + line.text, note);
                return;
            }
            cursor.insertText(QStringLiteral("%1 ").arg(lineNumber(oldSide ? line.oldLine : line.newLine),
                                                        oldSide ? oldWidth : newWidth),
                              gutter);
            const QString marker = line.kind == DiffLine::Kind::Added     ? QStringLiteral("+ ")
                                   : line.kind == DiffLine::Kind::Removed ? QStringLiteral("- ")
                                                                          : QStringLiteral("  ");
            const QString text = expandTabs(line.text);
            if (length <= 0) {
                cursor.insertText(marker + text, plain);
                return;
            }
            cursor.insertText(marker + text.left(start), plain);
            cursor.insertText(text.mid(start, length), oldSide ? removedWord : addedWord);
            cursor.insertText(text.mid(start + length), plain);
        };

        // Blok perubahan yang belum ditulis: baris hapus (kiri) dan tambah (kanan) dipasangkan per
        // baris; sisi yang lebih pendek diganjal baris kosong. Catatan "\ No newline" ikut sisinya.
        QList<const DiffLine *> removed;
        QList<const DiffLine *> added;
        auto flush = [&]() {
            const qsizetype rows = qMax(removed.size(), added.size());
            for (qsizetype row = 0; row < rows; ++row) {
                const DiffLine *before = row < removed.size() ? removed.at(row) : nullptr;
                const DiffLine *after = row < added.size() ? added.at(row) : nullptr;
                const bool beforeNote = before && before->kind == DiffLine::Kind::Note;
                const bool afterNote = after && after->kind == DiffLine::Kind::Note;
                startRow(!before ? fillerBlock : beforeNote ? plainBlock : removedBlock,
                         !after ? fillerBlock : afterNote ? plainBlock : addedBlock);
                ChangedRange range;
                if (before && after && !beforeNote && !afterNote) {
                    range = changedRange(expandTabs(before->text), expandTabs(after->text));
                }
                if (before) {
                    writeCell(left, *before, true, range.start, range.oldLength);
                }
                if (after) {
                    writeCell(right, *after, false, range.start, range.newLength);
                }
            }
            removed.clear();
            added.clear();
        };

        DiffLine::Kind previous = DiffLine::Kind::Hunk;
        for (qsizetype i = 0; i < file.lines.size(); ++i) {
            if (budget-- == 0) {
                flush();
                addNote(QStringLiteral("… %1 baris lagi tidak ditampilkan (diff melebihi %2 baris)")
                            .arg(file.lines.size() - i)
                            .arg(kMaxRenderedLines));
                break;
            }
            const DiffLine &line = file.lines.at(i);
            switch (line.kind) {
            case DiffLine::Kind::Hunk:
                flush();
                startRow(hunkBlock, hunkBlock);
                left.insertText(oldBlank + line.text, hunk);
                right.insertText(newBlank + line.text, hunk);
                break;
            case DiffLine::Kind::Removed:
                if (!added.isEmpty()) {
                    flush();   // hapus sesudah tambah = blok perubahan baru
                }
                removed.append(&line);
                break;
            case DiffLine::Kind::Added:
                added.append(&line);
                break;
            case DiffLine::Kind::Note:
                if (previous == DiffLine::Kind::Removed && added.isEmpty()) {
                    removed.append(&line);
                } else if (previous == DiffLine::Kind::Added) {
                    added.append(&line);
                } else {
                    flush();
                    addNote(line.text);
                }
                break;
            case DiffLine::Kind::Context:
                flush();
                startRow(plainBlock, plainBlock);
                writeCell(left, line, true, 0, 0);
                writeCell(right, line, false, 0, 0);
                break;
            }
            previous = line.kind;
        }
        flush();
    }
    left.endEditBlock();
    right.endEditBlock();

    m_oldText->verticalScrollBar()->setValue(0);
    m_oldText->horizontalScrollBar()->setValue(0);
}

void DiffView::scrollToFile(int index) {
    if (index >= 0 && index < m_fileBlocks.size()) {
        // NoWrap: satu blok = satu baris, jadi nilai scrollbar = nomor blok teratas. Dua kolom:
        // scrollbar kanan ikut lewat linkScrollBars
        (m_sideBySide ? m_oldText : m_text)->verticalScrollBar()->setValue(m_fileBlocks.at(index));
    }
}

int DiffView::visibleFile() const {
    const int top = (m_sideBySide ? m_oldText : m_text)->verticalScrollBar()->value();
    int file = 0;
    for (int i = 0; i < m_fileBlocks.size() && m_fileBlocks.at(i) <= top; ++i) {
        file = i;
    }
    return file;
}

void DiffView::linkScrollBars(QScrollBar *first, QScrollBar *second) {
    auto follow = [this](QScrollBar *target) {
        return [this, target](int value) {
            if (m_syncingScroll) {
                return;
            }
            m_syncingScroll = true;
            target->setValue(value);
            m_syncingScroll = false;
        };
    };
    connect(first, &QScrollBar::valueChanged, this, follow(second));
    connect(second, &QScrollBar::valueChanged, this, follow(first));
}

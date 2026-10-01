#include "MaintainabilityView.h"
#include "Theme.h"
#include "WorkspaceDiff.h"

#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QSet>
#include <QStyle>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

enum Column {
    NameColumn, BeforeColumn, AfterColumn, ChangeColumn,
    ComplexityColumn, InheritanceColumn, CouplingColumn, LinesColumn, ColumnCount
};

// Warna mengikuti palet styles.qss
constexpr QRgb kGoodText = 0x2f7d32;
constexpr QRgb kModerateText = 0xb7791f;
constexpr QRgb kLowText = 0xb3261e;
constexpr QRgb kMutedText = 0x8a7f70;
constexpr QRgb kTypeText = 0x33517a;

QString ratingKey(int index) {
    switch (maintainabilityRating(index)) {
    case MaintainabilityRating::Moderate: return QStringLiteral("moderate");
    case MaintainabilityRating::Low: return QStringLiteral("low");
    case MaintainabilityRating::Good: break;
    }
    return QStringLiteral("good");
}

QString ratingLabel(int index) {
    switch (maintainabilityRating(index)) {
    case MaintainabilityRating::Moderate: return QStringLiteral("Sedang");
    case MaintainabilityRating::Low: return QStringLiteral("Rendah");
    case MaintainabilityRating::Good: break;
    }
    return QStringLiteral("Baik");
}

QRgb ratingColor(int index) {
    switch (maintainabilityRating(index)) {
    case MaintainabilityRating::Moderate: return kModerateText;
    case MaintainabilityRating::Low: return kLowText;
    case MaintainabilityRating::Good: break;
    }
    return kGoodText;
}

// File tanpa baris kode (mis. hanya komentar) tidak punya MI yang bermakna
std::optional<int> indexOf(const std::optional<CodeMetrics> &metrics) {
    if (metrics && metrics->sloc > 0) {
        return metrics->maintainability;
    }
    return std::nullopt;
}

QString describe(const QString &side, const std::optional<CodeMetrics> &metrics) {
    if (!metrics) {
        return QStringLiteral("%1: —").arg(side);
    }
    return QStringLiteral("%1: MI %2 · %3 baris kode · %4 fungsi · kompleksitas %5 · volume %6")
        .arg(side)
        .arg(metrics->sloc > 0 ? QString::number(metrics->maintainability) : QStringLiteral("—"))
        .arg(metrics->sloc)
        .arg(metrics->functions)
        .arg(metrics->complexity)
        .arg(qRound(metrics->volume));
}

// Kolom Sebelum/Sesudah/Δ; missing = teks Δ bila salah satu sisi tidak ada ("baru", "dihapus")
void fillIndex(QTreeWidgetItem *item, std::optional<int> before, std::optional<int> after, const QString &missing) {
    item->setText(BeforeColumn, before ? QString::number(*before) : QStringLiteral("—"));
    item->setText(AfterColumn, after ? QString::number(*after) : QStringLiteral("—"));
    if (after) {
        item->setForeground(AfterColumn, Theme::text(ratingColor(*after)));
    }
    QRgb changeColor = kMutedText;
    if (before && after) {
        const int delta = *after - *before;
        item->setText(ChangeColumn, delta > 0 ? QStringLiteral("+%1").arg(delta)
                                              : delta < 0 ? QStringLiteral("−%1").arg(-delta) : QStringLiteral("0"));
        changeColor = delta < 0 ? kLowText : delta > 0 ? kGoodText : kMutedText;
    } else {
        item->setText(ChangeColumn, missing);
    }
    item->setForeground(ChangeColumn, Theme::text(changeColor));
}

void alignNumbers(QTreeWidgetItem *item, const QString &tooltip) {
    for (int column = NameColumn; column < ColumnCount; ++column) {
        item->setToolTip(column, tooltip);
        if (column != NameColumn) {
            item->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
        }
    }
}

QString baseName(const QString &member) {
    return member.section(QLatin1Char('('), 0, 0);
}

// Tipe dan member C# di bawah baris file. MI sebelumnya dicocokkan lewat nama tipe dan tanda
// tangan member; bila tanda tangannya berubah (mis. parameter ditambah), lewat nama saja asalkan
// kandidatnya tunggal.
void addTypeRows(QTreeWidgetItem *fileItem, const CodeMetrics &after, const std::optional<CodeMetrics> &before) {
    QHash<QString, const TypeMetrics *> previousTypes;
    QHash<QString, const MemberMetrics *> previousMembers;
    QMultiHash<QString, const MemberMetrics *> previousByName;
    if (before) {
        for (const TypeMetrics &type : before->types) {
            previousTypes.insert(type.fullName(), &type);
            for (const MemberMetrics &member : type.members) {
                const QString key = type.fullName() + QStringLiteral("::");
                previousMembers.insert(key + member.name, &member);
                previousByName.insert(key + baseName(member.name), &member);
            }
        }
    }

    for (const TypeMetrics &type : after.types) {
        auto *typeItem = new QTreeWidgetItem(fileItem);
        typeItem->setText(NameColumn, type.name);
        typeItem->setForeground(NameColumn, Theme::text(kTypeText));
        const TypeMetrics *old = previousTypes.value(type.fullName());
        fillIndex(typeItem, old ? std::optional<int>(old->maintainability) : std::nullopt, type.maintainability,
                  QStringLiteral("baru"));
        typeItem->setText(ComplexityColumn, QString::number(type.complexity));
        typeItem->setText(InheritanceColumn, QStringLiteral("%1%2").arg(type.inheritanceOpen ? QStringLiteral("≥")
                                                                                              : QString())
                                                  .arg(type.inheritanceDepth));
        typeItem->setText(CouplingColumn, QString::number(type.coupling));
        typeItem->setText(LinesColumn, QString::number(type.lines));
        QString inheritance = type.baseClass.isEmpty() ? QStringLiteral("langsung dari System.Object")
                                                       : QStringLiteral("kelas dasar %1").arg(type.baseClass);
        if (type.inheritanceOpen) {
            inheritance += QStringLiteral(" (dari luar project, kedalaman sebenarnya bisa lebih)");
        }
        alignNumbers(typeItem, QStringLiteral("%1 %2\nDIT %3%4: %5\n%6 member berkode · coupling %7 · %8 baris kode")
                                   .arg(type.kind, type.fullName(), type.inheritanceOpen ? QStringLiteral("≥") : QString())
                                   .arg(type.inheritanceDepth)
                                   .arg(inheritance)
                                   .arg(type.members.size())
                                   .arg(type.coupling)
                                   .arg(type.lines));

        const QString key = type.fullName() + QStringLiteral("::");
        QList<const MemberMetrics *> previous;
        QSet<const MemberMetrics *> matched;
        for (const MemberMetrics &member : type.members) {
            previous.append(previousMembers.value(key + member.name));
            if (previous.last()) {
                matched.insert(previous.last());
            }
        }
        for (qsizetype i = 0; i < type.members.size(); ++i) {
            if (previous.at(i)) {
                continue;
            }
            const MemberMetrics *candidate = nullptr;
            int candidates = 0;
            const QList<const MemberMetrics *> sameName = previousByName.values(key + baseName(type.members.at(i).name));
            for (const MemberMetrics *old : sameName) {
                if (!matched.contains(old)) {
                    candidate = old;
                    ++candidates;
                }
            }
            if (candidates == 1) {
                previous[i] = candidate;
                matched.insert(candidate);
            }
        }

        for (qsizetype i = 0; i < type.members.size(); ++i) {
            const MemberMetrics &member = type.members.at(i);
            const MemberMetrics *old = previous.at(i);
            auto *memberItem = new QTreeWidgetItem(typeItem);
            memberItem->setText(NameColumn, member.name);
            fillIndex(memberItem, old ? std::optional<int>(old->maintainability) : std::nullopt,
                      member.maintainability, QStringLiteral("baru"));
            memberItem->setText(ComplexityColumn, QString::number(member.complexity));
            memberItem->setText(CouplingColumn, QString::number(member.coupling));
            memberItem->setText(LinesColumn, QString::number(member.lines));
            QString tooltip = QStringLiteral("%1.%2\nMI %3 · kompleksitas %4 · coupling %5 · %6 baris kode · volume %7")
                                  .arg(type.name, member.name)
                                  .arg(member.maintainability)
                                  .arg(member.complexity)
                                  .arg(member.coupling)
                                  .arg(member.lines)
                                  .arg(qRound(member.volume));
            if (old && old->name != member.name) {
                tooltip += QStringLiteral("\nSebelumnya: %1").arg(old->name);
            }
            alignNumbers(memberItem, tooltip);
        }
    }
}

}

MaintainabilityView::MaintainabilityView(QWidget *parent)
    : QWidget(parent) {
    setObjectName("maintainabilityView");

    m_message = new QLabel(this);
    m_message->setObjectName("miMessage");
    m_message->setWordWrap(true);
    m_message->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_score = new QLabel(this);
    m_score->setObjectName("miScore");
    m_rating = new QLabel(this);
    m_rating->setObjectName("miRating");
    m_change = new QLabel(this);
    m_change->setObjectName("miChange");
    m_change->setWordWrap(true);
    m_basis = new QLabel(this);
    m_basis->setObjectName("miBasis");
    m_basis->setWordWrap(true);

    m_summary = new QWidget(this);
    m_summary->setObjectName("miSummary");
    m_summary->setAttribute(Qt::WA_StyledBackground, true);
    auto *caption = new QLabel(QStringLiteral("MAINTAINABILITY INDEX"), m_summary);
    caption->setObjectName("drawerSectionLabel");
    auto *details = new QVBoxLayout();
    details->setSpacing(2);
    details->addWidget(caption);
    details->addWidget(m_rating);
    details->addWidget(m_change);
    details->addWidget(m_basis);
    auto *summaryLayout = new QHBoxLayout(m_summary);
    summaryLayout->setContentsMargins(12, 10, 12, 10);
    summaryLayout->setSpacing(14);
    summaryLayout->addWidget(m_score, 0, Qt::AlignVCenter);
    summaryLayout->addLayout(details, 1);

    m_files = new QTreeWidget(this);
    m_files->setObjectName("miFileList");
    m_files->setColumnCount(ColumnCount);
    m_files->setHeaderLabels({QStringLiteral("Nama"), QStringLiteral("Sebelum"), QStringLiteral("Sesudah"),
                              QStringLiteral("Δ"), QStringLiteral("CC"), QStringLiteral("DIT"),
                              QStringLiteral("Coupling"), QStringLiteral("Baris")});
    const QStringList headerTips = {
        QStringLiteral("File yang berubah; untuk C# berisi tipe dan member-nya"),
        QStringLiteral("Maintainability Index sebelum perubahan (0–100)"),
        QStringLiteral("Maintainability Index sesudah perubahan (0–100)"),
        QStringLiteral("Selisih Maintainability Index"),
        QStringLiteral("Cyclomatic complexity sesudah perubahan"),
        QStringLiteral("Depth of inheritance (C#): jumlah kelas leluhur sampai System.Object"),
        QStringLiteral("Class coupling (C#): jumlah tipe berbeda yang dipakai"),
        QStringLiteral("Baris kode sesudah perubahan, tanpa baris kosong dan komentar"),
    };
    for (int column = NameColumn; column < ColumnCount; ++column) {
        m_files->headerItem()->setToolTip(column, headerTips.at(column));
        if (column != NameColumn) {
            m_files->headerItem()->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
        }
    }
    m_files->setRootIsDecorated(false);
    m_files->setIndentation(14);
    m_files->setUniformRowHeights(true);
    m_files->setTextElideMode(Qt::ElideMiddle);
    m_files->setSelectionMode(QAbstractItemView::NoSelection);
    m_files->setFocusPolicy(Qt::NoFocus);
    m_files->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QHeaderView *columns = m_files->header();
    columns->setStretchLastSection(false);
    columns->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
    for (int column = BeforeColumn; column < ColumnCount; ++column) {
        columns->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    }

    m_legend = new QLabel(this);
    m_legend->setObjectName("miLegend");
    m_legend->setWordWrap(true);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(8);
    root->addWidget(m_message);
    root->addWidget(m_summary);
    root->addWidget(m_files, 1);
    root->addWidget(m_legend);
    // Saat hanya pesan yang tampil (memuat / gagal / kosong), pesan tetap di atas
    root->addStretch();

    showMessage(QString(), false);
}

void MaintainabilityView::showLoading() {
    showMessage(QStringLiteral("Menghitung maintainability file yang berubah…"), false);
}

void MaintainabilityView::showDiff(const WorkspaceDiff &diff) {
    if (!diff.error.isEmpty()) {
        showMessage(diff.error, true);
        return;
    }

    int measured = 0;
    for (const FileDiff &file : diff.files) {
        if (file.before || file.after) {
            ++measured;
        }
    }
    const int skipped = int(diff.files.size()) - measured;
    const std::optional<int> before = diff.maintainabilityBefore();
    const std::optional<int> after = diff.maintainabilityAfter();
    if (!before && !after) {
        showMessage(diff.files.isEmpty()
                        ? QStringLiteral("Tidak ada perubahan yang belum di-commit di folder kerja.")
                        : QStringLiteral("Tidak ada file kode sumber yang berubah, jadi tidak ada yang diukur."),
                    false);
        return;
    }

    m_message->hide();
    m_summary->show();
    m_files->show();
    m_legend->show();

    m_score->setText(after ? QString::number(*after) : QStringLiteral("—"));
    setStyleProperty(m_score, "rating", after ? ratingKey(*after) : QStringLiteral("none"));
    m_rating->setText(after ? ratingLabel(*after) : QStringLiteral("Tidak ada kode tersisa"));
    setStyleProperty(m_rating, "rating", after ? ratingKey(*after) : QStringLiteral("none"));

    QString change;
    QString trend = QStringLiteral("flat");
    if (before && after) {
        const int delta = *after - *before;
        if (delta < 0) {
            change = QStringLiteral("Turun %1 dari %2 sebelum perubahan").arg(-delta).arg(*before);
            trend = QStringLiteral("down");
        } else if (delta > 0) {
            change = QStringLiteral("Naik %1 dari %2 sebelum perubahan").arg(delta).arg(*before);
            trend = QStringLiteral("up");
        } else {
            change = QStringLiteral("Sama dengan sebelum perubahan");
        }
    } else if (after) {
        change = QStringLiteral("Semua file yang diukur adalah file baru");
    } else {
        change = QStringLiteral("Semua file yang diukur dihapus (sebelumnya %1)").arg(*before);
    }
    m_change->setText(change);
    setStyleProperty(m_change, "trend", trend);
    m_basis->setText(QStringLiteral("Rata-rata %1 file kode, tertimbang baris kode").arg(measured));

    m_files->clear();
    bool csharp = false;
    QList<QTreeWidgetItem *> items;
    for (const FileDiff &file : diff.files) {
        if (!file.before && !file.after) {
            continue;
        }
        auto *item = new QTreeWidgetItem();
        item->setText(NameColumn, file.status == FileDiff::Status::Renamed
                                      ? QStringLiteral("%1 → %2").arg(file.oldPath, file.path)
                                      : file.path);
        QString missing = QStringLiteral("—");
        if (file.status == FileDiff::Status::Added) {
            missing = QStringLiteral("baru");
        } else if (file.status == FileDiff::Status::Deleted) {
            missing = QStringLiteral("dihapus");
        }
        fillIndex(item, indexOf(file.before), indexOf(file.after), missing);
        const CodeMetrics &current = file.after ? *file.after : *file.before;
        item->setText(ComplexityColumn, QString::number(current.complexity));
        item->setText(LinesColumn, QString::number(current.sloc));
        alignNumbers(item, QStringLiteral("%1\n%2\n%3")
                               .arg(file.path, describe(QStringLiteral("Sebelum"), file.before),
                                    describe(QStringLiteral("Sesudah"), file.after)));
        if (file.after && !file.after->types.isEmpty()) {
            csharp = true;
            addTypeRows(item, *file.after, file.before);
        }
        items.append(item);
    }
    m_files->addTopLevelItems(items);
    m_files->setRootIsDecorated(csharp);
    m_files->setColumnHidden(InheritanceColumn, !csharp);
    m_files->setColumnHidden(CouplingColumn, !csharp);
    if (csharp) {
        m_files->expandAll();
    }

    QString legend = QStringLiteral(
        "Skala 0–100 seperti Visual Studio: ≥ 20 baik · 10–19 sedang · < 10 rendah. Tiap file dihitung dari "
        "rata-rata per fungsi: 171 − 5,2·ln(volume Halstead) − 0,23·kompleksitas siklomatik − 16,2·ln(baris kode). "
        "Nilainya perkiraan dari token, jadi paling berguna untuk membandingkan sebelum dan sesudah.");
    if (csharp) {
        legend += QStringLiteral(
            "\nC#: rincian per tipe dan member seperti Code Metrics Visual Studio. DIT = kedalaman pewarisan "
            "(≥ bila kelas dasarnya dari luar project dan tidak dikenal); Coupling = jumlah tipe berbeda yang "
            "dipakai. Keduanya diperkirakan dari nama tanpa kompilasi.");
    }
    if (skipped > 0) {
        legend += QStringLiteral("\n%1 file lain tidak diukur (bukan kode sumber yang didukung, biner, atau terlalu besar).")
                      .arg(skipped);
    }
    m_legend->setText(legend);
}

void MaintainabilityView::showMessage(const QString &text, bool error) {
    m_message->setText(text);
    m_message->setVisible(!text.isEmpty());
    setStyleProperty(m_message, "error", error ? QStringLiteral("true") : QStringLiteral("false"));
    m_summary->hide();
    m_files->clear();
    m_files->hide();
    m_legend->hide();
}

void MaintainabilityView::setStyleProperty(QWidget *widget, const char *name, const QString &value) {
    if (widget->property(name).toString() == value) {
        return;
    }
    widget->setProperty(name, value);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

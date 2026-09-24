#ifndef MAINTAINABILITYVIEW_H
#define MAINTAINABILITYVIEW_H

#pragma once

#include <QWidget>

struct WorkspaceDiff;
class QLabel;
class QTreeWidget;

// Maintainability Index file kode yang berubah: skor rata-rata sesudah perubahan beserta arah
// perubahannya dari sebelum, lalu tabel per file (sebelum, sesudah, selisih).
class MaintainabilityView : public QWidget {
    Q_OBJECT

public:
    explicit MaintainabilityView(QWidget *parent = nullptr);

    void showLoading();
    void showDiff(const WorkspaceDiff &diff);

private:
    void showMessage(const QString &text, bool error);
    static void setStyleProperty(QWidget *widget, const char *name, const QString &value);

    QLabel *m_message;
    QWidget *m_summary;
    QLabel *m_score;
    QLabel *m_rating;
    QLabel *m_change;   // arah perubahan, berwarna
    QLabel *m_basis;    // dasar rata-rata, netral
    QTreeWidget *m_files;
    QLabel *m_legend;
};

#endif // MAINTAINABILITYVIEW_H

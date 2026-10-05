#pragma once

#include "CanvasBoard.h"
#include "TaskItem.h"

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QWidget>

class QLabel;
class QLineEdit;
class QMimeData;
class QTreeWidget;
class QTreeWidgetItem;

// Pustaka artefak kanvas: task dari semua project yang terbuka beserta dokumen hasil tiap stage dan
// lampirannya. Item diseret ke kanvas (atau diklik dua kali) menjadi kartu referensi.
class CanvasLibrary : public QWidget {
    Q_OBJECT

public:
    // Isi drag: array JSON CanvasSource
    static const QString kMimeType;
    static QMimeData *mimeData(const QList<CanvasSource> &sources);
    static QList<CanvasSource> sources(const QMimeData *mime);

    explicit CanvasLibrary(QWidget *parent = nullptr);

    // Bangun ulang daftar; project currentProject paling atas. Item yang dibuka/ditutup pengguna
    // tetap seperti itu.
    void setTasks(const QList<TaskItem> &tasks, const QString &currentProject, const QStringList &stageOrder);

signals:
    void sourceActivated(const CanvasSource &source);

private:
    void applyFilter();
    void rememberExpansion(QTreeWidgetItem *item, bool expanded);

    QLineEdit *m_filter;
    QTreeWidget *m_tree;
    QLabel *m_empty;
    QSet<QString> m_expanded;    // key item yang dibuka pengguna
    QSet<QString> m_collapsed;   // key item yang ditutup pengguna
    bool m_rebuilding = false;
};

#ifndef SWIMLANEWIDGET_H
#define SWIMLANEWIDGET_H

#pragma once

#include <QMap>
#include <QString>
#include <QWidget>

namespace Ui {
class SwimlaneWidget;
}

class KanbanColumnWidget;
class KanbanCardWidget;
class StageCatalog;

class SwimlaneWidget : public QWidget
{
    Q_OBJECT

public:
    // Kolom dibangun dari urutan stage di katalog
    SwimlaneWidget(const QString &projectId, const StageCatalog &catalog, QWidget *parent = nullptr);
    ~SwimlaneWidget() override;

    QString projectId() const { return m_projectId; }
    void setProjectTitle(const QString &title);

    // Tampilkan folder kerja project di tombol header (kosong = belum dipilih)
    void setWorkingDirectory(const QString &path);

    // Manajemen kartu dalam swimlane
    void addCardToStage(const QString &stageName, KanbanCardWidget *card);
    KanbanColumnWidget *column(const QString &stageName) const;
    QList<KanbanColumnWidget *> columns() const { return m_columns.values(); }

    // nullptr bila tidak ada kartu dengan id itu
    KanbanCardWidget *cardById(const QString &taskId) const;

    // Key stage kolom tempat kartu berada; kosong bila kartu tidak ada di swimlane ini
    QString stageOf(const KanbanCardWidget *card) const;

signals:
    void closeProjectRequested(const QString &projectId);
    // Tombol folder kerja di header diklik
    void workingDirectoryChangeRequested(const QString &projectId);
    void cardMoved(const QString &projectId,
                   KanbanCardWidget *card,
                   const QString &targetStage,
                   int targetIndex);

private slots:
    void onCloseClicked();
    void handleCardDropped(KanbanCardWidget *card, const QString &targetStage, int targetIndex);

private:
    Ui::SwimlaneWidget *ui;
    QString m_projectId;

    // Mapping nama stage ke pointer kolom
    QMap<QString, KanbanColumnWidget *> m_columns;

    void initializeColumns(const StageCatalog &catalog);
};
#endif // SWIMLANEWIDGET_H

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
class QPushButton;
class QResizeEvent;

class SwimlaneWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SwimlaneWidget(const QString &projectId, QWidget *parent = nullptr);
    ~SwimlaneWidget() override;

    QString projectId() const { return m_projectId; }
    void setProjectTitle(const QString &title);

    // Manajemen kartu dalam swimlane
    void addCardToStage(const QString &stageName, KanbanCardWidget *card);
    KanbanColumnWidget *column(const QString &stageName) const;
    QList<KanbanColumnWidget *> columns() const { return m_columns.values(); }

signals:
    void newTaskRequested(const QString &projectId);
    void closeProjectRequested(const QString &projectId);
    void cardMoved(const QString &projectId,
                   KanbanCardWidget *card,
                   const QString &targetStage,
                   int targetIndex);

protected:
    // Jaga tombol New Task tetap menempel di pojok kanan bawah saat ukuran berubah
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onNewTaskClicked();
    void onCloseClicked();
    void handleCardDropped(KanbanCardWidget *card, const QString &targetStage, int targetIndex);

private:
    Ui::SwimlaneWidget *ui;
    QString m_projectId;

    // Tombol New Task mengambang: anak langsung SwimlaneWidget, di luar layout mana pun,
    // sehingga tidak ikut tergeser saat kolom di-scroll horizontal.
    QPushButton *m_btnNewTask;

    // Mapping nama stage ke pointer kolom
    QMap<QString, KanbanColumnWidget *> m_columns;

    void initializeColumns();
    void repositionNewTaskButton();
};
#endif // SWIMLANEWIDGET_H

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

signals:
    void newTaskRequested(const QString &projectId);
    void closeProjectRequested(const QString &projectId);
    void cardMoved(const QString &projectId,
                   KanbanCardWidget *card,
                   const QString &targetStage,
                   int targetIndex);

private slots:
    void onNewTaskClicked();
    void onCloseClicked();
    void handleCardDropped(KanbanCardWidget *card, const QString &targetStage, int targetIndex);

private:
    Ui::SwimlaneWidget *ui;
    QString m_projectId;

    // Mapping nama stage ke pointer kolom
    QMap<QString, KanbanColumnWidget *> m_columns;

    void initializeColumns();
};
#endif // SWIMLANEWIDGET_H

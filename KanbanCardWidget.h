#ifndef KANBANCARDWIDGET_H
#define KANBANCARDWIDGET_H


#pragma once

#include <QWidget>
#include <QPoint>

namespace Ui {
class KanbanCardWidget;
}


class KanbanCardWidget : public QWidget{
    Q_OBJECT;
public:
    explicit KanbanCardWidget(QWidget *parent = nullptr);
    ~KanbanCardWidget() override;

    void setCardData(const QString &id,
                     const QString &category,
                     const QString &title,
                     const QString &subtext,
                     const QString &badge);

    QString id() const { return m_id; }
    QString category() const { return m_category; }
    QString title() const { return m_title; }
    QString subtext() const { return m_subtext; }
    QString badge() const { return m_badge; }

signals:
    void cardClicked(const QString &cardId);
    // Tombol play di pojok kanan atas kartu diklik
    void runRequested(const QString &cardId);

protected:
    //Event penanganan drag and drop
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private :
    Ui::KanbanCardWidget *ui;

    // Titik awal klik untuk mendeteksi ambang drag (drag threshold)
    QPoint m_dragStartPosition;

    //Data internal kartu
    QString m_id;
    QString m_category;
    QString m_title;
    QString m_subtext;
    QString m_badge;

    void updateUI();
};

#endif // KANBANCARDWIDGET_H

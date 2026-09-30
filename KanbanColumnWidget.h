#ifndef KANBANCOLUMNWIDGET_H
#define KANBANCOLUMNWIDGET_H

#pragma once

#include <QWidget>
#include <QString>
#include <QList>

namespace Ui {
class KanbanColumnWidget;
class KanbanCardWidget;
}

class HoverInfoPopup;
class KanbanCardWidget;
class QVBoxLayout;
class QDragEnterEvent;
class QDragMoveEvent;
class QDragLeaveEvent;
class QDropEvent;

class KanbanColumnWidget : public QWidget {
    Q_OBJECT

public:
    explicit KanbanColumnWidget(QWidget *parent = nullptr);
    ~KanbanColumnWidget() override;

    void setStageName(const QString &name);
    QString stageName() const { return m_stageName; }

    // Penjelasan stage (teks kaya) yang muncul saat ikon info di sebelah judul, atau judulnya
    // sendiri, di-hover; kosong = ikon disembunyikan
    void setStageInfo(const QString &info);
    QString stageInfo() const;

    // Stage kolom ini punya agent? Diteruskan ke tombol run semua kartu di dalamnya
    void setRunnable(bool runnable);
    bool isRunnable() const { return m_runnable; }

    void addCard(KanbanCardWidget *card);
    void insertCard(int index, KanbanCardWidget *card);
    void removeCard(KanbanCardWidget *card);
    int cardCount() const;
    QList<KanbanCardWidget *> cards() const;   // urutan atas -> bawah

signals:
    // Dipancarkan saat kartu di-drop ke kolom ini
    void cardDropped(KanbanCardWidget *card, const QString &targetStage, int targetIndex);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    Ui::KanbanColumnWidget *ui;
    QString m_stageName;
    QVBoxLayout *m_cardListLayout;
    HoverInfoPopup *m_infoPopup;
    bool m_runnable = false;

    // Menghitung indeks baris kartu berdasarkan posisi vertikal mouse
    int calculateInsertIndex(int dropY, const KanbanCardWidget *exclude) const;
};

#endif // KANBANCOLUMNWIDGET_H

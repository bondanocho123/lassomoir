#ifndef SWIMLANEWIDGET_H
#define SWIMLANEWIDGET_H

#pragma once

#include <QMap>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QWidget>

namespace Ui {
class SwimlaneWidget;
}

class KanbanColumnWidget;
class KanbanCardWidget;
class QFrame;
class StageCatalog;
struct GitHead;

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

    // Branch aktif folder kerja di tombol branch header; tombolnya hanya tampil bila folder kerja
    // berada di repository git. Klik = jendela riwayat branch & commit.
    void setGitHead(const GitHead &head);

    // Folder referensi project (hanya dibaca agent): jumlahnya di tombol "Referensi", daftarnya
    // di popup tombol itu. Popup yang sedang terbuka ikut diperbarui.
    void setReferenceDirectories(const QStringList &dirs);
    QStringList referenceDirectories() const { return m_referenceDirs; }

    // Popup daftar folder referensi di bawah tombol "Referensi": hapus per folder, tambah folder
    void showReferencePopup();

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
    // Tombol "Kanvas" di header diklik
    void canvasRequested(const QString &projectId);
    // Tombol folder kerja di header diklik
    void workingDirectoryChangeRequested(const QString &projectId);
    // Tombol branch di header diklik
    void branchViewRequested(const QString &projectId);
    // Dari popup folder referensi: "Tambah folder…" dan tombol × per folder
    void referenceDirectoryAddRequested(const QString &projectId);
    void referenceDirectoryRemoveRequested(const QString &projectId, const QString &dir);
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
    QStringList m_referenceDirs;
    QPointer<QFrame> m_referencePopup;

    // Mapping nama stage ke pointer kolom
    QMap<QString, KanbanColumnWidget *> m_columns;

    void initializeColumns(const StageCatalog &catalog);
    // Isi popup folder referensi (judul, baris per folder, tombol tambah) dari m_referenceDirs
    void fillReferencePopup(QFrame *popup);
    // Ukur ulang popup, lalu gantung di bawah tombol "Referensi" dan jaga tetap di dalam layar
    void placeReferencePopup(QFrame *popup);
};
#endif // SWIMLANEWIDGET_H

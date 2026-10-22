#ifndef SWIMLANEWIDGET_H
#define SWIMLANEWIDGET_H

#pragma once

#include <QHash>
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
class QPushButton;
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

    // Pedoman project (nama file di folder pedoman, lihat ProjectGuidelines): jumlahnya di tombol
    // "Pedoman", daftarnya di popup tombol itu. Popup yang sedang terbuka ikut diperbarui.
    void setGuidelines(const QString &directory, const QStringList &names);
    QStringList guidelines() const { return m_guidelines; }

    // Popup daftar pedoman di bawah tombol "Pedoman": buka & hapus per file, tambah file
    void showGuidelinePopup();

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
    // Dari popup pedoman: "Tambah file…" dan tombol × per file
    void guidelineAddRequested(const QString &projectId);
    void guidelineRemoveRequested(const QString &projectId, const QString &fileName);
    void cardMoved(const QString &projectId,
                   KanbanCardWidget *card,
                   const QString &targetStage,
                   int targetIndex);

protected:
    // Lebar swimlane, font, atau stylesheet berubah: bentuk header dihitung ulang
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;

private slots:
    void onCloseClicked();
    void handleCardDropped(KanbanCardWidget *card, const QString &targetStage, int targetIndex);

private:
    Ui::SwimlaneWidget *ui;
    QString m_projectId;
    QStringList m_referenceDirs;
    QString m_guidelineDir;
    QStringList m_guidelines;

    // Popup daftar di bawah tombol header "Referensi" atau "Pedoman"; satu yang terbuka sekaligus
    enum class ListPopup { References, Guidelines };
    QPointer<QFrame> m_listPopup;
    ListPopup m_listPopupKind = ListPopup::References;

    // Teks tombol header yang berikon (Kanvas, folder kerja, branch, Referensi, Pedoman). Selama baris
    // header tidak muat utuh, tombol-tombol itu tinggal ikon (teksnya tetap ada di tooltip) dan
    // judul project dipendekkan: board bisa menyempit, mis. di samping drawer hasil agent, tanpa
    // ada tombol yang terdorong keluar.
    QHash<QPushButton *, QString> m_headerTexts;
    // Lebar baris header dalam bentuk utuhnya; -1 = harus diukur ulang
    int m_fullHeaderWidth = -1;
    // Lebar minimum tombol header dari file .ui; hanya berlaku selama tombolnya berteks
    int m_headerButtonMinWidth = 0;

    void setHeaderText(QPushButton *button, const QString &text);
    // Ukur ulang bentuk utuh header (teks atau jumlah tombolnya berubah), lalu pilih bentuknya
    void remeasureHeader();
    void updateHeaderMode();
    void setHeaderCompact(bool compact);

    // Mapping nama stage ke pointer kolom
    QMap<QString, KanbanColumnWidget *> m_columns;

    void initializeColumns(const StageCatalog &catalog);
    void showListPopup(ListPopup kind);
    QPushButton *listPopupAnchor() const;
    // Isi popup (judul, baris per folder/file, tombol tambah) dari m_referenceDirs atau m_guidelines
    void fillListPopup(QFrame *popup);
    // Popup yang terbuka diisi ulang di tempat bila daftarnya yang berubah
    void refreshListPopup(ListPopup kind);
    // Ukur ulang popup, lalu gantung di bawah tombolnya dan jaga tetap di dalam layar
    void placeListPopup(QFrame *popup);
};
#endif // SWIMLANEWIDGET_H

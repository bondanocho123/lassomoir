#ifndef PROMPTEDITOR_H
#define PROMPTEDITOR_H

#pragma once

#include "TaskAttachments.h"

#include <QFrame>
#include <QList>
#include <QPixmap>
#include <QString>
#include <QStringList>

class QHBoxLayout;
class QLabel;
class QMimeData;
class QPlainTextEdit;
class QScrollArea;
class QVBoxLayout;

// Kotak prompt di form task (field SUBTEXT): teks multi-baris untuk agent beserta lampirannya.
// Foto bisa ditempel (Ctrl+V), diseret ke kotak, atau dipilih lewat tombol Foto; tiap foto tampil
// sebagai thumbnail (klik = pratinjau, × = hapus). Dokumen Excel/Word/CSV masuk lewat tombol File
// (atau diseret) dan tampil sebagai daftar di bawah thumbnail.
// Tidak menulis ke disk: pemanggil menyimpan attachments() lewat TaskAttachments::save().
class PromptEditor : public QFrame {
    Q_OBJECT

public:
    explicit PromptEditor(QWidget *parent = nullptr);

    QString text() const;
    void setText(const QString &text);
    void setPlaceholderText(const QString &text);
    QPlainTextEdit *textEdit() const { return m_text; }

    // Lampiran yang sudah tersimpan di folder lampiran task (mode edit)
    void setStoredAttachments(const QString &directory, const QStringList &fileNames);

    // Lampiran saat ini: foto dulu, lalu dokumen
    QList<TaskAttachments::Draft> attachments() const;
    int imageCount() const { return int(m_images.size()); }
    int documentCount() const { return int(m_documents.size()); }

    // Foto dari clipboard atau hasil seret. false bila ditolak; alasannya di notice().
    bool addImage(const QImage &image, const QString &fileName = QString());
    // File dari disk: foto atau dokumen menurut ekstensinya. Kembalikan jumlah yang masuk.
    int addFiles(const QStringList &paths);
    void removeImage(int index);
    void removeDocument(int index);

    // Pesan penolakan terakhir (format tidak didukung, batas foto); kosong bila tidak ada
    QString notice() const { return m_notice; }

    // Dipanggil kotak teks untuk isi clipboard/seret: true bila isinya diperlakukan sebagai lampiran
    bool canTakeAttachments(const QMimeData *source) const;
    bool takeAttachments(const QMimeData *source);

public slots:
    void chooseImages();
    void chooseDocuments();
    // Dialog pratinjau ukuran penuh, dengan tombol hapus
    void previewImage(int index);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    // Garis kotak ikut menyala saat kotak teksnya fokus
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    bool appendImage(const TaskAttachments::NormalizedImage &image, const QString &baseName, QString *problem);
    QString uniqueDraftName(const QString &fileName) const;
    QPixmap pixmapFor(const TaskAttachments::Draft &draft) const;
    QPixmap thumbnailFor(const TaskAttachments::Draft &draft) const;
    void rebuildImages();
    void rebuildDocuments();
    void setNotice(const QString &text);

    QPlainTextEdit *m_text;
    QScrollArea *m_imageStrip;
    QWidget *m_imageRow;
    QHBoxLayout *m_imageLayout;
    QWidget *m_documentList;
    QVBoxLayout *m_documentLayout;
    QLabel *m_hint;

    QList<TaskAttachments::Draft> m_images;
    QList<QPixmap> m_thumbnails;   // sejajar dengan m_images
    QList<TaskAttachments::Draft> m_documents;
    QString m_notice;
};

#endif // PROMPTEDITOR_H

#ifndef TASKATTACHMENTS_H
#define TASKATTACHMENTS_H

#pragma once

#include "TaskItem.h"
#include "TaskMaterials.h"

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QSize>
#include <QString>
#include <QStringList>

// Lampiran task (foto dan dokumen Excel/Word/CSV) di folder lampiran task,
// <AppData>/projects/<project>/attachments/<taskId>/ (lihat FileManager::attachmentDirectory).
namespace TaskAttachments {

// Foto ikut sebagai blok gambar di setiap run. Claude memperkecil sisi di atas 1568 px, menolak
// gambar di atas 5 MB (base64), dan satu request maksimal 32 MB; batas di bawah menjaga semuanya.
constexpr int kMaxImageSide = 1568;
constexpr qint64 kMaxImageBytes = 3500 * 1024;
constexpr qint64 kMaxTotalImageBytes = 18 * 1024 * 1024;
constexpr int kMaxImages = 20;
constexpr qint64 kMaxDocumentBytes = 50 * 1024 * 1024;

// Isi dokumen yang ikut prompt, per dokumen dan total satu task. Dokumen yang lebih panjang
// dipotong; teks lengkapnya ditulis ke .teks/<nama>.txt supaya agent bisa membacanya dengan Read.
constexpr qsizetype kDocumentPromptChars = 40000;
constexpr qsizetype kDocumentsPromptChars = 100000;

bool isImage(const QString &fileName);      // png, jpg, jpeg, gif, bmp, webp
bool isDocument(const QString &fileName);   // DocumentText::supportedSuffixes()

// Filter untuk QFileDialog
QString imageFilter();
QString documentFilter();

// Lampiran di form task sebelum disimpan
struct Draft {
    QString fileName;       // nama tampilan, juga nama file tujuan (bisa diberi akhiran " (2)")
    QString storedPath;     // lampiran lama: file di folder lampiran task
    QString sourcePath;     // dokumen baru: disalin saat task disimpan
    QByteArray imageData;   // foto baru: hasil normalizeImage()

    bool isImage() const { return TaskAttachments::isImage(fileName); }
    bool isStored() const { return !storedPath.isEmpty(); }
    qint64 size() const;    // byte, untuk tampilan dan batas total foto
};

// Foto siap simpan: orientasi EXIF diterapkan, sisi terpanjang <= kMaxImageSide, PNG (JPEG untuk
// foto kamera atau bila PNG-nya terlalu besar), dan <= kMaxImageBytes
struct NormalizedImage {
    QByteArray data;
    QString suffix;   // "png" atau "jpg"
    QSize size;
    QString error;    // kalimat untuk pengguna; data kosong
};
NormalizedImage normalizeImage(const QImage &image, bool photo = false);
NormalizedImage normalizeImageFile(const QString &path);

// Nama file yang aman di Windows dan belum ada di taken (tanpa beda huruf besar/kecil):
// "data.csv" -> "data (2).csv"
QString uniqueName(const QString &fileName, const QStringList &taken);

// Samakan folder lampiran dengan daftar draft: draft baru ditulis/disalin, lampiran lama yang
// tidak ada lagi di daftar dihapus. Kembalikan nama file yang tersimpan, urut sesuai daftar;
// draft yang gagal ditulis dilewati dan alasannya masuk *errors.
QStringList save(const QString &directory, const QList<Draft> &drafts, const QStringList &previous,
                 QStringList *errors = nullptr);

// Bahan run: foto dan isi teks dokumen dari folder lampiran task, plus folder referensi project
// yang masih ada. Membaca disk; panggil tepat sebelum run.
TaskMaterials materials(const TaskItem &task, const QString &attachmentDirectory,
                        const QStringList &referenceDirectories);

}

#endif // TASKATTACHMENTS_H

#ifndef BRANCHVIEWER_H
#define BRANCHVIEWER_H

#pragma once

#include "GitHistory.h"

#include <QDialog>
#include <QList>
#include <QString>

#include <functional>

class DiffView;
class QComboBox;
class QLabel;
class QPushButton;
class QTimer;
class QToolButton;
class QTreeWidget;

// Jendela riwayat git satu project: pilih branch, lihat daftar commit-nya, lalu bandingkan
// perubahannya. Klik satu commit = perubahan commit itu terhadap induknya; Ctrl + klik dua commit =
// perubahan dari yang lama ke yang baru; pilih branch pembanding = seluruh perubahan branch sejak
// bercabang dari pembandingnya, seperti merge request (commit-nya saja yang didaftar). Hanya
// membaca repository: memilih branch di sini tidak men-checkout apa pun. Non-modal, satu jendela
// per project. Git dibaca di thread pool; jawaban yang tersusul permintaan lebih baru dibuang.
class BranchViewer : public QDialog {
    Q_OBJECT

public:
    BranchViewer(const QString &projectId, const QString &workingDirectory, QWidget *parent = nullptr);

    // Munculkan jendela project ini yang sudah terbuka di jendela parent, atau buka baru. branch kosong
    // = branch yang aktif di folder kerja; compareWith kosong = tanpa pembanding. Keduanya nama tampil
    // ("main", "origin/main").
    static BranchViewer *showProject(const QString &projectId, const QString &workingDirectory, QWidget *parent,
                                     const QString &branch = QString(), const QString &compareWith = QString());
    // Jendela project ini yang sedang terbuka di jendela parent; nullptr bila tidak ada
    static BranchViewer *find(const QString &projectId, QWidget *parent);

    QString projectId() const { return m_projectId; }
    QString workingDirectory() const { return m_workingDirectory; }
    // Branch yang sedang dilihat dan pembandingnya (nama tampil); kosong bila belum ada / tanpa pembanding
    QString branch() const;
    QString compareTarget() const;

    // Pilih branch dan pembandingnya; bila daftar branch belum terbaca, dipilih begitu terbaca
    void selectBranch(const QString &branch, const QString &compareWith = QString());
    // Folder kerja project diganti: semuanya dibaca ulang dari folder baru
    void setWorkingDirectory(const QString &directory);
    // Baca ulang daftar branch, commit, dan perubahan yang sedang dipilih (F5)
    void reload();

signals:
    // HEAD folder kerja terbaca ulang (branch aktif bisa sudah diganti di luar aplikasi)
    void headRead(const QString &projectId, const GitHead &head);

private:
    // Jalankan work di thread pool lalu apply di thread GUI, selama jendela masih ada
    template <typename T>
    void runGit(std::function<T()> work, std::function<void(const T &)> apply);

    void applyBranches(const GitBranchList &list);
    void fillBranchSelectors(const QString &branch, const QString &compareWith);
    const GitBranch *selectedBranch() const;
    const GitBranch *selectedTarget() const;
    // Daftar commit dari awal: riwayat branch, atau commit yang belum ada di pembanding
    void loadCommits();
    void loadMoreCommits();
    void applyLog(const GitLog &log, bool append);
    void applyComparison(const GitComparison &comparison);
    void showListMessage(const QString &text, bool error);
    void addCommitItems(int from);
    void updateListTitle();
    // Pilihan di daftar commit -> judul detail + perubahan (dijeda sebentar, lihat m_selectionTimer)
    void showSelection();
    void loadDiff(const QString &from, const QString &to, bool fromMergeBase);
    void setDetail(const QString &title, const QString &meta, const QString &body);
    void updateLocation();

    QString m_projectId;
    QString m_workingDirectory;
    GitBranchList m_branches;
    QList<GitCommit> m_commits;    // commit di daftar, terbaru dulu
    GitComparison m_comparison;    // berlaku bila ada pembanding
    QString m_revision;            // revisi git daftar commit ("A..B" bila ada pembanding)
    bool m_hasMore = false;
    int m_total = -1;
    QString m_pendingBranch;       // pilihan yang menunggu daftar branch terbaca
    QString m_pendingCompare;
    bool m_hasPending = false;
    bool m_loadingBranches = false;
    int m_branchRequest = 0;
    int m_logRequest = 0;
    int m_diffRequest = 0;

    QComboBox *m_branch;
    QComboBox *m_compare;
    QToolButton *m_swap;
    QPushButton *m_reload;
    QLabel *m_location;
    QLabel *m_listTitle;
    QLabel *m_listMessage;
    QTreeWidget *m_list;
    QPushButton *m_more;
    QLabel *m_detailTitle;
    QLabel *m_detailMeta;
    QLabel *m_detailBody;
    DiffView *m_diffView;
    QTimer *m_selectionTimer;
};

#endif // BRANCHVIEWER_H

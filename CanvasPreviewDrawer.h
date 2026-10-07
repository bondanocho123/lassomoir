#pragma once

#include <QPointer>
#include <QRect>
#include <QString>
#include <QWidget>

class ElidedLabel;
class MarkdownView;
class MermaidRenderer;
class QFrame;
class QLabel;
class QPushButton;
class QVariantAnimation;

// Drawer pratinjau Markdown di halaman kanvas: isi kartu terpilih dalam bentuk jadi (judul, daftar,
// tabel, kode, diagram Mermaid). Meluncur keluar dari tepi kanan kanvas, di sebelah panel kanan, dan
// menimpa kanvas (juga Pustaka bila ruangnya sempit) tanpa menggeser tata letak halaman. Tombol
// perluas membuatnya menutupi seluruh halaman di bawah toolbar.
// Pasif: isi dan batas ruangnya datang dari CanvasPage.
class CanvasPreviewDrawer : public QWidget {
    Q_OBJECT

public:
    explicit CanvasPreviewDrawer(MermaidRenderer *renderer, QWidget *parent = nullptr);

    // documentId berbeda dari yang sedang tampil = dokumen lain: gulirnya kembali ke atas. Dokumen
    // yang sama (mis. catatan yang sedang diketik) mempertahankan posisi gulirnya.
    void showDocument(const QString &documentId, const QString &kind, const QString &title, const QString &markdown);
    QString markdown() const;

    // area: ruang halaman di bawah toolbar; dockRight: tepi kanan drawer selama tidak diperluas
    // (eksklusif). Keduanya di koordinat widget induk.
    void setBounds(const QRect &area, int dockRight);

    void slideIn();
    void slideOut();
    bool isOpen() const { return m_open; }

    void setExpanded(bool expanded);
    bool isExpanded() const { return m_expanded; }

signals:
    void closeRequested();   // tombol ✕ atau Esc
    void openChanged(bool open);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    // Tempat drawer saat terbuka penuh, termasuk pita bayangan di kirinya; shadow = lebar pita itu
    QRect restingRect(int *shadow = nullptr) const;
    // slideWidth > 0: panel dipertahankan selebar itu selagi drawer meluncur, jadi isinya ikut
    // bergeser alih-alih ditata ulang di setiap frame
    void animateTo(const QRect &target, int slideWidth);
    void finishAnimation();
    void layoutPanel();
    void updateExpandButton();

    QRect m_area;
    int m_dockRight = 0;
    bool m_open = false;
    bool m_expanded = false;
    int m_shadow = 0;       // lebar pita bayangan di kiri panel
    int m_slideWidth = 0;
    QString m_documentId;
    QPointer<QVariantAnimation> m_animation;

    QFrame *m_panel;
    QLabel *m_kind;
    ElidedLabel *m_title;
    QPushButton *m_expand;
    MarkdownView *m_view;
};

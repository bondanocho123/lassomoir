#ifndef SIDEPANELDOCK_H
#define SIDEPANELDOCK_H

#pragma once

#include <QObject>
#include <QPointer>
#include <QString>

class QSplitter;
class QTimer;
class QVariantAnimation;
class QWidget;
class VerticalTabButton;

// Panel di tepi kanan sebuah QSplitter mendatar (panel Lieutenant) dengan tiga keadaan:
//   Pinned  terpasang tetap di pane-nya; lebarnya digeser lewat handle splitter
//   Tab     tinggal rel sempit berisi tab tegak ber-caption. Hover atau klik tab menampilkan
//           panel sementara di kiri rel, menimpa pane sebelahnya tanpa menggesernya, sampai
//           kursor meninggalkan rel + panel
//   Hidden  panel dan relnya hilang; lebarnya jatuh ke pane sebelahnya
//
// Caranya sama dengan sidebar project di MainWindow: pane splitter diisi rel yang memegang tempat
// (lebar) panel, sedangkan panelnya mengambang di atas overlayParent dan mengikuti geometri rel.
class SidePanelDock : public QObject {
    Q_OBJECT

public:
    enum class Mode { Pinned, Tab, Hidden };
    Q_ENUM(Mode)

    // Panel = widget di pane paneIndex (bukan pane pertama: pane di kirinya yang memberi dan
    // menerima lebar). Rel dan tabnya bernama name + "Rail" / "Tab" untuk styles.qss.
    SidePanelDock(QSplitter *splitter, int paneIndex, QWidget *overlayParent, const QString &name,
                  const QString &caption, QObject *parent = nullptr);

    Mode mode() const { return m_mode; }
    void setMode(Mode mode);

    // Tampil (Pinned atau Tab) / Hidden; menampilkan lagi memakai keadaan sebelum disembunyikan
    bool isPanelVisible() const { return m_mode != Mode::Hidden; }
    void setPanelVisible(bool visible);

    // Keadaan Tab: panel tampil sementara di samping rel. showFlyout() diabaikan di keadaan lain
    // dan selama rel masih beranimasi.
    void showFlyout();
    void closeFlyout();
    bool isFlyoutOpen() const { return m_flyoutOpen; }

signals:
    void modeChanged(SidePanelDock::Mode mode);

protected:
    // Panel mengikuti rel-nya; hover tab menampilkan panel sementara
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // Klik tab (mouse atau keyboard): buka panel sementara, atau tutup bila kursor tidak di atasnya
    void handleTabClicked();
    // Tutup panel sementara begitu kursor sudah meninggalkan rel + panel
    void pollFlyout();
    bool cursorOverFlyout() const;

    // Geser lebar rel dari lebarnya sekarang ke target; settle() memasang batas lebar keadaan baru
    void animateRail(int target);
    void settle();
    bool isAnimating() const;
    // Lebar pane rel; selisihnya diambil dari / dikembalikan ke pane di kirinya
    int paneWidth() const;
    void setPaneWidth(int width);
    // Tampilkan / sembunyikan rel (berlebar 0) tanpa mengubah lebar pane selain pane di kirinya
    void setRailShown(bool shown);
    // Lebar panel saat terpasang (lebar terakhirnya, dalam batas minimum/maksimum panel) dan
    // lebar rel saat tinggal tab
    int pinnedWidth() const;
    int tabRailWidth() const;
    // Tempatkan panel: menutupi rel (terpasang) atau di kiri rel (tampil sementara)
    void syncPanelGeometry();

    QSplitter *m_splitter;
    int m_index;
    QWidget *m_panel;
    QWidget *m_overlay;
    QWidget *m_rail = nullptr;
    VerticalTabButton *m_tab = nullptr;

    Mode m_mode = Mode::Pinned;
    // Keadaan tampil terakhir, dipakai saat panel dimunculkan lagi dari Hidden
    Mode m_shownMode = Mode::Pinned;
    // Batas lebar asli panel (dari file .ui); selama terpasang jadi batas lebar rel
    int m_panelMinWidth;
    int m_panelMaxWidth;
    // Lebar terakhir selagi terpasang; 0 = belum pernah dilepas
    int m_pinnedWidth = 0;
    // Lebar yang ditahan panel selama rel masih melebar di bawahnya (lihat setMode)
    int m_heldWidth = 0;
    QPointer<QVariantAnimation> m_animation;

    bool m_flyoutOpen = false;
    // Kursor sudah sempat berada di atas rel + panel sejak panel tampil sementara
    bool m_flyoutArmed = false;
    // Tombol mouse mulai ditekan selagi kursor di atas rel + panel, dan belum dilepas
    bool m_flyoutDragging = false;
    // Keadaan tombol mouse pada pemeriksaan sebelumnya
    bool m_flyoutPressed = false;
    int m_flyoutMisses = 0;
    QTimer *m_flyoutTimer = nullptr;
};

#endif // SIDEPANELDOCK_H

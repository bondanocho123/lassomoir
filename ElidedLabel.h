#ifndef ELIDEDLABEL_H
#define ELIDEDLABEL_H

#pragma once

#include <QLabel>
#include <QString>

// Label satu baris yang memotong teksnya dengan "…" mengikuti lebar yang tersedia.
// Lebar minimumnya 0, jadi tidak pernah mendorong widget di sebelahnya keluar dari baris.
// Teks lengkap muncul sebagai tooltip hanya ketika teksnya terpotong.
class ElidedLabel : public QLabel {
    Q_OBJECT

public:
    explicit ElidedLabel(const QString &text = QString(), QWidget *parent = nullptr,
                         Qt::TextElideMode mode = Qt::ElideRight);

    void setFullText(const QString &text);
    QString fullText() const { return m_fullText; }

    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void refresh();

    QString m_fullText;
    Qt::TextElideMode m_mode;
};

#endif // ELIDEDLABEL_H

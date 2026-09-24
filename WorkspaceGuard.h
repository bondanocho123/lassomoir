#ifndef WORKSPACEGUARD_H
#define WORKSPACEGUARD_H

#pragma once

#include <QSet>
#include <QString>

// Paling banyak satu agent penulis per folder kerja, lintas stage.
// Agent yang hanya membaca tidak pernah ditahan.
class WorkspaceGuard {
public:
    // Pembaca: selalu true. Penulis: true (dan folder dikunci) bila belum ada penulis lain.
    bool tryAcquire(const QString &dir, bool writes);

    // Buka kunci penulis; tidak berpengaruh untuk pembaca
    void release(const QString &dir, bool writes);

    // Sedang ada penulis di folder ini?
    bool isLocked(const QString &dir) const;

private:
    // Path absolut yang dibersihkan, supaya "E:/repo" dan "E:/repo/" dianggap sama
    static QString normalize(const QString &dir);

    QSet<QString> m_writers;   // folder yang sedang ditulis agent
};

#endif // WORKSPACEGUARD_H

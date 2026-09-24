#include "WorkspaceGuard.h"

#include <QDir>
#include <QFileInfo>

bool WorkspaceGuard::tryAcquire(const QString &dir, bool writes) {
    if (!writes) {
        return true;
    }
    const QString key = normalize(dir);
    if (m_writers.contains(key)) {
        return false;
    }
    m_writers.insert(key);
    return true;
}

void WorkspaceGuard::release(const QString &dir, bool writes) {
    if (writes) {
        m_writers.remove(normalize(dir));
    }
}

bool WorkspaceGuard::isLocked(const QString &dir) const {
    return m_writers.contains(normalize(dir));
}

QString WorkspaceGuard::normalize(const QString &dir) {
    QString path = QDir::cleanPath(QFileInfo(dir).absoluteFilePath());
#ifdef Q_OS_WIN
    // Path Windows tidak peka huruf besar/kecil
    path = path.toLower();
#endif
    return path;
}

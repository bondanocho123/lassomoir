#ifndef AGENTRUNTIME_H
#define AGENTRUNTIME_H

#pragma once

#include "AgentTypes.h"

#include <QObject>

// Satu agent yang sedang hidup. Kontrak untuk semua implementasi:
// - start() dipanggil tepat sekali;
// - eventReceived() 0..n kali, lalu finished() tepat sekali, setelah semua event;
// - finished() boleh terjadi di dalam start() (mis. program tidak ditemukan);
// - cancel() idempoten; finished("cancelled") menyusul bila run belum selesai;
// - destruktor tidak memancarkan sinyal apa pun.
class AgentSession : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    ~AgentSession() override = default;

    virtual void start() = 0;
    virtual void cancel() = 0;
    virtual bool isRunning() const = 0;

signals:
    void eventReceived(const AgentEvent &event);   // Text / ToolUse / Stderr
    void finished(const AgentResult &result);
};

// Login akun backend untuk File > Integrations. OAuth-nya dijalankan CLI backend itu sendiri di
// browser, jadi aplikasi tidak pernah memegang token. Kontrak:
// - refresh() -> statusChanged() satu kali, boleh langsung di dalam refresh();
// - start() -> finished() satu kali (boleh di dalam start()), lalu statusChanged() dengan status baru;
// - cancel() hanya berpengaruh selama login berjalan: finished(false) menyusul.
class AccountLogin : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    ~AccountLogin() override = default;

    virtual void refresh() = 0;
    virtual void start() = 0;
    virtual void cancel() = 0;
    virtual bool isRunning() const = 0;

signals:
    void statusChanged(const AccountStatus &status);
    void finished(bool success, const QString &message);   // message: alasan gagal; kosong bila berhasil atau dibatalkan
};

// Pabrik AgentSession. StageSwarm hanya mengenal interface ini, bukan QProcess,
// jadi logika gerombolan bisa dites dengan runtime palsu.
class AgentRuntime {
public:
    virtual ~AgentRuntime() = default;

    // Backend siap dipakai? Isi *reason bila tidak
    virtual bool isAvailable(QString *reason) const = 0;

    // Session baru yang belum di-start; dimiliki parent
    virtual AgentSession *createSession(const AgentLaunch &launch, QObject *parent) = 0;

    // Login akun backend ini; dimiliki parent
    virtual AccountLogin *createAccountLogin(QObject *parent) = 0;

    // Pemeriksaan lengkap (terpasang, versi, login). Boleh memblokir beberapa detik karena
    // menjalankan proses, jadi panggil dari thread lain. Bawaan: dianggap siap.
    virtual RuntimeCheck check() const { return RuntimeCheck(); }

    // Run yang gagal karena backend-nya (belum terpasang, belum login)? Selain itu Ok.
    virtual RuntimeCheck diagnose(const AgentResult &result) const {
        Q_UNUSED(result);
        return RuntimeCheck();
    }
};

#endif // AGENTRUNTIME_H

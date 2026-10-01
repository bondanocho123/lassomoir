#ifndef RUNTIMENOTICEDIALOG_H
#define RUNTIMENOTICEDIALOG_H

#pragma once

#include "AgentTypes.h"

#include <QDialog>

// Pemberitahuan bahwa Claude Code belum siap dipakai (belum terpasang, terlalu lama,
// belum login), dengan tombol membuka panduan perbaikannya atau batal.
// URL panduan tersimpan di property "helpUrl".
class RuntimeNoticeDialog : public QDialog {
    Q_OBJECT

public:
    explicit RuntimeNoticeDialog(const RuntimeCheck &check, QWidget *parent = nullptr);

    RuntimeCheck::Status status() const { return m_status; }

private:
    RuntimeCheck::Status m_status;
};

#endif // RUNTIMENOTICEDIALOG_H

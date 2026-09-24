#include "ConsolePanelWidget.h"
#include "ui_ConsolePanelWidget.h"

ConsolePanelWidget::ConsolePanelWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::ConsolePanelWidget)
{
    ui->setupUi(this);

    // Log hanya untuk dibaca. Output agent bisa panjang, jadi jumlah barisnya dibatasi
    // (baris terlama dibuang lebih dulu) supaya memori tidak terus membengkak.
    ui->textBrowserLog->setReadOnly(true);
    ui->textBrowserLog->setMaximumBlockCount(5000);

    connect(ui->lineEditPrompt, &QLineEdit::returnPressed,
            this, &ConsolePanelWidget::onInputSubmitted);
}

ConsolePanelWidget::~ConsolePanelWidget()
{
    delete ui;
}

void ConsolePanelWidget::appendLog(const QString &log) {
    ui->textBrowserLog->appendPlainText(log);
}

void ConsolePanelWidget::onInputSubmitted() {
    QString cmd = ui->lineEditPrompt->text().trimmed();
    if (!cmd.isEmpty()) {
        appendLog("> " + cmd);
        emit commandSubmitted(cmd);
        ui->lineEditPrompt->clear();
    }
}

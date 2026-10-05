#include "ConsolePanelWidget.h"
#include "ui_ConsolePanelWidget.h"
#include "ConsoleTaskCard.h"
#include "Theme.h"

#include <QIcon>
#include <QStringList>

namespace {
// Sisi ikon Live (px logis)
constexpr int kLiveIconSide = 16;
}

ConsolePanelWidget::ConsolePanelWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::ConsolePanelWidget)
{
    ui->setupUi(this);

    // Tanpa WA_StyledBackground latar dan garis panel dari styles.qss tidak ikut dilukis
    setAttribute(Qt::WA_StyledBackground, true);

    // Penanda "Live" berupa ikon, jadi artinya dijelaskan lewat tooltip dan nama aksesibilitas
    ui->labelLiveIndicator->setPixmap(Theme::icon(":/icons/signal.svg").pixmap(QSize(kLiveIconSide, kLiveIconSide)));
    ui->labelLiveIndicator->setToolTip(QStringLiteral("Live: notifikasi dan keluaran agent tampil di sini secara langsung"));
    ui->labelLiveIndicator->setAccessibleName(QStringLiteral("Live"));

    // Panel sendiri tidak berpindah keadaan: tombolnya hanya meminta, pemilik panel yang memutuskan
    ui->btnConsoleClose->setIcon(Theme::icon(":/icons/close.svg"));
    connect(ui->btnConsolePin, &QPushButton::clicked, this, [this]() { emit pinRequested(!m_pinned); });
    connect(ui->btnConsoleClose, &QPushButton::clicked, this, &ConsolePanelWidget::closeRequested);
    setPinned(true);
}

ConsolePanelWidget::~ConsolePanelWidget()
{
    delete ui;
}

void ConsolePanelWidget::appendLog(const QString &log) {
    if (!m_systemCard) {
        m_systemCard = new ConsoleTaskCard(QString(), ui->taskListContainer);
        ui->taskListLayout->insertWidget(0, m_systemCard);
        m_systemCard->show();
    }
    m_systemCard->appendEntry(log);
    promote(m_systemCard);
}

void ConsolePanelWidget::appendTaskLog(const TaskItem &task, const QString &log) {
    ConsoleTaskCard *card = ensureCard(task);
    card->appendEntry(log);
    promote(card);
}

void ConsolePanelWidget::appendTaskOutput(const TaskItem &task, const QString &log) {
    ensureCard(task)->appendEntry(log);
}

void ConsolePanelWidget::updateTask(const TaskItem &task) {
    if (ConsoleTaskCard *card = m_cards.value(task.id)) {
        card->setTask(task);
    }
}

void ConsolePanelWidget::setRunState(const TaskItem &task, RunState state) {
    ConsoleTaskCard *card = m_cards.value(task.id);
    if (!card && state == RunState::Idle) {
        return;
    }
    (card ? card : ensureCard(task))->setRunState(state);
}

void ConsolePanelWidget::markTaskRemoved(const TaskItem &task) {
    ensureCard(task)->setRemoved();
}

ConsoleTaskCard *ConsolePanelWidget::taskCard(const QString &taskId) const {
    return m_cards.value(taskId);
}

QList<ConsoleTaskCard *> ConsolePanelWidget::cards() const {
    QList<ConsoleTaskCard *> result;
    for (int i = 0; i < ui->taskListLayout->count(); ++i) {
        if (auto *card = qobject_cast<ConsoleTaskCard *>(ui->taskListLayout->itemAt(i)->widget())) {
            result.append(card);
        }
    }
    return result;
}

QString ConsolePanelWidget::logText() const {
    QStringList logs;
    for (ConsoleTaskCard *card : cards()) {
        logs.append(card->logText());
    }
    return logs.join(QLatin1Char('\n'));
}

void ConsolePanelWidget::setPinned(bool pinned) {
    m_pinned = pinned;
    // Pin tegak = terpasang; pin rebah = panel sedang lepas (tampil sementara dari tab di samping)
    ui->btnConsolePin->setIcon(Theme::icon(pinned ? ":/icons/pinned.svg" : ":/icons/unpinned.svg"));
    ui->btnConsolePin->setToolTip(pinned ? QStringLiteral("Lepas pin: ciutkan jadi tab di samping")
                                         : QStringLiteral("Pin panel: pasang tetap di samping"));
}

ConsoleTaskCard *ConsolePanelWidget::ensureCard(const TaskItem &task) {
    if (ConsoleTaskCard *card = m_cards.value(task.id)) {
        return card;
    }
    auto *card = new ConsoleTaskCard(task.id, ui->taskListContainer);
    card->setTask(task);
    connect(card, &ConsoleTaskCard::activated, this, &ConsolePanelWidget::taskActivated);
    ui->taskListLayout->insertWidget(0, card);
    card->show();
    m_cards.insert(task.id, card);
    return card;
}

void ConsolePanelWidget::promote(ConsoleTaskCard *card) {
    if (ui->taskListLayout->indexOf(card) == 0) {
        return;
    }
    ui->taskListLayout->removeWidget(card);
    ui->taskListLayout->insertWidget(0, card);
}

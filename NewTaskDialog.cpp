#include "NewTaskDialog.h"

#include <QComboBox>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace {
// Harus persis sama dengan key yang dikenal StageProfile (lihat StageProfile.h),
// supaya task baru tidak pernah dibuat dengan stage yang tidak valid.
const QStringList kStageKeys = {
    "WAITING", "SPECIFIER", "CODER", "CLEANER",
    "ARCHITECT", "HARDENER", "QA", "DONE"
};
const QStringList kCategoryPresets = {
    "component", "utility", "design", "bug", "feature"
};
}

NewTaskDialog::NewTaskDialog(const QString &projectId, QWidget *parent)
    : QDialog(parent), m_projectId(projectId) {
    setObjectName("NewTaskDialog");
    setWindowTitle("New Task");
    setModal(true);
    setMinimumWidth(420);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(24, 20, 24, 20);
    root->setSpacing(16);

    auto *subtitle = new QLabel(QString("%1").arg(projectId), this);
    subtitle->setObjectName("taskFormSubtitle");
    root->addWidget(subtitle);
    root->addWidget(buildDivider());

    m_titleInput = new QLineEdit(this);
    m_titleInput->setObjectName("taskFormInput");
    m_titleInput->setPlaceholderText("Rancang ulang halaman login");
    root->addWidget(buildField("TASK TITLE", m_titleInput));

    m_subtextInput = new QLineEdit(this);
    m_subtextInput->setObjectName("taskFormInput");
    m_subtextInput->setPlaceholderText("PIC: Budi / waiting in queue");
    root->addWidget(buildField("SUBTEXT", m_subtextInput));

    m_categoryInput = new QComboBox(this);
    m_categoryInput->setObjectName("taskFormCombo");
    m_categoryInput->setEditable(true);
    m_categoryInput->addItems(kCategoryPresets);
    m_categoryInput->setCurrentText("component");

    m_stageInput = new QComboBox(this);
    m_stageInput->setObjectName("taskFormCombo");
    m_stageInput->addItems(kStageKeys);
    m_stageInput->setCurrentText("WAITING");

    auto *fieldRow = new QHBoxLayout();
    fieldRow->setSpacing(12);
    fieldRow->addWidget(buildField("CATEGORY", m_categoryInput));
    fieldRow->addWidget(buildField("STAGE", m_stageInput));
    root->addLayout(fieldRow);

    auto *approvalsLabel = new QLabel("APPROVALS REQUIRED", this);
    approvalsLabel->setObjectName("taskFormFieldLabel");
    root->addWidget(approvalsLabel);

    auto *btnMinus = new QPushButton("-", this);
    btnMinus->setObjectName("btnTaskFormStepper");
    btnMinus->setFixedSize(28, 28);
    btnMinus->setCursor(Qt::PointingHandCursor);
    connect(btnMinus, &QPushButton::clicked, this, [this]() { changeApprovals(-1); });

    m_approvalsValueLabel = new QLabel("0", this);
    m_approvalsValueLabel->setObjectName("taskFormApprovalsValue");
    m_approvalsValueLabel->setAlignment(Qt::AlignCenter);
    m_approvalsValueLabel->setFixedSize(40, 28);

    auto *btnPlus = new QPushButton("+", this);
    btnPlus->setObjectName("btnTaskFormStepper");
    btnPlus->setFixedSize(28, 28);
    btnPlus->setCursor(Qt::PointingHandCursor);
    connect(btnPlus, &QPushButton::clicked, this, [this]() { changeApprovals(1); });

    auto *previewCaption = new QLabel("Preview:", this);
    previewCaption->setObjectName("taskFormPreviewCaption");

    m_badgePreviewLabel = new QLabel(this);
    m_badgePreviewLabel->setObjectName("labelBadge"); // pakai persis gaya badge kartu kanban asli

    auto *stepperRow = new QHBoxLayout();
    stepperRow->setSpacing(8);
    stepperRow->addWidget(btnMinus);
    stepperRow->addWidget(m_approvalsValueLabel);
    stepperRow->addWidget(btnPlus);
    stepperRow->addStretch(1);
    stepperRow->addWidget(previewCaption);
    stepperRow->addWidget(m_badgePreviewLabel);
    root->addLayout(stepperRow);

    root->addWidget(buildDivider());

    m_btnCreate = new QPushButton("Buat Task", this);
    m_btnCreate->setObjectName("btnTaskFormCreate");
    m_btnCreate->setCursor(Qt::PointingHandCursor);
    m_btnCreate->setEnabled(false);
    connect(m_btnCreate, &QPushButton::clicked, this, &QDialog::accept);

    auto *btnCancel = new QPushButton("Batal", this);
    btnCancel->setObjectName("btnTaskFormCancel");
    btnCancel->setCursor(Qt::PointingHandCursor);
    connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);

    auto *buttonRow = new QHBoxLayout();
    buttonRow->addStretch(1);
    buttonRow->addWidget(btnCancel);
    buttonRow->addWidget(m_btnCreate);
    root->addLayout(buttonRow);

    connect(m_titleInput, &QLineEdit::textChanged, this, &NewTaskDialog::updateCreateButtonEnabled);
    connect(m_titleInput, &QLineEdit::returnPressed, this, [this]() {
        if (m_btnCreate->isEnabled()) m_btnCreate->click();
    });

    updateBadgePreview();
}

QWidget *NewTaskDialog::buildField(const QString &labelText, QWidget *inputWidget) {
    auto *container = new QWidget(this);
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto *label = new QLabel(labelText, container);
    label->setObjectName("taskFormFieldLabel");
    layout->addWidget(label);
    layout->addWidget(inputWidget);

    return container;
}

QFrame *NewTaskDialog::buildDivider() {
    auto *line = new QFrame(this);
    line->setObjectName("taskFormDivider");
    line->setFixedHeight(1);
    line->setFrameShape(QFrame::NoFrame);
    return line;
}

void NewTaskDialog::changeApprovals(int delta) {
    m_approvals = qMax(0, m_approvals + delta);
    m_approvalsValueLabel->setText(QString::number(m_approvals));
    updateBadgePreview();
}

void NewTaskDialog::updateBadgePreview() {
    m_badgePreviewLabel->setText(QString("✓ %1").arg(m_approvals));
}

void NewTaskDialog::updateCreateButtonEnabled() {
    m_btnCreate->setEnabled(!m_titleInput->text().trimmed().isEmpty());
}

TaskItem NewTaskDialog::resultTask() const {
    TaskItem item;
    item.id = QString::number(QDateTime::currentMSecsSinceEpoch());
    item.projectId = m_projectId;
    item.stage = m_stageInput->currentText();
    item.category = m_categoryInput->currentText().trimmed();
    item.title = m_titleInput->text().trimmed();
    item.subtext = m_subtextInput->text().trimmed();
    item.approvals = m_approvals;
    item.badge = QString("✓ %1").arg(m_approvals);
    return item;
}

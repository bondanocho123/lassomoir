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
const QStringList kCategoryPresets = {
    "component", "utility", "design", "bug", "feature"
};
}

NewTaskDialog::NewTaskDialog(const QString &projectId, const QStringList &stageKeys, QWidget *parent)
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
    // Pilihan stage datang dari StageCatalog, jadi task baru tidak pernah memakai key yang tidak dikenal
    m_stageInput->addItems(stageKeys);
    m_stageInput->setCurrentText("WAITING");

    auto *fieldRow = new QHBoxLayout();
    fieldRow->setSpacing(12);
    fieldRow->addWidget(buildField("CATEGORY", m_categoryInput));
    fieldRow->addWidget(buildField("STAGE", m_stageInput));
    root->addLayout(fieldRow);

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
}

NewTaskDialog::NewTaskDialog(const TaskItem &task, const QStringList &stageKeys, QWidget *parent)
    : NewTaskDialog(task.projectId, stageKeys, parent) {
    m_editing = true;
    m_original = task;

    setWindowTitle("Edit Task");
    m_btnCreate->setText("Simpan");

    m_titleInput->setText(task.title);
    m_subtextInput->setText(task.subtext);
    m_categoryInput->setCurrentText(task.category);

    m_stageInput->setCurrentText(task.stage);
    m_stageInput->setEnabled(false);
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

void NewTaskDialog::updateCreateButtonEnabled() {
    m_btnCreate->setEnabled(!m_titleInput->text().trimmed().isEmpty());
}

TaskItem NewTaskDialog::resultTask() const {
    TaskItem item = m_original;
    if (!m_editing) {
        item.id = QString::number(QDateTime::currentMSecsSinceEpoch());
        item.projectId = m_projectId;
    }
    item.stage = m_stageInput->currentText();
    item.category = m_categoryInput->currentText().trimmed();
    item.title = m_titleInput->text().trimmed();
    item.subtext = m_subtextInput->text().trimmed();
    return item;
}

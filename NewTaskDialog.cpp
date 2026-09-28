#include "NewTaskDialog.h"
#include "PromptEditor.h"
#include "StageCatalog.h"

#include <QComboBox>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShortcut>
#include <QVBoxLayout>

namespace {
const QStringList kCategoryPresets = {
    "component", "utility", "design", "bug", "feature"
};

// Stage yang model & effort-nya boleh dipilih per task; stage lain memakai bawaan katalog
const QStringList kTunableStages = {"SPECIFIER", "CODER"};

struct Choice {
    const char *value;
    const char *label;
};
const QList<Choice> kModelChoices = {
    {"haiku", "Haiku"}, {"sonnet", "Sonnet"}, {"opus", "Opus"},
};
const QList<Choice> kEffortChoices = {
    {"low", "Low"}, {"medium", "Medium"}, {"high", "High"}, {"xhigh", "XHigh"}, {"max", "Max"},
};

// Teks item pertama: "Bawaan (Sonnet)". Nilai yang bukan alias dikenal ditampilkan apa adanya.
QString labelFor(const QList<Choice> &choices, const QString &value) {
    for (const Choice &choice : choices) {
        if (value == QLatin1String(choice.value)) {
            return QString::fromLatin1(choice.label);
        }
    }
    return value;
}

void fillCombo(QComboBox *combo, const QList<Choice> &choices, const QString &defaultValue) {
    combo->setObjectName("taskFormCombo");
    const QString defaultLabel = labelFor(choices, defaultValue);
    combo->addItem(defaultLabel.isEmpty() ? QStringLiteral("Bawaan")
                                          : QStringLiteral("Bawaan (%1)").arg(defaultLabel),
                   QString());
    for (const Choice &choice : choices) {
        combo->addItem(QString::fromLatin1(choice.label), QString::fromLatin1(choice.value));
    }
}
}

NewTaskDialog::NewTaskDialog(const QString &projectId, const StageCatalog &catalog, QWidget *parent)
    : QDialog(parent), m_projectId(projectId) {
    setObjectName("NewTaskDialog");
    setWindowTitle("New Task");
    setModal(true);
    setMinimumWidth(480);

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

    // Subtext = prompt untuk agent: multi-baris, bisa berisi foto dan dokumen lampiran
    m_promptInput = new PromptEditor(this);
    m_promptInput->setPlaceholderText("Instruksi untuk agent: tujuan, batasan, contoh…\n"
                                      "Foto bisa ditempel (Ctrl+V) atau diseret ke sini.");
    root->addWidget(buildField("SUBTEXT / PROMPT", m_promptInput), 1);

    m_categoryInput = new QComboBox(this);
    m_categoryInput->setObjectName("taskFormCombo");
    m_categoryInput->setEditable(true);
    m_categoryInput->addItems(kCategoryPresets);
    m_categoryInput->setCurrentText("component");

    m_stageInput = new QComboBox(this);
    m_stageInput->setObjectName("taskFormCombo");
    // Pilihan stage datang dari StageCatalog, jadi task baru tidak pernah memakai key yang tidak dikenal
    m_stageInput->addItems(catalog.keys());
    m_stageInput->setCurrentText("WAITING");

    auto *fieldRow = new QHBoxLayout();
    fieldRow->setSpacing(12);
    fieldRow->addWidget(buildField("CATEGORY", m_categoryInput));
    fieldRow->addWidget(buildField("STAGE", m_stageInput));
    root->addLayout(fieldRow);

    // Model & effort agent per stage; hanya stage yang punya agent di katalog yang ditampilkan
    QList<QWidget *> tuningRows;
    for (const QString &stageKey : kTunableStages) {
        const StageProfile *profile = catalog.profile(stageKey);
        if (profile && profile->agent()) {
            tuningRows.append(buildTuningRow(stageKey, *profile->agent()));
        }
    }
    if (!tuningRows.isEmpty()) {
        root->addWidget(buildDivider());
        for (QWidget *row : std::as_const(tuningRows)) {
            root->addWidget(row);
        }
    }

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
    // Enter di kotak prompt menulis baris baru; Ctrl+Enter menyimpan dari mana saja
    for (const QKeySequence &keys : {QKeySequence(Qt::CTRL | Qt::Key_Return), QKeySequence(Qt::CTRL | Qt::Key_Enter)}) {
        connect(new QShortcut(keys, this), &QShortcut::activated, this, [this]() {
            if (m_btnCreate->isEnabled()) m_btnCreate->click();
        });
    }
}

NewTaskDialog::NewTaskDialog(const TaskItem &task, const QString &attachmentDirectory, const StageCatalog &catalog,
                             QWidget *parent)
    : NewTaskDialog(task.projectId, catalog, parent) {
    m_editing = true;
    m_original = task;

    setWindowTitle("Edit Task");
    m_btnCreate->setText("Simpan");

    m_titleInput->setText(task.title);
    m_promptInput->setText(task.subtext);
    m_promptInput->setStoredAttachments(attachmentDirectory, task.attachments);
    m_categoryInput->setCurrentText(task.category);

    m_stageInput->setCurrentText(task.stage);
    m_stageInput->setEnabled(false);

    for (auto it = m_tuningInputs.cbegin(); it != m_tuningInputs.cend(); ++it) {
        const AgentTuning tuning = task.tuning.value(it.key());
        selectValue(it->model, tuning.model);
        selectValue(it->effort, tuning.effort);
    }
}

QWidget *NewTaskDialog::buildTuningRow(const QString &stageKey, const AgentDefinition &defaults) {
    TuningInputs inputs;
    inputs.model = new QComboBox(this);
    fillCombo(inputs.model, kModelChoices, defaults.model);
    inputs.effort = new QComboBox(this);
    fillCombo(inputs.effort, kEffortChoices, defaults.effort);
    m_tuningInputs.insert(stageKey, inputs);

    auto *row = new QWidget(this);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    layout->addWidget(buildField(QStringLiteral("%1 MODEL").arg(stageKey), inputs.model));
    layout->addWidget(buildField(QStringLiteral("%1 EFFORT").arg(stageKey), inputs.effort));
    return row;
}

void NewTaskDialog::selectValue(QComboBox *combo, const QString &value) {
    int index = combo->findData(value);
    if (index < 0) {
        // Nilai tersimpan di luar daftar (mis. id model penuh): tetap ditampilkan agar tidak hilang diam-diam
        combo->addItem(value, value);
        index = combo->count() - 1;
    }
    combo->setCurrentIndex(index);
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

QList<TaskAttachments::Draft> NewTaskDialog::attachments() const {
    return m_promptInput->attachments();
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
    item.subtext = m_promptInput->text().trimmed();
    for (auto it = m_tuningInputs.cbegin(); it != m_tuningInputs.cend(); ++it) {
        const AgentTuning tuning{it->model->currentData().toString(), it->effort->currentData().toString()};
        if (tuning.isEmpty()) {
            item.tuning.remove(it.key());
        } else {
            item.tuning.insert(it.key(), tuning);
        }
    }
    return item;
}

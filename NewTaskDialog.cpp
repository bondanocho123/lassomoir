#include "NewTaskDialog.h"
#include "GitHistory.h"
#include "PromptEditor.h"
#include "StageCatalog.h"
#include "TaskGit.h"

#include <QComboBox>
#include <QDateTime>
#include <QFrame>
#include <QFutureWatcher>
#include <QSet>
#include <QtConcurrent/QtConcurrentRun>
#include <QFont>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QAction>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QShortcut>
#include <QVBoxLayout>

#include <algorithm>

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

const QString kBranchHint = QStringLiteral("Task bercabang dari branch ini. Branch ini di-pull dari origin "
                                           "saat task dibuat dan sebelum setiap run.");
const QString kOriginPrefix = QStringLiteral("origin/");
const QString kTaskBranchPrefix = QStringLiteral("lassomoir/");
constexpr int kBranchLabelMaxWidth = 380;   // nama branch lebih panjang dipendekkan di tengah

// "&" di nama branch jangan dibaca sebagai penanda shortcut tombol/menu
QString escapeMnemonic(QString text) {
    return text.replace(QLatin1Char('&'), QStringLiteral("&&"));
}

// Hasil fetch origin + daftar branch sesudahnya
struct FetchedBranches {
    QString error;        // kosong = daftar dari origin sudah diperbarui
    GitBranchList list;
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

    // Branch dipilih paling dulu: seluruh pekerjaan task berangkat dari branch ini. Tampil sebagai
    // label + caret (menu-indicator di styles.qss), selebar namanya dan menempel kiri.
    m_branchButton = new QPushButton(this);
    m_branchButton->setObjectName("taskFormBranch");
    m_branchButton->setCursor(Qt::PointingHandCursor);
    m_branchMenu = new QMenu(this);
    m_branchMenu->setObjectName("taskFormBranchMenu");
    m_branchMenu->setToolTipsVisible(true);
    m_branchHint = new QLabel(this);
    m_branchHint->setObjectName("taskFormHint");
    m_branchHint->setWordWrap(true);
    QWidget *branchField = buildField("BRANCH", m_branchButton);
    branchField->layout()->setAlignment(m_branchButton, Qt::AlignLeft);
    branchField->layout()->addWidget(m_branchHint);
    root->addWidget(branchField);
    showBranchMessage("Folder kerja belum dipilih",
                      "Branch task mengikuti branch yang aktif di folder kerja saat run pertama.");

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

    // Branch task sudah dibuat dari branch dasarnya: tidak bisa dipindah lagi
    if (!task.branch.isEmpty()) {
        showBranchMessage(task.branch.base.isEmpty() ? task.branch.name : task.branch.base,
                          QString("Branch task %1 sudah dibuat dari branch ini.").arg(task.branch.name));
    }
}

void NewTaskDialog::loadBranches(const QString &workingDirectory) {
    if (!m_original.branch.isEmpty()) {
        return;
    }
    if (workingDirectory.isEmpty()) {
        showBranchMessage("Folder kerja belum dipilih",
                          "Branch task mengikuti branch yang aktif di folder kerja saat run pertama.");
        return;
    }
    // Folder biasa dikenali tanpa menjalankan git
    if (!TaskGit::looksLikeRepository(workingDirectory)) {
        showBranchMessage("Folder kerja bukan repository git", "Task berjalan tanpa branch.");
        return;
    }

    m_branchesLoading = true;
    showBranchMessage("Memuat branch…", kBranchHint);
    updateCreateButtonEnabled();
    // Dialog ditutup sebelum git selesai: watcher ikut terhapus, hasilnya dibuang
    auto *watcher = new QFutureWatcher<GitBranchList>(this);
    connect(watcher, &QFutureWatcher<GitBranchList>::finished, this, [this, watcher, workingDirectory]() {
        watcher->deleteLater();
        m_branchesLoading = false;
        showBranches(watcher->result());
        updateCreateButtonEnabled();
        if (!m_branchOptions.isEmpty()) {
            fetchBranches(workingDirectory);
        }
    });
    watcher->setFuture(QtConcurrent::run(&GitHistory::branches, workingDirectory));
}

void NewTaskDialog::fetchBranches(const QString &workingDirectory) {
    m_branchHint->setText("Memperbarui daftar branch dari origin…");
    auto *watcher = new QFutureWatcher<FetchedBranches>(this);
    connect(watcher, &QFutureWatcher<FetchedBranches>::finished, this, [this, watcher]() {
        watcher->deleteLater();
        const FetchedBranches fetched = watcher->result();
        if (!fetched.error.isEmpty()) {
            m_branchHint->setText(QString("Daftar branch dari origin belum diperbarui (%1). %2").arg(fetched.error, kBranchHint));
            return;
        }
        showBranches(fetched.list);
    });
    watcher->setFuture(QtConcurrent::run([workingDirectory]() {
        FetchedBranches fetched;
        fetched.error = TaskGit::fetchOrigin(workingDirectory).error;
        if (fetched.error.isEmpty()) {
            fetched.list = GitHistory::branches(workingDirectory);
        }
        return fetched;
    }));
}

void NewTaskDialog::showBranches(const GitBranchList &list) {
    if (!list.error.isEmpty()) {
        showBranchMessage("Branch tidak bisa dibaca", list.error);
        return;
    }
    const QString selected = m_branchOptions.isEmpty() ? QString() : m_branch;

    QSet<QString> locals;
    for (const GitBranch &branch : list.branches) {
        if (!branch.remote) {
            locals.insert(branch.name);
        }
    }

    // Urutan GitHistory dipertahankan: yang aktif, lokal lain, lalu yang hanya ada di origin
    QList<BranchOption> options;
    QString current;
    for (const GitBranch &branch : list.branches) {
        QString base = branch.name;
        if (branch.remote) {
            if (!branch.name.startsWith(kOriginPrefix)) {
                continue;
            }
            base = branch.name.mid(kOriginPrefix.size());
            if (locals.contains(base)) {
                continue;
            }
        }
        if (base.startsWith(kTaskBranchPrefix)) {
            continue;
        }

        QStringList tip = {QString("%1 · %2").arg(branch.subject, GitHistory::relativeTime(branch.date))};
        if (branch.remote) {
            tip.append("Baru ada di origin: branch lokalnya dibuat saat di-pull");
        } else if (!branch.track.isEmpty()) {
            tip.append(QString("%1 dibanding %2").arg(branch.track, branch.upstream));
        }
        options.append({branch.current ? QString("%1 (aktif)").arg(base) : branch.name, base, tip.join('\n')});
        if (branch.current) {
            current = base;
        }
    }
    if (options.isEmpty()) {
        showBranchMessage("Repository belum punya commit", "Task berjalan tanpa branch sampai ada commit pertama.");
        return;
    }

    // Pilihan pengguna dipertahankan saat daftar diperbarui; task yang diedit tetap di branch dasar
    // pilihannya; task baru mulai dari branch yang aktif (atau yang teratas bila detached HEAD)
    QString chosen = !selected.isEmpty() ? selected : m_original.branch.base;
    if (chosen.isEmpty()) {
        chosen = current.isEmpty() ? options.first().base : current;
    }
    const bool known = std::any_of(options.cbegin(), options.cend(),
                                   [&chosen](const BranchOption &option) { return option.base == chosen; });
    if (!known) {
        options.append({chosen, chosen, QStringLiteral("Tidak ditemukan di repository")});
    }

    m_branchOptions = options;
    m_branchMenu->clear();
    for (const BranchOption &option : std::as_const(m_branchOptions)) {
        QAction *action = m_branchMenu->addAction(escapeMnemonic(option.text));
        action->setData(option.base);
        action->setToolTip(option.tip);
        const QString base = option.base;
        connect(action, &QAction::triggered, this, [this, base]() { selectBranch(base); });
    }
    m_branchButton->setMenu(m_branchMenu);
    m_branchButton->setEnabled(true);
    m_branchHint->setText(kBranchHint);
    selectBranch(chosen);
}

void NewTaskDialog::showBranchMessage(const QString &message, const QString &hint) {
    m_branchOptions.clear();
    m_branch.clear();
    m_branchMenu->clear();
    // Tanpa menu: tidak ada caret, dan klik tidak membuka apa-apa
    m_branchButton->setMenu(nullptr);
    m_branchButton->setText(escapeMnemonic(message));
    m_branchButton->setToolTip(QString());
    m_branchButton->setEnabled(false);
    m_branchHint->setText(hint);
    m_branchHint->setVisible(!hint.isEmpty());
}

void NewTaskDialog::selectBranch(const QString &base) {
    m_branch = base;
    QString text = base;
    QString tip;
    for (const BranchOption &option : std::as_const(m_branchOptions)) {
        if (option.base == base) {
            text = option.text;
            tip = option.tip;
        }
    }
    for (QAction *action : m_branchMenu->actions()) {
        QFont font = action->font();
        font.setBold(action->data().toString() == base);
        action->setFont(font);
    }
    // Lebar label mengikuti font dari styles.qss, jadi dipoles dulu sebelum dipendekkan
    m_branchButton->ensurePolished();
    const QString shown = m_branchButton->fontMetrics().elidedText(text, Qt::ElideMiddle, kBranchLabelMaxWidth);
    m_branchButton->setText(escapeMnemonic(shown));
    m_branchButton->setToolTip(shown == text ? tip : text + QLatin1Char('\n') + tip);
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
    m_btnCreate->setEnabled(!m_titleInput->text().trimmed().isEmpty() && !m_branchesLoading);
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
    if (!m_branchOptions.isEmpty()) {
        item.branch.base = m_branch;
    }
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

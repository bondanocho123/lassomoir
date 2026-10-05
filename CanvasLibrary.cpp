#include "CanvasLibrary.h"
#include "CanvasWorkflow.h"
#include "Theme.h"

#include <QApplication>
#include <QFontInfo>
#include <QFontMetrics>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMimeData>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int kSourceRole = Qt::UserRole + 1;
constexpr int kKeyRole = Qt::UserRole + 2;
constexpr int kDetailRole = Qt::UserRole + 3;

QJsonObject sourceObject(const QTreeWidgetItem *item) {
    return item->data(0, kSourceRole).toJsonObject();
}

// Dua baris per item: nama di atas, keterangan redup di bawah (stage, status, jenis lampiran),
// supaya nama tidak terpotong oleh kolom keterangan di panel yang sempit
class LibraryDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        if (!index.data(kDetailRole).toString().isEmpty()) {
            const QFontMetrics metrics(option.font);
            size.setHeight(qMax(size.height(), metrics.height() * 2 + 4));
        }
        return size;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const QString detail = index.data(kDetailRole).toString();
        if (detail.isEmpty()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        QStyleOptionViewItem item(option);
        initStyleOption(&item, index);
        const QString label = item.text;
        item.text.clear();
        const QWidget *widget = item.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        // Latar, sorotan hover/terpilih, dan fokus tetap dari style (dan styles.qss)
        style->drawControl(QStyle::CE_ItemViewItem, &item, painter, widget);

        const QRect area = style->subElementRect(QStyle::SE_ItemViewItemText, &item, widget).adjusted(2, 0, -2, 0);
        QFont labelFont = item.font;
        QFont detailFont = item.font;
        detailFont.setPixelSize(qMax(9, QFontInfo(item.font).pixelSize() - 2));
        const QFontMetrics labelMetrics(labelFont);
        const QFontMetrics detailMetrics(detailFont);
        const int top = area.top() + (area.height() - labelMetrics.height() - detailMetrics.height()) / 2;
        painter->save();
        painter->setFont(labelFont);
        painter->setPen(Theme::text(0x3d3730));
        painter->drawText(QRect(area.left(), top, area.width(), labelMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                          labelMetrics.elidedText(label, Qt::ElideRight, area.width()));
        painter->setFont(detailFont);
        painter->setPen(Theme::text(0x8a7f70));
        painter->drawText(QRect(area.left(), top + labelMetrics.height(), area.width(), detailMetrics.height()),
                          Qt::AlignLeft | Qt::AlignVCenter, detailMetrics.elidedText(detail, Qt::ElideRight, area.width()));
        painter->restore();
    }
};

// Item yang bisa diseret membawa CanvasSource; baris project tidak
class LibraryTree final : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;

protected:
    QStringList mimeTypes() const override { return {CanvasLibrary::kMimeType}; }

    QMimeData *mimeData(const QList<QTreeWidgetItem *> &items) const override {
        QList<CanvasSource> sources;
        for (const QTreeWidgetItem *item : items) {
            const QJsonObject object = sourceObject(item);
            if (!object.isEmpty()) {
                sources.append(CanvasSource::fromJson(object));
            }
        }
        return sources.isEmpty() ? nullptr : CanvasLibrary::mimeData(sources);
    }

    Qt::DropActions supportedDropActions() const override { return Qt::CopyAction; }
};

QString taskSummary(const TaskItem &task) {
    switch (task.state) {
    case TaskState::AwaitingReview: return QStringLiteral("%1 · review").arg(task.stage);
    case TaskState::Failed: return QStringLiteral("%1 · gagal").arg(task.stage);
    case TaskState::Idle: break;
    }
    return task.stage;
}

}

const QString CanvasLibrary::kMimeType = QStringLiteral("application/x-lassomoir-canvas-source");

QMimeData *CanvasLibrary::mimeData(const QList<CanvasSource> &sources) {
    QJsonArray array;
    for (const CanvasSource &source : sources) {
        array.append(source.toJson());
    }
    auto *mime = new QMimeData();
    mime->setData(kMimeType, QJsonDocument(array).toJson(QJsonDocument::Compact));
    return mime;
}

QList<CanvasSource> CanvasLibrary::sources(const QMimeData *mime) {
    QList<CanvasSource> result;
    if (!mime || !mime->hasFormat(kMimeType)) {
        return result;
    }
    const QJsonArray array = QJsonDocument::fromJson(mime->data(kMimeType)).array();
    for (const QJsonValue &value : array) {
        const CanvasSource source = CanvasSource::fromJson(value.toObject());
        if (source.isValid()) {
            result.append(source);
        }
    }
    return result;
}

CanvasLibrary::CanvasLibrary(QWidget *parent) : QWidget(parent) {
    setObjectName("canvasLibrary");
    setAttribute(Qt::WA_StyledBackground, true);

    auto *title = new QLabel(QStringLiteral("PUSTAKA ARTEFAK"), this);
    title->setObjectName("canvasPanelTitle");
    auto *hint = new QLabel(QStringLiteral("Seret dokumen hasil stage, lampiran, atau task ke kanvas"), this);
    hint->setObjectName("canvasPanelHint");
    hint->setWordWrap(true);

    m_filter = new QLineEdit(this);
    m_filter->setObjectName("canvasLibraryFilter");
    m_filter->setPlaceholderText(QStringLiteral("Cari task atau artefak…"));
    m_filter->setClearButtonEnabled(true);

    m_tree = new LibraryTree(this);
    m_tree->setObjectName("canvasLibraryTree");
    m_tree->setColumnCount(1);
    m_tree->setHeaderHidden(true);
    m_tree->setItemDelegate(new LibraryDelegate(m_tree));
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setDragEnabled(true);
    m_tree->setDragDropMode(QAbstractItemView::DragOnly);
    m_tree->setRootIsDecorated(true);
    m_tree->setFrameShape(QFrame::NoFrame);

    m_empty = new QLabel(QStringLiteral("Belum ada task di project yang terbuka. Dokumen hasil run task akan "
                                        "muncul di sini."),
                         this);
    m_empty->setObjectName("canvasPanelHint");
    m_empty->setWordWrap(true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(6);
    layout->addWidget(title);
    layout->addWidget(hint);
    layout->addWidget(m_filter);
    layout->addWidget(m_tree, 1);
    layout->addWidget(m_empty);
    layout->addStretch(0);

    connect(m_filter, &QLineEdit::textChanged, this, &CanvasLibrary::applyFilter);
    connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        const QJsonObject object = sourceObject(item);
        if (!object.isEmpty()) {
            emit sourceActivated(CanvasSource::fromJson(object));
        }
    });
    connect(m_tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) { rememberExpansion(item, true); });
    connect(m_tree, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *item) { rememberExpansion(item, false); });
}

void CanvasLibrary::rememberExpansion(QTreeWidgetItem *item, bool expanded) {
    if (m_rebuilding || !m_filter->text().trimmed().isEmpty()) {
        return;
    }
    const QString key = item->data(0, kKeyRole).toString();
    (expanded ? m_expanded : m_collapsed).insert(key);
    (expanded ? m_collapsed : m_expanded).remove(key);
}

void CanvasLibrary::setTasks(const QList<TaskItem> &tasks, const QString &currentProject, const QStringList &stageOrder) {
    m_rebuilding = true;
    m_tree->clear();

    QMap<QString, QList<TaskItem>> byProject;
    for (const TaskItem &task : tasks) {
        byProject[task.projectId].append(task);
    }
    QStringList projects = byProject.keys();
    if (projects.removeOne(currentProject)) {
        projects.prepend(currentProject);
    }

    for (const QString &projectId : std::as_const(projects)) {
        auto *projectItem = new QTreeWidgetItem(m_tree, {projectId});
        const QString projectKey = QStringLiteral("p:") + projectId;
        projectItem->setData(0, kKeyRole, projectKey);
        projectItem->setFlags(Qt::ItemIsEnabled);
        QFont bold = projectItem->font(0);
        bold.setBold(true);
        projectItem->setFont(0, bold);
        projectItem->setToolTip(0, projectId == currentProject ? QStringLiteral("Project kanvas ini")
                                                               : QStringLiteral("Project lain yang terbuka"));

        QList<TaskItem> projectTasks = byProject.value(projectId);
        std::sort(projectTasks.begin(), projectTasks.end(), [&stageOrder](const TaskItem &a, const TaskItem &b) {
            const qsizetype first = stageOrder.indexOf(a.stage);
            const qsizetype second = stageOrder.indexOf(b.stage);
            if (first != second) {
                return first < second;
            }
            return a.title.localeAwareCompare(b.title) < 0;
        });
        for (const TaskItem &task : std::as_const(projectTasks)) {
            auto *taskItem = new QTreeWidgetItem(projectItem, {task.title});
            taskItem->setData(0, kDetailRole, taskSummary(task));
            const QString taskKey = QStringLiteral("t:") + task.id;
            taskItem->setData(0, kKeyRole, taskKey);
            taskItem->setData(0, kSourceRole, CanvasSource{task.projectId, task.id, QString(), QString()}.toJson());
            taskItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
            taskItem->setToolTip(0, QStringLiteral("%1\nSeret ke kanvas sebagai kartu task (status ikut terbarui)").arg(task.title));

            const QList<CanvasWorkflow::Artifact> artifacts = CanvasWorkflow::artifactsOf(task, stageOrder);
            for (const CanvasWorkflow::Artifact &artifact : artifacts) {
                auto *artifactItem = new QTreeWidgetItem(taskItem, {artifact.label});
                artifactItem->setData(0, kDetailRole, artifact.detail);
                artifactItem->setData(0, kSourceRole, artifact.source.toJson());
                artifactItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
                artifactItem->setToolTip(0, QStringLiteral("%1 · %2\nSeret ke kanvas, atau klik dua kali")
                                                .arg(artifact.label, task.title));
            }
            taskItem->setExpanded(m_expanded.contains(taskKey));
        }
        projectItem->setExpanded(m_expanded.contains(projectKey)
                                 || (projectId == currentProject && !m_collapsed.contains(projectKey)));
    }

    m_empty->setVisible(tasks.isEmpty());
    m_tree->setVisible(!tasks.isEmpty());
    m_rebuilding = false;
    applyFilter();
}

void CanvasLibrary::applyFilter() {
    const QString needle = m_filter->text().trimmed();
    auto matches = [&needle](const QTreeWidgetItem *item) {
        return item->text(0).contains(needle, Qt::CaseInsensitive)
               || item->data(0, kDetailRole).toString().contains(needle, Qt::CaseInsensitive);
    };
    for (int p = 0; p < m_tree->topLevelItemCount(); ++p) {
        QTreeWidgetItem *project = m_tree->topLevelItem(p);
        bool projectVisible = false;
        for (int t = 0; t < project->childCount(); ++t) {
            QTreeWidgetItem *task = project->child(t);
            const bool taskMatches = needle.isEmpty() || matches(task);
            bool anyArtifact = false;
            for (int a = 0; a < task->childCount(); ++a) {
                QTreeWidgetItem *artifact = task->child(a);
                const bool matchesArtifact = taskMatches || matches(artifact);
                artifact->setHidden(!matchesArtifact);
                anyArtifact = anyArtifact || matchesArtifact;
            }
            const bool visible = taskMatches || anyArtifact;
            task->setHidden(!visible);
            if (!needle.isEmpty() && anyArtifact && !taskMatches) {
                task->setExpanded(true);
            }
            projectVisible = projectVisible || visible;
        }
        project->setHidden(!projectVisible && !needle.isEmpty());
        if (!needle.isEmpty() && projectVisible) {
            project->setExpanded(true);
        }
    }
}

#include "PromptEditor.h"
#include "DocumentText.h"
#include "Theme.h"

#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLocale>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QStandardPaths>
#include <QStyle>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int kThumbSide = 64;
constexpr int kRemovePreviewResult = 2;   // QDialog::done() dari tombol "Hapus foto"
const QString kDefaultHint = QStringLiteral("Ctrl+V atau seret untuk menempel foto");

// Kotak teks yang menyerahkan gambar dan file hasil tempel/seret ke PromptEditor
class PromptTextEdit final : public QPlainTextEdit {
public:
    explicit PromptTextEdit(PromptEditor *owner) : QPlainTextEdit(owner), m_owner(owner) {}

protected:
    bool canInsertFromMimeData(const QMimeData *source) const override {
        return m_owner->canTakeAttachments(source) || QPlainTextEdit::canInsertFromMimeData(source);
    }

    void insertFromMimeData(const QMimeData *source) override {
        if (!m_owner->takeAttachments(source)) {
            QPlainTextEdit::insertFromMimeData(source);
        }
    }

private:
    PromptEditor *m_owner;
};

QStringList localFiles(const QMimeData *source) {
    QStringList paths;
    if (source && source->hasUrls()) {
        const QList<QUrl> urls = source->urls();
        for (const QUrl &url : urls) {
            if (url.isLocalFile()) {
                paths.append(url.toLocalFile());
            }
        }
    }
    return paths;
}

// File yang ditangani sebagai lampiran (termasuk .xls/.doc, supaya ditolak dengan alasan yang jelas
// alih-alih path-nya tertempel sebagai teks)
bool isAttachmentFile(const QString &path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    return TaskAttachments::isImage(path) || TaskAttachments::isDocument(path)
           || suffix == QLatin1String("xls") || suffix == QLatin1String("doc");
}

QString sizeText(qint64 bytes) {
    return QLocale::system().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

// Jenis dokumen untuk warna label di styles.qss
QString documentKind(const QString &fileName) {
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    if (suffix == QLatin1String("docx")) {
        return QStringLiteral("word");
    }
    if (suffix == QLatin1String("csv") || suffix == QLatin1String("tsv")) {
        return QStringLiteral("csv");
    }
    return QStringLiteral("excel");
}

// Buang widget lama dari layout. deleteLater: tombol × yang sedang mengirim sinyal klik ada di
// antara widget itu, jadi tidak boleh dihapus langsung.
void clearLayout(QLayout *layout) {
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
}

}

PromptEditor::PromptEditor(QWidget *parent) : QFrame(parent) {
    setObjectName("taskPromptBox");
    setAcceptDrops(true);
    // Dialog yang diperbesar memberi ruang tambahannya ke kotak teks, bukan ke label field di atasnya
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

    auto *text = new PromptTextEdit(this);
    m_text = text;
    m_text->setObjectName("taskFormPrompt");
    m_text->setFrameShape(QFrame::NoFrame);
    m_text->setTabChangesFocus(true);   // form: Tab pindah ke field berikutnya
    m_text->setMinimumHeight(110);
    m_text->installEventFilter(this);

    // Thumbnail foto berderet; bergulir ke samping bila tidak muat
    m_imageRow = new QWidget();
    m_imageRow->setObjectName("promptImageRow");
    m_imageLayout = new QHBoxLayout(m_imageRow);
    m_imageLayout->setContentsMargins(0, 0, 0, 0);
    m_imageLayout->setSpacing(6);
    m_imageStrip = new QScrollArea(this);
    m_imageStrip->setObjectName("promptImageStrip");
    m_imageStrip->setWidget(m_imageRow);
    m_imageStrip->setWidgetResizable(true);
    m_imageStrip->setFrameShape(QFrame::NoFrame);
    m_imageStrip->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_imageStrip->setFixedHeight(kThumbSide + 6 + 12);   // thumbnail + ruang scrollbar
    m_imageStrip->hide();

    m_documentList = new QWidget(this);
    m_documentList->setObjectName("promptDocumentList");
    m_documentLayout = new QVBoxLayout(m_documentList);
    m_documentLayout->setContentsMargins(0, 0, 0, 0);
    m_documentLayout->setSpacing(4);
    m_documentList->hide();

    auto *toolbar = new QFrame(this);
    toolbar->setObjectName("promptToolbar");
    auto *addImage = new QPushButton(Theme::icon(":/icons/image.svg"), QStringLiteral("Foto"), toolbar);
    addImage->setObjectName("btnPromptAddImage");
    addImage->setCursor(Qt::PointingHandCursor);
    addImage->setToolTip("Lampirkan foto. Bisa juga Ctrl+V atau seret gambar ke kotak ini.");
    connect(addImage, &QPushButton::clicked, this, &PromptEditor::chooseImages);
    auto *addDocument = new QPushButton(Theme::icon(":/icons/paperclip.svg"), QStringLiteral("File"), toolbar);
    addDocument->setObjectName("btnPromptAddDocument");
    addDocument->setCursor(Qt::PointingHandCursor);
    addDocument->setToolTip("Lampirkan file Excel (.xlsx), Word (.docx), atau CSV; isinya ikut dibaca agent");
    connect(addDocument, &QPushButton::clicked, this, &PromptEditor::chooseDocuments);
    m_hint = new QLabel(kDefaultHint, toolbar);
    m_hint->setObjectName("promptHint");
    m_hint->setWordWrap(true);
    m_hint->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    // Pesan panjang dibungkus, bukan melebarkan dialog
    m_hint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto *toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(4, 4, 6, 4);
    toolbarLayout->setSpacing(4);
    toolbarLayout->addWidget(addImage);
    toolbarLayout->addWidget(addDocument);
    toolbarLayout->addWidget(m_hint, 1);

    auto *attachments = new QVBoxLayout();
    attachments->setContentsMargins(8, 0, 8, 6);
    attachments->setSpacing(6);
    attachments->addWidget(m_imageStrip);
    attachments->addWidget(m_documentList);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);
    layout->addWidget(m_text, 1);
    layout->addLayout(attachments);
    layout->addWidget(toolbar);
}

QString PromptEditor::text() const {
    return m_text->toPlainText();
}

void PromptEditor::setText(const QString &text) {
    m_text->setPlainText(text);
}

void PromptEditor::setPlaceholderText(const QString &text) {
    m_text->setPlaceholderText(text);
}

void PromptEditor::setStoredAttachments(const QString &directory, const QStringList &fileNames) {
    m_images.clear();
    m_thumbnails.clear();
    m_documents.clear();
    const QDir dir(directory);
    for (const QString &name : fileNames) {
        TaskAttachments::Draft draft;
        draft.fileName = name;
        draft.storedPath = dir.filePath(name);
        if (draft.isImage()) {
            m_images.append(draft);
            m_thumbnails.append(thumbnailFor(draft));
        } else {
            m_documents.append(draft);
        }
    }
    rebuildImages();
    rebuildDocuments();
}

QList<TaskAttachments::Draft> PromptEditor::attachments() const {
    return m_images + m_documents;
}

bool PromptEditor::addImage(const QImage &image, const QString &fileName) {
    const TaskAttachments::NormalizedImage normalized = TaskAttachments::normalizeImage(image);
    const QString baseName = fileName.isEmpty()
                                 ? QStringLiteral("tempel-%1").arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"))
                                 : QFileInfo(fileName).completeBaseName();
    QString problem = normalized.error;
    if (problem.isEmpty() && appendImage(normalized, baseName, &problem)) {
        setNotice(QString());
        rebuildImages();
        return true;
    }
    setNotice(problem);
    return false;
}

int PromptEditor::addFiles(const QStringList &paths) {
    int added = 0;
    QStringList problems;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (TaskAttachments::isImage(path)) {
            const TaskAttachments::NormalizedImage normalized = TaskAttachments::normalizeImageFile(path);
            QString problem = normalized.error;
            if (problem.isEmpty() && appendImage(normalized, info.completeBaseName(), &problem)) {
                ++added;
            } else {
                problems.append(QStringLiteral("%1: %2").arg(info.fileName(), problem));
            }
            continue;
        }

        QString error;
        if (!DocumentText::check(path, &error)) {
            problems.append(QStringLiteral("%1: %2").arg(info.fileName(), error));
            continue;
        }
        if (info.size() > TaskAttachments::kMaxDocumentBytes) {
            problems.append(QStringLiteral("%1: lebih dari %2").arg(info.fileName(), sizeText(TaskAttachments::kMaxDocumentBytes)));
            continue;
        }
        const QString source = info.absoluteFilePath();
        const bool duplicate = std::any_of(m_documents.cbegin(), m_documents.cend(),
                                           [&source](const TaskAttachments::Draft &draft) {
            return QFileInfo(draft.sourcePath).absoluteFilePath() == source;
        });
        if (duplicate) {
            continue;   // file yang sama tidak perlu dilampirkan dua kali
        }
        TaskAttachments::Draft draft;
        draft.fileName = uniqueDraftName(info.fileName());
        draft.sourcePath = source;
        m_documents.append(draft);
        ++added;
    }

    rebuildImages();
    rebuildDocuments();
    setNotice(problems.join(QStringLiteral(" · ")));
    return added;
}

bool PromptEditor::appendImage(const TaskAttachments::NormalizedImage &image, const QString &baseName, QString *problem) {
    if (m_images.size() >= TaskAttachments::kMaxImages) {
        *problem = QStringLiteral("maksimal %1 foto per task").arg(TaskAttachments::kMaxImages);
        return false;
    }
    qint64 total = image.data.size();
    for (const TaskAttachments::Draft &draft : std::as_const(m_images)) {
        total += draft.size();
    }
    if (total > TaskAttachments::kMaxTotalImageBytes) {
        *problem = QStringLiteral("total foto melebihi %1").arg(sizeText(TaskAttachments::kMaxTotalImageBytes));
        return false;
    }

    TaskAttachments::Draft draft;
    draft.fileName = uniqueDraftName(QStringLiteral("%1.%2").arg(baseName, image.suffix));
    draft.imageData = image.data;
    m_images.append(draft);
    m_thumbnails.append(thumbnailFor(draft));
    return true;
}

void PromptEditor::removeImage(int index) {
    if (index < 0 || index >= m_images.size()) {
        return;
    }
    m_images.removeAt(index);
    m_thumbnails.removeAt(index);
    setNotice(QString());
    rebuildImages();
}

void PromptEditor::removeDocument(int index) {
    if (index < 0 || index >= m_documents.size()) {
        return;
    }
    m_documents.removeAt(index);
    setNotice(QString());
    rebuildDocuments();
}

bool PromptEditor::canTakeAttachments(const QMimeData *source) const {
    if (!source) {
        return false;
    }
    const QStringList files = localFiles(source);
    if (std::any_of(files.cbegin(), files.cend(), isAttachmentFile)) {
        return true;
    }
    return source->hasImage();
}

bool PromptEditor::takeAttachments(const QMimeData *source) {
    if (!source) {
        return false;
    }
    // File dari Explorer lebih dulu: nama aslinya ikut terbawa
    QStringList files = localFiles(source);
    files.removeIf([](const QString &path) { return !isAttachmentFile(path); });
    if (!files.isEmpty()) {
        addFiles(files);
        return true;
    }
    if (source->hasImage()) {
        addImage(qvariant_cast<QImage>(source->imageData()));
        return true;
    }
    return false;
}

void PromptEditor::chooseImages() {
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Lampirkan foto"),
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation), TaskAttachments::imageFilter());
    if (!paths.isEmpty()) {
        addFiles(paths);
    }
}

void PromptEditor::chooseDocuments() {
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Lampirkan file Excel, Word, atau CSV"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation), TaskAttachments::documentFilter());
    if (!paths.isEmpty()) {
        addFiles(paths);
    }
}

void PromptEditor::previewImage(int index) {
    if (index < 0 || index >= m_images.size()) {
        return;
    }
    const TaskAttachments::Draft draft = m_images.at(index);
    const QPixmap pixmap = pixmapFor(draft);

    QDialog dialog(this);
    dialog.setObjectName("imagePreviewDialog");
    dialog.setWindowTitle(draft.fileName);

    auto *image = new QLabel(&dialog);
    image->setObjectName("imagePreview");
    image->setAlignment(Qt::AlignCenter);
    if (pixmap.isNull()) {
        image->setText(QStringLiteral("Foto tidak bisa dibuka"));
    } else {
        // Muat di ~80% layar, tanpa diperbesar melebihi ukuran aslinya
        const QSize available = screen()->availableGeometry().size() * 0.8 - QSize(48, 120);
        const bool tooLarge = pixmap.width() > available.width() || pixmap.height() > available.height();
        image->setPixmap(tooLarge ? pixmap.scaled(available, Qt::KeepAspectRatio, Qt::SmoothTransformation) : pixmap);
    }

    auto *info = new QLabel(pixmap.isNull() ? QString()
                                            : QStringLiteral("%1 × %2 px · %3")
                                                  .arg(pixmap.width()).arg(pixmap.height()).arg(sizeText(draft.size())),
                            &dialog);
    info->setObjectName("imagePreviewInfo");
    auto *remove = new QPushButton(QStringLiteral("Hapus foto"), &dialog);
    remove->setObjectName("btnPreviewRemove");
    remove->setCursor(Qt::PointingHandCursor);
    auto *close = new QPushButton(QStringLiteral("Tutup"), &dialog);
    close->setObjectName("btnPreviewClose");
    close->setCursor(Qt::PointingHandCursor);
    close->setDefault(true);
    connect(remove, &QPushButton::clicked, &dialog, [&dialog]() { dialog.done(kRemovePreviewResult); });
    connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);

    auto *actions = new QHBoxLayout();
    actions->addWidget(info, 1);
    actions->addWidget(remove);
    actions->addWidget(close);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(16, 16, 16, 14);
    layout->setSpacing(12);
    layout->addWidget(image, 1);
    layout->addLayout(actions);

    if (dialog.exec() == kRemovePreviewResult) {
        removeImage(index);
    }
}

void PromptEditor::dragEnterEvent(QDragEnterEvent *event) {
    if (canTakeAttachments(event->mimeData())) {
        event->acceptProposedAction();
    }
}

void PromptEditor::dragMoveEvent(QDragMoveEvent *event) {
    if (canTakeAttachments(event->mimeData())) {
        event->acceptProposedAction();
    }
}

void PromptEditor::dropEvent(QDropEvent *event) {
    if (takeAttachments(event->mimeData())) {
        event->acceptProposedAction();
    }
}

bool PromptEditor::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_text && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)) {
        setProperty("focused", event->type() == QEvent::FocusIn);
        style()->unpolish(this);
        style()->polish(this);
    }
    return QFrame::eventFilter(watched, event);
}

QString PromptEditor::uniqueDraftName(const QString &fileName) const {
    QStringList taken;
    for (const TaskAttachments::Draft &draft : m_images + m_documents) {
        taken.append(draft.fileName);
    }
    return TaskAttachments::uniqueName(fileName, taken);
}

QPixmap PromptEditor::pixmapFor(const TaskAttachments::Draft &draft) const {
    QPixmap pixmap;
    if (draft.imageData.isEmpty()) {
        pixmap.load(draft.storedPath);
    } else {
        pixmap.loadFromData(draft.imageData);
    }
    return pixmap;
}

QPixmap PromptEditor::thumbnailFor(const TaskAttachments::Draft &draft) const {
    const QPixmap source = pixmapFor(draft);
    if (source.isNull()) {
        return QPixmap();
    }
    // Potongan persegi dari tengah foto, setajam layar HiDPI
    const qreal ratio = devicePixelRatioF();
    const int side = qRound(kThumbSide * ratio);
    const QPixmap scaled = source.scaled(side, side, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    QPixmap thumbnail = scaled.copy((scaled.width() - side) / 2, (scaled.height() - side) / 2, side, side);
    thumbnail.setDevicePixelRatio(ratio);
    return thumbnail;
}

void PromptEditor::rebuildImages() {
    clearLayout(m_imageLayout);
    for (int i = 0; i < m_images.size(); ++i) {
        const TaskAttachments::Draft &draft = m_images.at(i);
        auto *thumb = new QToolButton(m_imageRow);
        thumb->setObjectName("promptThumb");
        thumb->setIcon(QIcon(m_thumbnails.at(i)));
        thumb->setIconSize(QSize(kThumbSide, kThumbSide));
        thumb->setFixedSize(kThumbSide + 6, kThumbSide + 6);
        thumb->setCursor(Qt::PointingHandCursor);
        thumb->setToolTip(m_thumbnails.at(i).isNull()
                              ? QStringLiteral("%1\nFile foto tidak ditemukan").arg(draft.fileName)
                              : QStringLiteral("%1 · %2\nKlik untuk pratinjau").arg(draft.fileName, sizeText(draft.size())));
        connect(thumb, &QToolButton::clicked, this, [this, i]() { previewImage(i); });

        auto *remove = new QToolButton(thumb);
        remove->setObjectName("btnThumbRemove");
        remove->setIcon(Theme::icon(":/icons/close-white.svg"));
        remove->setIconSize(QSize(8, 8));
        remove->setFixedSize(18, 18);
        remove->move(thumb->width() - remove->width() - 2, 2);
        remove->setCursor(Qt::PointingHandCursor);
        remove->setToolTip(QStringLiteral("Hapus %1").arg(draft.fileName));
        connect(remove, &QToolButton::clicked, this, [this, i]() { removeImage(i); });

        m_imageLayout->addWidget(thumb);
        // Anak baru dari induk yang sudah tampil baru muncul di siklus event berikutnya; tampilkan sekarang
        thumb->show();
    }
    m_imageLayout->addStretch(1);
    m_imageStrip->setVisible(!m_images.isEmpty());
}

void PromptEditor::rebuildDocuments() {
    clearLayout(m_documentLayout);
    for (int i = 0; i < m_documents.size(); ++i) {
        const TaskAttachments::Draft &draft = m_documents.at(i);
        auto *row = new QFrame(m_documentList);
        row->setObjectName("promptDocument");

        auto *kind = new QLabel(QFileInfo(draft.fileName).suffix().toUpper(), row);
        kind->setObjectName("promptDocumentKind");
        kind->setProperty("kind", documentKind(draft.fileName));
        kind->setAlignment(Qt::AlignCenter);

        auto *name = new QLabel(row);
        name->setObjectName("promptDocumentName");
        name->ensurePolished();   // font dari stylesheet, supaya pemotongan teks diukur dengan benar
        name->setText(name->fontMetrics().elidedText(draft.fileName, Qt::ElideMiddle, 260));
        name->setToolTip(QDir::toNativeSeparators(draft.isStored() ? draft.storedPath : draft.sourcePath));
        name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

        auto *size = new QLabel(sizeText(draft.size()), row);
        size->setObjectName("promptDocumentSize");

        auto *remove = new QToolButton(row);
        remove->setObjectName("btnDocumentRemove");
        remove->setIcon(Theme::icon(":/icons/close.svg"));
        remove->setIconSize(QSize(10, 10));
        remove->setCursor(Qt::PointingHandCursor);
        remove->setToolTip(QStringLiteral("Hapus %1").arg(draft.fileName));
        connect(remove, &QToolButton::clicked, this, [this, i]() { removeDocument(i); });

        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(6, 3, 4, 3);
        layout->setSpacing(8);
        layout->addWidget(kind);
        layout->addWidget(name, 1);
        layout->addWidget(size);
        layout->addWidget(remove);
        m_documentLayout->addWidget(row);
        row->show();
    }
    m_documentList->setVisible(!m_documents.isEmpty());
}

void PromptEditor::setNotice(const QString &text) {
    m_notice = text;
    m_hint->setText(text.isEmpty() ? kDefaultHint : text);
    m_hint->setToolTip(text);
    m_hint->setProperty("error", !text.isEmpty());
    m_hint->style()->unpolish(m_hint);
    m_hint->style()->polish(m_hint);
}

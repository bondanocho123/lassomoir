#include "NotionExportDialog.h"
#include "NotionAccess.h"
#include "NotionClient.h"

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int kDialogWidth = 480;
constexpr int kSearchDelayMs = 300;
constexpr int kTitleLimit = 200;
constexpr int kIdRole = Qt::UserRole;
constexpr int kTitleRole = Qt::UserRole + 1;

QLabel *fieldLabel(const QString &text, QWidget *parent) {
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("notionExportField"));
    return label;
}

}

NotionExportDialog::NotionExportDialog(const QString &token, const QString &title, const QString &markdown,
                                       QWidget *parent)
    : QDialog(parent),
      m_markdown(markdown),
      m_client(new NotionClient(token, this)) {
    setObjectName(QStringLiteral("notionExportDialog"));
    setWindowTitle(QStringLiteral("Kirim ke Notion"));

    auto *heading = new QLabel(QStringLiteral("Kirim ke Notion"), this);
    heading->setObjectName(QStringLiteral("notionExportTitle"));
    auto *hint = new QLabel(QStringLiteral("Isinya menjadi halaman baru di bawah halaman yang dipilih. Halaman lain di "
                                           "Notion tidak diubah."),
                            this);
    hint->setObjectName(QStringLiteral("notionExportHint"));
    hint->setWordWrap(true);

    m_title = new QLineEdit(NotionExport::pageTitle(title), this);
    m_title->setObjectName(QStringLiteral("notionExportPageTitle"));
    m_title->setMaxLength(kTitleLimit);
    m_title->setPlaceholderText(QStringLiteral("Judul halaman baru"));

    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("notionExportSearch"));
    m_search->setPlaceholderText(QStringLiteral("Cari halaman…"));
    m_search->setClearButtonEnabled(true);
    m_pages = new QListWidget(this);
    m_pages->setObjectName(QStringLiteral("notionExportPages"));
    m_pages->setMinimumHeight(170);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("notionExportStatus"));
    m_status->setWordWrap(true);
    m_error = new QLabel(this);
    m_error->setObjectName(QStringLiteral("notionExportError"));
    // Isinya bisa dari Notion: tampil apa adanya, bukan sebagai HTML
    m_error->setTextFormat(Qt::PlainText);
    m_error->setWordWrap(true);
    m_error->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_error->hide();

    m_open = new QPushButton(QStringLiteral("Buka di Notion"), this);
    m_open->setObjectName(QStringLiteral("btnNotionExportOpen"));
    m_open->setCursor(Qt::PointingHandCursor);
    m_open->setAutoDefault(false);
    m_open->hide();
    m_cancel = new QPushButton(QStringLiteral("Batal"), this);
    m_cancel->setObjectName(QStringLiteral("btnNotionExportCancel"));
    m_cancel->setCursor(Qt::PointingHandCursor);
    m_cancel->setAutoDefault(false);
    m_send = new QPushButton(QStringLiteral("Kirim"), this);
    m_send->setObjectName(QStringLiteral("btnNotionExportSend"));
    m_send->setCursor(Qt::PointingHandCursor);
    m_send->setDefault(true);

    auto *actions = new QHBoxLayout();
    actions->setSpacing(8);
    actions->addStretch(1);
    actions->addWidget(m_cancel);
    actions->addWidget(m_open);
    actions->addWidget(m_send);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 14);
    root->setSpacing(8);
    root->addWidget(heading);
    root->addWidget(hint);
    root->addSpacing(2);
    root->addWidget(fieldLabel(QStringLiteral("JUDUL HALAMAN"), this));
    root->addWidget(m_title);
    root->addSpacing(2);
    root->addWidget(fieldLabel(QStringLiteral("DI BAWAH HALAMAN"), this));
    root->addWidget(m_search);
    root->addWidget(m_pages, 1);
    root->addWidget(m_status);
    root->addWidget(m_error);
    root->addLayout(actions);

    m_searchDelay.setSingleShot(true);
    m_searchDelay.setInterval(kSearchDelayMs);
    connect(&m_searchDelay, &QTimer::timeout, this, &NotionExportDialog::search);
    connect(m_search, &QLineEdit::textChanged, this, [this]() {
        // Daftar yang tampil masih milik kata cari sebelumnya
        m_searching = true;
        refreshSend();
        m_searchDelay.start();
    });
    connect(m_title, &QLineEdit::textChanged, this, &NotionExportDialog::refreshSend);
    connect(m_pages, &QListWidget::itemSelectionChanged, this, &NotionExportDialog::refreshSend);
    connect(m_pages, &QListWidget::itemDoubleClicked, this, [this]() {
        if (m_send->isEnabled()) {
            send();
        }
    });
    connect(m_send, &QPushButton::clicked, this, &NotionExportDialog::send);
    connect(m_cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_open, &QPushButton::clicked, this, [this]() {
        const QUrl url(m_url);
        // Alamatnya datang dari jawaban API: hanya halaman web yang dibuka
        if (url.scheme() == QLatin1String("https")) {
            QDesktopServices::openUrl(url);
        }
    });

    connect(m_client, &NotionClient::pagesFound, this, &NotionExportDialog::showPages);
    connect(m_client, &NotionClient::searchFailed, this, [this](const QString &message) {
        m_searching = false;
        m_pages->clear();
        showStatus(message, true);
        refreshSend();
    });
    connect(m_client, &NotionClient::pageCreated, this, &NotionExportDialog::finish);
    connect(m_client, &NotionClient::createFailed, this, [this](const QString &message) {
        m_sending = false;
        showStatus(message, true);
        refreshSend();
    });

    setMinimumWidth(kDialogWidth);
    // Font dari stylesheet harus sudah terpasang sebelum tinggi teks dihitung
    ensurePolished();
    resize(kDialogWidth, sizeHint().height());
    m_pages->setFocus();
    search();
}

void NotionExportDialog::search() {
    m_searchDelay.stop();
    m_searching = true;
    showStatus(QStringLiteral("Memuat halaman dari Notion…"));
    refreshSend();
    m_client->searchPages(m_search->text());
}

void NotionExportDialog::showPages(const QList<NotionExport::Page> &found) {
    m_searching = false;
    const QString query = m_search->text().trimmed();
    const NotionAccess::Parent last = NotionAccess::lastParent();
    // Pilihan pengguna dipertahankan; tanpa pilihan, halaman induk terakhir yang dipilih
    const QString wanted = selectedId().isEmpty() ? last.id : selectedId();

    QList<NotionExport::Page> pages = found;
    const bool lastListed = std::any_of(pages.cbegin(), pages.cend(),
                                        [&last](const NotionExport::Page &page) { return page.id == last.id; });
    if (query.isEmpty() && !last.id.isEmpty() && !lastListed) {
        // Di luar hasil yang dikembalikan Notion, tetapi tetap tujuan yang paling mungkin dipakai lagi
        pages.prepend({last.id, last.title, QString(), QString()});
    }

    m_pages->clear();
    for (const NotionExport::Page &page : std::as_const(pages)) {
        auto *item = new QListWidgetItem(page.icon.isEmpty() ? page.title : page.icon + QLatin1Char(' ') + page.title,
                                         m_pages);
        item->setData(kIdRole, page.id);
        item->setData(kTitleRole, page.title);
        if (page.id == wanted) {
            m_pages->setCurrentItem(item);
        }
    }

    if (!pages.isEmpty()) {
        showStatus(QString());
    } else if (query.isEmpty()) {
        showStatus(QStringLiteral("Belum ada halaman yang dibagikan ke integrasi ini. Di Notion: buka halaman tujuan > "
                                  "••• > Connections > pilih integrasinya, lalu cari lagi di sini."));
    } else {
        showStatus(QStringLiteral("Tidak ada halaman berjudul \"%1\" yang dibagikan ke integrasi ini.").arg(query));
    }
    refreshSend();
}

void NotionExportDialog::send() {
    const QString parentId = selectedId();
    const QString title = m_title->text().simplified();
    if (parentId.isEmpty() || title.isEmpty() || m_sending || !m_url.isEmpty()) {
        return;
    }
    m_sending = true;
    showStatus(QStringLiteral("Mengirim ke Notion…"));
    refreshSend();
    m_client->createPage(parentId, title, NotionExport::toNotionMarkdown(m_markdown));
}

void NotionExportDialog::finish(const NotionExport::Page &page) {
    m_sending = false;
    m_url = page.url;
    const QString parentTitle = selectedTitle();
    NotionAccess::saveLastParent({selectedId(), parentTitle});

    showStatus(QStringLiteral("Terkirim sebagai halaman baru di bawah \"%1\".").arg(parentTitle));
    m_title->setEnabled(false);
    m_search->setEnabled(false);
    m_pages->setEnabled(false);
    m_send->hide();
    m_cancel->setText(QStringLiteral("Tutup"));
    m_open->setVisible(!m_url.isEmpty());
    m_open->setDefault(true);
    refreshSend();
    emit exported(m_title->text().simplified(), m_url);
}

void NotionExportDialog::refreshSend() {
    m_send->setEnabled(!m_searching && !m_sending && m_url.isEmpty() && !selectedId().isEmpty()
                       && !m_title->text().simplified().isEmpty());
}

void NotionExportDialog::showStatus(const QString &text, bool error) {
    m_status->setText(error ? QString() : text);
    m_status->setVisible(!error && !text.isEmpty());
    m_error->setText(error ? text : QString());
    m_error->setVisible(error && !text.isEmpty());
    // Label yang membungkus teks hanya melaporkan tinggi satu baris sebagai minimumnya (lihat
    // IntegrationsDialog::fitHeight): dialog ditinggikan bila pesannya tidak lagi muat
    layout()->activate();
    const int needed = layout()->totalMinimumHeightForWidth(width());
    if (height() < needed) {
        resize(width(), needed);
    }
}

QString NotionExportDialog::selectedId() const {
    const QListWidgetItem *item = m_pages->currentItem();
    return item && item->isSelected() ? item->data(kIdRole).toString() : QString();
}

QString NotionExportDialog::selectedTitle() const {
    const QListWidgetItem *item = m_pages->currentItem();
    return item ? item->data(kTitleRole).toString() : QString();
}

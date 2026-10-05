#pragma once

#include "CanvasBoard.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointF>
#include <QSizeF>
#include <QString>
#include <QStringList>

// Pemilik state satu kanvas brainstorm (satu project): satu-satunya tempat kartu dan garis berubah.
// View hanya mengirim intent; intent yang ditolak tidak mengubah apa pun, dan setiap intent yang
// berhasil adalah satu langkah undo. Hasil langkah AI dan salinan isi referensi bukan keputusan
// pengguna: tidak masuk riwayat undo dan tidak ikut terbalik saat undo/redo.
class CanvasModel : public QObject {
    Q_OBJECT

public:
    explicit CanvasModel(const QString &projectId, QObject *parent = nullptr);

    QString projectId() const { return m_projectId; }
    const CanvasBoard &board() const { return m_board; }
    const CanvasNode *node(const QString &id) const { return m_board.node(id); }

    // Isi dari disk: riwayat undo dikosongkan dan tidak dianggap perubahan (changed tidak dipancarkan)
    void load(const CanvasBoard &board);

    QString addNote(const QPointF &pos, const QString &text = QString(), const QString &color = QString());
    QString addStep(const QPointF &pos, const QString &instruction = QString(),
                    CanvasStepOutput output = CanvasStepOutput::Document);
    // title / detail / text: isi sumber saat ini, lihat CanvasWorkflow::resolve
    QString addReference(const CanvasSource &source, const QPointF &pos, const QString &title,
                         const QString &detail, const QString &text, bool available = true);
    bool moveNodes(const QHash<QString, QPointF> &positions);
    bool resizeNode(const QString &id, const QSizeF &size);
    // Catatan & langkah AI saja. grow: ukuran yang dibutuhkan isinya; kartu hanya membesar, dalam
    // langkah undo yang sama dengan teksnya
    bool setText(const QString &id, const QString &text, const QSizeF &grow = QSizeF());
    bool setColor(const QString &id, const QString &color);
    bool setStepOptions(const QString &id, const QString &model, const QString &effort, CanvasStepOutput output);
    bool removeNodes(const QStringList &ids);   // garis yang menempel ikut terhapus
    // Id garis baru; kosong + *reason bila ditolak (kartu tidak ada, ke dirinya sendiri, sudah
    // tersambung, atau membuat putaran)
    QString connectNodes(const QString &from, const QString &to, QString *reason = nullptr);
    bool removeEdges(const QStringList &ids);
    // Salinan kartu beserta garis di antara kartu-kartu itu, digeser offset. Hasil langkah AI tidak
    // ikut disalin. Id baru urut sesuai ids.
    QStringList duplicateNodes(const QStringList &ids, const QPointF &offset);

    // Beberapa intent sebagai satu langkah undo (mis. task usulan beserta garisnya)
    void beginMacro();
    void endMacro();

    // Bukan intent pengguna: tidak masuk riwayat undo
    bool setStepResult(const QString &id, const AgentResult &result, const QDateTime &finishedAt);
    bool refreshReference(const QString &id, const QString &title, const QString &detail, const QString &text,
                          bool available);
    // Posisi tampilan ikut tersimpan pada penulisan berikutnya, tanpa memicu penulisan sendiri
    void setView(const QPointF &center, qreal zoom);

    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    bool undo();
    bool redo();

signals:
    void nodeAdded(const CanvasNode &node);
    void nodeChanged(const CanvasNode &node);
    void nodeRemoved(const QString &id);
    void edgeAdded(const CanvasEdge &edge);
    void edgeRemoved(const QString &id);
    // Isi diganti seluruhnya (load, undo, redo): tampilan dibangun ulang dari board()
    void boardReset();
    // Ada perubahan yang perlu disimpan
    void changed();
    void undoStateChanged();

private:
    QString insert(const CanvasNode &node);
    void record();
    void restore(const CanvasBoard &snapshot);

    QString m_projectId;
    CanvasBoard m_board;
    QList<CanvasBoard> m_undo;
    QList<CanvasBoard> m_redo;
    int m_macroDepth = 0;
    bool m_macroRecorded = false;
};

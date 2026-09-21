#ifndef CONSOLEPANELWIDGET_H
#define CONSOLEPANELWIDGET_H

#include <QWidget>

namespace Ui {
class ConsolePanelWidget;
}

class ConsolePanelWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ConsolePanelWidget(QWidget *parent = nullptr);
    ~ConsolePanelWidget();

    void appendLog(const QString &log);

signals:
    void commandSubmitted(const QString &command);

private slots:
    void onInputSubmitted();

private:
    Ui::ConsolePanelWidget *ui;
};

#endif // CONSOLEPANELWIDGET_H

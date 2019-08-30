#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include "candev.h"
#include "qdebugstream.h"


namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = 0);
    ~MainWindow();

private slots:
    void on_actionOpen_triggered();

    void on_actionLoad_bin_triggered();

    void on_terminal_textChanged();

private:
    Ui::MainWindow *ui;

    virtual void timerEvent(QTimerEvent *e);

    Q_DebugStream *terminalStream;

    static const int stepMs;
    static const int terminalMaxLen;

    int timerId;
};

#endif // MAINWINDOW_H

#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "opendialog.h"
#include "loadbindialog.h"


const int MainWindow::stepMs = 10;

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    ui->splitter->setStretchFactor(0, 0);
    ui->splitter->setStretchFactor(1, 1);
    this->terminalStream = new Q_DebugStream(std::cout, ui->terminal);
    this->timerId = this->startTimer(this->stepMs);
}

MainWindow::~MainWindow()
{
    delete ui;
    delete this->terminalStream;
}


void MainWindow::on_actionOpen_triggered()
{
    OpenDialog d;
    int ret = d.exec();
    if (ret) {
        CanDev::instance()->open(d.getBaudrate(), d.getDev());
    }
}

void MainWindow::timerEvent(QTimerEvent *e)
{
    if (e) {

    }

    CanDev::CanMsg_st msg;
    while (CanDev::instance()->receive(msg)) {
        ui->widget->canReceive(msg);
    }
}

void MainWindow::on_actionLoad_bin_triggered()
{
    this->killTimer(this->timerId);
    LoadBinDialog d;
    d.exec();
    this->timerId = this->startTimer(this->stepMs);
}

#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "opendialog.h"

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    ui->splitter->setStretchFactor(0, 0);
    ui->splitter->setStretchFactor(1, 1);
    this->terminalStream = new Q_DebugStream(std::cout, ui->terminal);
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

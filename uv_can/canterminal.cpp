#include "canterminal.h"
#include "ui_can_terminal.h"

CanTerminal::CanTerminal(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::can_terminal)
{
    ui->setupUi(this);
}

CanTerminal::~CanTerminal()
{
    delete ui;
}

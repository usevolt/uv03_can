/*
 * ui.c
 *
 *  Created on: Jul 7, 2016
 *      Author: usevolt
 */


#include "ui.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>


pthread_mutex_t ui_mutex = PTHREAD_MUTEX_INITIALIZER;



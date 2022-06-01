
#include <stdio.h>
#include <stdlib.h>
#include <readline/readline.h>
#include <readline/history.h>

#include "Readline.h"

using namespace cnoid;

readlineAdaptor::readlineAdaptor(QObject *parent)
    : QObject(parent) {

}

bool readlineAdaptor::startThread() {
    rl_future = QtConcurrent::run(this, &readlineAdaptor::readlineProc);
    return rl_future.isStarted();
}

static bool do_terminate;
static int check_state() {
    if (do_terminate) {
        rl_done = 1;
    }
    return 0;
}

void readlineAdaptor::readlineProc() {
    // for killing by Ctrl-C
    rl_catch_signals = 0;
    rl_clear_signals();
    rl_event_hook = check_state;
    do_terminate = false;

    char* comm;
    while ((comm = readline("")) != nullptr && !do_terminate) {
        if (strlen(comm) > 0) {
            add_history(comm);
            {
                QString qmsg(comm);
                Q_EMIT sendRequest(qmsg);
            }
        } else {
            // just input return
            {
                QString qmsg("\n");
                Q_EMIT sendRequest(qmsg);
            }
        }
        // free buffer
        free(comm);
    }
}

void readlineAdaptor::setTerminate() {
    do_terminate = true;
}

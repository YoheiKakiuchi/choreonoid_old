#ifndef CNOID_READLINE_H
#define CNOID_READLINE_H

#include <QObject>
#include <QtConcurrent>
#include "exportdecl.h"

namespace cnoid {

    class CNOID_EXPORT readlineAdaptor : public QObject
    {
        Q_OBJECT;

    public:
        explicit readlineAdaptor(QObject *parent = nullptr);

    public:
        ~readlineAdaptor() { }

    public:
        bool startThread();
        void readlineProc();
        void setTerminate();

    Q_SIGNALS:
        void sendRequest(const QString &msg);

    private:
        QFuture<void> rl_future;
    };

}

#endif // CNOID_READLINE_H

/*
    Found in StackOverflow when looking for alternatives to QtSingleApplication
    https://stackoverflow.com/questions/5006547/qt-best-practice-for-a-single-instance-app-protection/
*/

#ifndef RUNGUARD_H
#define RUNGUARD_H

#include <QObject>
#include <QSharedMemory>
#include <QSystemSemaphore>

class RunGuard{
    public:
        RunGuard(const QString& key);
        ~RunGuard();

        bool isAnotherRunning();
        bool tryToRun();
        void release();

    private:
        const QString key;
        const QString memLockKey;
        const QString sharedmemKey;

        QSharedMemory sharedMem;
        QSystemSemaphore memLock;

        Q_DISABLE_COPY(RunGuard)
};

#endif //RUNGUARD_H
#include <QtCore/qmap.h>
#include <QtCore/qeventloop.h>
#include <QtCore/qthread.h>
#include <QtCore/qsocketnotifier.h>
#include <QtCore/qdebug.h>
#include <QtCore/qtimer.h>
#include <QtCore/qpointer.h>
#include <QtCore/qcoreevent.h>

#include "../include/eventloop.h"

QTNETWORKNG_NAMESPACE_BEGIN

struct QtWatcher
{
    virtual ~QtWatcher();
};

QtWatcher::~QtWatcher() {}

struct IoWatcher: public QtWatcher
{
    IoWatcher(qintptr fd, EventLoopCoroutine::EventType event, Functor *callback);
    virtual ~IoWatcher();

    EventLoopCoroutine::EventType event;
    QSocketNotifier read;
    QSocketNotifier write;
    Functor *callback;
    qintptr fd;
};

IoWatcher::IoWatcher(qintptr fd, EventLoopCoroutine::EventType event, Functor *callback)
    :event(event), read(fd, QSocketNotifier::Read), write(fd, QSocketNotifier::Write), callback(callback), fd(fd)
{
    read.setEnabled(false);
    write.setEnabled(false);
}

IoWatcher::~IoWatcher()
{
    delete callback;
}

struct TimerWatcher: public QtWatcher
{
    TimerWatcher(int interval, bool singleshot, Functor *callback);
    virtual ~TimerWatcher();

    int timerId;
    int interval;
    bool singleshot;
    Functor *callback;
};

TimerWatcher::TimerWatcher(int interval, bool singleshot, Functor *callback)
    :interval(interval), singleshot(singleshot), callback(callback)
{
}

TimerWatcher::~TimerWatcher()
{
    if(callback) {
        delete callback;
    }
}

class EventLoopCoroutinePrivateQt: public QObject, EventLoopCoroutinePrivate
{
    Q_OBJECT
public:
    EventLoopCoroutinePrivateQt(EventLoopCoroutine* q);
    virtual ~EventLoopCoroutinePrivateQt();
public:
    virtual void run() override;
    virtual int createWatcher(EventLoopCoroutine::EventType event, qintptr fd, Functor *callback) override;
    virtual void startWatcher(int watcherId) override;
    virtual void stopWatcher(int watcherId) override;
    virtual void removeWatcher(int watcherId) override;
    virtual void triggerIoWatchers(qintptr fd) override;
    virtual int callLater(int msecs, Functor *callback) override;
    virtual void callLaterThreadSafe(int msecs, Functor *callback) override;
    virtual int callRepeat(int msecs, Functor *callback) override;
    virtual void cancelCall(int callbackId) override;
    virtual int exitCode() override;
    virtual void runUntil(BaseCoroutine *coroutine) override;
private slots:
    void callLaterThreadSafeStub(int msecs, void* callback)
    {
        callLater(msecs, reinterpret_cast<Functor*>(callback));
    }
protected:
    virtual void timerEvent(QTimerEvent *event);
private slots:
    void handleIoEvent(int socket);
private:
    QEventLoop *loop;
    QMap<int, QtWatcher*> watchers;
    QMap<int, int> timers;
    int nextWatcherId;
    int qtExitCode;
    Q_DECLARE_PUBLIC(EventLoopCoroutine)
    friend struct TriggerIoWatchersArgumentsFunctor;
};

EventLoopCoroutinePrivateQt::EventLoopCoroutinePrivateQt(EventLoopCoroutine *q)
    :EventLoopCoroutinePrivate(q), loop(new QEventLoop()), nextWatcherId(1)
{
}

EventLoopCoroutinePrivateQt::~EventLoopCoroutinePrivateQt()
{
    foreach(QtWatcher *watcher, watchers) {
        delete watcher;
    }
    if(loop->isRunning()) {
        qWarning("deleting running qt eventloop.");
        loop->quit();
    }
    delete loop;
//    if(loop) {
//        loop->quit(); // XXX ::run() may be in other coroutine;
//    }
}

void EventLoopCoroutinePrivateQt::run()
{
    QPointer<EventLoopCoroutinePrivateQt> self(this);
    int result;

    // XXX loop is deleted in other coroutine, `this` pointer is invalid.
    // very bad taste! I don't like this.
    volatile QEventLoop *localLoop = loop;
    result = ((QEventLoop*)localLoop)->exec();
//    delete localLoop;

    if(!self.isNull()) {
        self->qtExitCode = result;
    }
}

void EventLoopCoroutinePrivateQt::handleIoEvent(int socket)
{
    Q_UNUSED(socket)

    QSocketNotifier *n = dynamic_cast<QSocketNotifier*>(sender());
    if(!n) {
        qDebug() << "can not retrieve sender() while handling qt io event.";
        return;
    }

    IoWatcher *w = 0;
    if(n->type() == QSocketNotifier::Read) {
        w = reinterpret_cast<IoWatcher*>(reinterpret_cast<char*>(n) - offsetof(IoWatcher, read));
    } else if (n->type() == QSocketNotifier::Write) {
        w = reinterpret_cast<IoWatcher*>(reinterpret_cast<char*>(n) - offsetof(IoWatcher, write));
    } else {
        qDebug() << "unknown QSocketNotifier type.";
        return;
    }

    (*w->callback)();
}

int EventLoopCoroutinePrivateQt::createWatcher(EventLoopCoroutine::EventType event, qintptr fd, Functor *callback)
{
    IoWatcher *w = new IoWatcher(fd, event, callback);

    connect(&w->read, SIGNAL(activated(int)), SLOT(handleIoEvent(int)), Qt::DirectConnection);
    connect(&w->write, SIGNAL(activated(int)), SLOT(handleIoEvent(int)), Qt::DirectConnection);
    watchers.insert(nextWatcherId, w);
    return nextWatcherId++;
}

void EventLoopCoroutinePrivateQt::startWatcher(int watcherId)
{
    IoWatcher *w = dynamic_cast<IoWatcher*>(watchers.value(watcherId));
    if(w) {
        if(w->event & EventLoopCoroutine::Read) {
            w->read.setEnabled(true);
        }
        if(w->event & EventLoopCoroutine::Write) {
            w->write.setEnabled(true);
        }
    }
}

void EventLoopCoroutinePrivateQt::stopWatcher(int watcherId)
{
    IoWatcher *w = dynamic_cast<IoWatcher*>(watchers.value(watcherId));
    if(w) {
        w->read.setEnabled(false);
        w->write.setEnabled(false);
    }
}

void EventLoopCoroutinePrivateQt::removeWatcher(int watcherId)
{
    IoWatcher *w = dynamic_cast<IoWatcher*>(watchers.take(watcherId));
    if(w) {
        w->read.setEnabled(false);
        w->write.setEnabled(false);
        delete w;
    }
}

struct TriggerIoWatchersArgumentsFunctor: public Functor
{
    TriggerIoWatchersArgumentsFunctor(int watcherId, EventLoopCoroutinePrivateQt *eventloop)
        :watcherId(watcherId), eventloop(eventloop) {}
    int watcherId;
    QPointer<EventLoopCoroutinePrivateQt> eventloop;
    virtual void operator() () override
    {
        if(eventloop.isNull()) {
            qWarning("triggerIoWatchers() is called without eventloop.");
            return;
        }
        IoWatcher *w = dynamic_cast<IoWatcher*>(eventloop->watchers.value(watcherId));
        if(w) {
            (*w->callback)();
        }
    }
};


void EventLoopCoroutinePrivateQt::triggerIoWatchers(qintptr fd)
{
    for(QMap<int, QtWatcher*>::const_iterator itor = watchers.constBegin(); itor != watchers.constEnd(); ++itor) {
        IoWatcher *w = dynamic_cast<IoWatcher*>(itor.value());
        if(w && w->fd == fd) {
            w->read.setEnabled(false);
            w->write.setEnabled(false);
            callLater(0, new TriggerIoWatchersArgumentsFunctor(itor.key(), this));
        }
    }
}

void EventLoopCoroutinePrivateQt::timerEvent(QTimerEvent *event)
{
    if(!timers.contains(event->timerId())) {
        return;
    }

    int watcherId = timers.value(event->timerId());
    TimerWatcher *watcher = dynamic_cast<TimerWatcher*>(watchers.value(watcherId));

    if(!watcher) {
        return;
    }

    bool singleshot = watcher->singleshot;
    (*watcher->callback)();
    if(singleshot) {
        // watcher may be deleted!
        if(watchers.contains(watcherId)) {
            watchers.remove(watcherId);
            timers.remove(event->timerId());
            killTimer(event->timerId());
            delete watcher;
        }
    } else {
        //watcher->timerId = startTimer(watcher->interval);
    }
}


int EventLoopCoroutinePrivateQt::callLater(int msecs, Functor *callback)
{
    TimerWatcher *w = new TimerWatcher(msecs, true, callback);
    w->timerId = startTimer(msecs, Qt::CoarseTimer);
    watchers.insert(nextWatcherId, w);
    timers.insert(w->timerId, nextWatcherId);
    return nextWatcherId++;
}

void EventLoopCoroutinePrivateQt::callLaterThreadSafe(int msecs, Functor *callback)
{
    QMetaObject::invokeMethod(this, "callLaterThreadSafeStub", Qt::QueuedConnection, Q_ARG(int, msecs), Q_ARG(void*, callback));
}

int EventLoopCoroutinePrivateQt::callRepeat(int msecs, Functor *callback)
{
    TimerWatcher *w = new TimerWatcher(msecs, false, callback);
    w->timerId = startTimer(msecs);
    watchers.insert(nextWatcherId, w);
    timers.insert(w->timerId, nextWatcherId);
    return nextWatcherId++;
}

void EventLoopCoroutinePrivateQt::cancelCall(int callbackId)
{
    TimerWatcher *w = dynamic_cast<TimerWatcher*>(watchers.take(callbackId));
    if(w) {
        timers.remove(w->timerId);
        killTimer(w->timerId);
        delete w;
    }
}

int EventLoopCoroutinePrivateQt::exitCode()
{
    return qtExitCode;
}


void EventLoopCoroutinePrivateQt::runUntil(BaseCoroutine *coroutine)
{
    QSharedPointer<QEventLoop> sub(new QEventLoop());
    QPointer<BaseCoroutine> loopCoroutine = BaseCoroutine::current();
    std::function<BaseCoroutine*(BaseCoroutine*)> shutdown = [this, sub, loopCoroutine] (BaseCoroutine *arg) -> BaseCoroutine * {
        sub->exit();
        if(!loopCoroutine.isNull()) {
            loopCoroutine->yield();
        }
        return arg;
    };
    coroutine->finished.addCallback(shutdown);
    sub->exec();
}

EventLoopCoroutine::EventLoopCoroutine()
    :BaseCoroutine(BaseCoroutine::current()), d_ptr(new EventLoopCoroutinePrivateQt(this))
{

}

QTNETWORKNG_NAMESPACE_END

#include "eventloop_qt.moc"

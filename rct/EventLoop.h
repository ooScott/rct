#ifndef EVENTLOOP_H // -*- mode:c++ -*-
#define EVENTLOOP_H

#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>
#include <queue>
#include <set>
#include <unordered_set>
#include <vector>
#include "Apply.h"
#include "rct-config.h"
#if defined(HAVE_EPOLL)
#  include <sys/epoll.h>
#elif defined(HAVE_KQUEUE)
#  include <sys/types.h>
#  include <sys/event.h>
#  include <sys/time.h>
#elif defined(HAVE_SELECT)
#  include <sys/select.h>
#endif

class Event
{
public:
    virtual ~Event() { }
    virtual void exec() = 0;
};

template<typename Object, typename... Args>
class SignalEvent : public Event
{
public:
    enum MoveType { Move };

    SignalEvent(Object& o, Args&&... a)
        : obj(o), args(a...)
    {
    }
    SignalEvent(Object&& o, Args&&... a)
        : obj(o), args(a...)
    {
    }
    SignalEvent(Object& o, MoveType, Args&&... a)
        : obj(o), args(std::move(a) ...)
    {
    }
    SignalEvent(Object&& o, MoveType, Args&&... a)
        : obj(o), args(std::move(a) ...)
    {
    }

    void exec() { applyMove(obj, args); }

private:
    Object obj;
    std::tuple<Args...> args;
};

template<typename T>
class DeleteLaterEvent : public Event
{
public:
    DeleteLaterEvent(T* d)
        : del(d)
    {
    }

    void exec() { delete del; }

private:
    T* del;
};

class EventLoop : public std::enable_shared_from_this<EventLoop>
{
public:
    typedef std::shared_ptr<EventLoop> SharedPtr;
    typedef std::weak_ptr<EventLoop> WeakPtr;

    EventLoop();
    ~EventLoop();

    enum Flag {
        None = 0x0,
        MainEventLoop = 0x1,
        EnableSigIntHandler = 0x2,
        EnableSigTermHandler = 0x4
    };
    enum PostType {
        Move = 1,
        Async
    };

    void init(unsigned int flags = None);

    unsigned int flags() const { return flgs; }

    template<typename T>
    static void deleteLater(T* del)
    {
        if (EventLoop::SharedPtr loop = eventLoop()) {
            loop->post(new DeleteLaterEvent<T>(del));
        } else {
            error("No event loop!");
        }
    }
    template<typename Object, typename... Args>
    void post(Object& object, Args&&... args)
    {
        post(new SignalEvent<Object, Args...>(object, std::forward<Args>(args)...));
    }
    template<typename Object, typename... Args>
    void postMove(Object& object, Args&&... args)
    {
        post(new SignalEvent<Object, Args...>(object, SignalEvent<Object, Args...>::Move, std::forward<Args>(args)...));
    }
    template<typename Object, typename... Args>
    void callLater(Object&& object, Args&&... args)
    {
        post(new SignalEvent<Object, Args...>(std::forward<Object>(object), std::forward<Args>(args)...));
    }
    template<typename Object, typename... Args>
    void callLaterMove(Object&& object, Args&&... args)
    {
        post(new SignalEvent<Object, Args...>(std::forward<Object>(object), SignalEvent<Object, Args...>::Move, std::forward<Args>(args)...));
    }
    void post(Event* event);
    void wakeup();

    enum Mode {
        SocketRead = 0x1,
        SocketWrite = 0x2,
        SocketOneShot = 0x4,
        SocketError = 0x8,
        SocketLevelTriggered = 0x10
    };
    bool registerSocket(int fd, unsigned int mode, std::function<void(int, unsigned int)>&& func);
    bool updateSocket(int fd, unsigned int mode);
    void unregisterSocket(int fd);
    unsigned int processSocket(int fd, int timeout = -1);

    // See Timer.h for the flags
    int registerTimer(std::function<void(int)>&& func, int timeout, unsigned int flags = 0);
    void unregisterTimer(int id);

    // Changes to the inactivity timeout while the loop is running may
    // not be honoured.
    void setInactivityTimeout(int timeout);

    enum { Success = 0x100, GeneralError = 0x200, Timeout = 0x400 };
    unsigned int exec(int timeout = -1);
    void quit();

    //bool isRunning() const { std::lock_guard<std::mutex> locker(mutex); return !mExecStack.empty(); }

    static EventLoop::SharedPtr mainEventLoop() { std::lock_guard<std::mutex> locker(mainMutex); return mainLoop.lock(); }
    static EventLoop::SharedPtr eventLoop();

    static bool isMainThread() { return EventLoop::mainEventLoop() && std::this_thread::get_id() == EventLoop::mainEventLoop()->threadId; }
private:
#if defined(HAVE_EPOLL)
    typedef epoll_event NativeEvent;
#elif defined(HAVE_KQUEUE)
    typedef struct kevent NativeEvent;
#elif defined(HAVE_SELECT)
    struct NativeEvent
    {
        fd_set* rdfd;
        fd_set* wrfd;
    };
#endif

    void clearTimer(int id);
    bool sendPostedEvents();
    bool sendTimers();
    void cleanup();
    unsigned int processSocketEvents(NativeEvent* events, int eventCount);
    unsigned int fireSocket(int fd, unsigned int mode);

    static void error(const char* err);

private:
    static std::mutex mainMutex;
    mutable std::mutex mutex;
    std::thread::id threadId;

    std::queue<Event*> events;
    int eventPipe[2];
#if defined(HAVE_EPOLL) || defined(HAVE_KQUEUE)
    int pollFd;
#endif

    std::map<int, std::pair<unsigned int, std::function<void(int, unsigned int)> > > sockets;

    class TimerData
    {
    public:
        TimerData() { }
        TimerData(uint64_t w, int i, unsigned int f, int in, std::function<void(int)>&& cb)
            : when(w), id(i), flags(f), interval(in), callback(std::move(cb))
        {
        }
        TimerData(TimerData&& other)
            : when(other.when), id(other.id), flags(other.flags),
              interval(other.interval), callback(std::move(other.callback))
        {
        }
        TimerData& operator=(TimerData&& other)
        {
            when = other.when;
            id = other.id;
            flags = other.flags;
            interval = other.interval;
            callback = std::move(other.callback);
            return *this;
        }

        bool operator<(const TimerData& other) const
        {
            return when < other.when;
        }

        uint64_t when;
        uint32_t id;
        unsigned int flags;
        int interval;
        std::function<void(int)> callback;

    private:
        TimerData(const TimerData& other) = delete;
        TimerData& operator=(const TimerData& other) = delete;
    };

    struct TimerDataSet
    {
        bool operator()(TimerData* a, TimerData* b) const { return a->when < b->when; }
    };
    struct TimerDataHash
    {
        // two operators for the price of one!
        size_t operator()(TimerData* a) const { return a->id; }
        bool operator()(TimerData* a, TimerData* b) const { return a->id == b->id; }
    };
    typedef std::multiset<TimerData*, TimerDataSet> TimersByTime;
    typedef std::unordered_set<TimerData*, TimerDataHash, TimerDataHash> TimersById;
    TimersByTime timersByTime;
    TimersById timersById;
    uint32_t nextTimerId;

    bool stop;
    bool timeout;

    static EventLoop::WeakPtr mainLoop;

    unsigned int flgs;

    int inactivityTimeout;
private:
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;
};

#endif

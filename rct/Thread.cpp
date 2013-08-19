#include "rct/Thread.h"
#include "rct/EventLoop.h"

Thread::Thread()
    : mAutoDelete(false)
{
}

Thread::Thread(Thread&& other)
{
    mAutoDelete = other.mAutoDelete;
    other.mAutoDelete = false;
    mThread = std::move(other.mThread);
}

Thread::~Thread()
{
}

void Thread::start()
{
    mThread = std::thread([=]() {
            run(); 
            if (isAutoDelete())
                EventLoop::mainEventLoop()->callLater(std::bind(&Thread::finish, this));
        });
}

bool Thread::join()
{
    mThread.join();
    return true;
}

void Thread::finish()
{
    join();
    delete this;
}

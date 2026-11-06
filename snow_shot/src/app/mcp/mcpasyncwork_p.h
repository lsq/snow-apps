#pragma once

#include "snow_shot/platform/applicationqos.h"
#include "snow_shot/runtime/runtimeactivitytracker.h"

#include <QFuture>
#include <QFutureWatcher>
#include <QMetaObject>
#include "snow_shot/compat/qt_compat.h"
#include <QThreadPool>
#include <exception>
#include <memory>
#include <type_traits>
#include <utility>

namespace snow_shot::app::mcp {
// Reserve queued work before submission, including work that survives request cancellation.
template <typename Work> bool queueMcpWork(QObject* receiver, Work work) {
    return QMetaObject::invokeMethod(receiver, runtime::trackRuntimeWork(std::move(work)),
                                     Qt::QueuedConnection);
}

// Owns a QObject child that only exists to outlive one result read.
class McpWorkKeeper final : public QObject {
  public:
    explicit McpWorkKeeper(QObject* owner) : QObject(owner) {}
    std::shared_ptr<void> promise;
};

// A future plus the promise that owns its stored results.
//
// Qt 6's QPromise shares its results with the future, so the promise may die as
// soon as the work returns. Qt 5's SnowPromise wraps a QFutureInterface, and
// QFutureInterface::~QFutureInterface() clears the stored results as soon as the
// last interface object goes away -- the future shares the state, not the
// results. A QFutureWatcher reads the result when `finished` arrives, which is
// after the worker's copy of the promise is gone, so on Qt 5 every background
// request answered from an already cleared store: `future().resultCount()` was
// 0 and `QFutureWatcher::result()` indexed into nothing (the configuration
// export fixture segfaulted there). Handing the promise to the watcher keeps the
// results alive for exactly as long as the object that reads them.
template <typename Result> class McpWork {
  public:
    McpWork(QFuture<Result> future, std::shared_ptr<SnowPromise<Result>> promise)
        : m_future(std::move(future)), m_promise(std::move(promise)) {}

    void setFutureOn(QFutureWatcher<Result>* watcher) {
        auto* keeper = new McpWorkKeeper(watcher);
        keeper->promise = m_promise;
        m_promise.reset();
        watcher->setFuture(m_future);
    }

  private:
    QFuture<Result> m_future;
    std::shared_ptr<SnowPromise<Result>> m_promise;
};

// Keep bounded application work on the caller's pool. QtCore supplies promises
// and completion signals even in the application's minimal static Qt build.
template <typename Work>
McpWork<std::invoke_result_t<Work>> runMcpWork(QThreadPool* pool, Work work) {
    using Result = std::invoke_result_t<Work>;
    auto promise = std::make_shared<SnowPromise<Result>>();
    auto future = promise->future();
    promise->start();
    pool->start(snow_shot::runtime::trackRuntimeWork(
        [promise, work = std::move(work)]() mutable {
            platform::applyApplicationQoSToCurrentThread();
            try {
                if (!promise->isCanceled()) {
                    if constexpr (std::is_void_v<Result>)
                        work();
                    else
                        promise->addResult(work());
                }
            } catch (...) {
                promise->setException(std::current_exception());
            }
            promise->finish();
            // The watcher's keeper is the only owner that has to survive from
            // here: it outlives the read of the result on the caller's thread.
            promise.reset();
        }));
    return McpWork<Result>(future, std::move(promise));
}
} // namespace snow_shot::app::mcp

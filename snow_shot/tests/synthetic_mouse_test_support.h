#ifndef SNOW_SHOT_TESTS_SYNTHETIC_MOUSE_TEST_SUPPORT_H
#define SNOW_SHOT_TESTS_SYNTHETIC_MOUSE_TEST_SUPPORT_H

#include <QDateTime>
#include <QEvent>
#include <QPointF>
#include <QWindow>
#include <qpa/qwindowsysteminterface.h>

#include <cmath>

// Qt 5.15 has no timestamp-free QWindowSystemInterface::handleMouseEvent
// template: the argument after the window is `ulong timestamp` there, so the
// Qt 6 spelling (window, local, global, state, button, type) finds no candidate
// at all. The timestamp only orders deliveries inside the platform queue, and
// the routing these tests exercise keys off the arguments, so "now" is a
// faithful stand-in for it.
template <typename Delivery>
bool sendSyntheticMouse(QWindow* window, const QPointF& local, const QPointF& global,
                        Qt::MouseButtons state, Qt::MouseButton button, QEvent::Type type) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return QWindowSystemInterface::handleMouseEvent<Delivery>(window, local, global, state, button,
                                                              type);
#else
    return QWindowSystemInterface::handleMouseEvent<Delivery>(
        window, static_cast<ulong>(QDateTime::currentMSecsSinceEpoch()), local, global, state,
        button, type);
#endif
}

// Qt 5 hands a window-level synthetic move to the widget through
// QWidgetWindow::handleMouseEvent, which rebuilds the widget-local position from
// QMouseEvent::globalPos() -- an integer QPoint there. A half-pixel step (the
// Retina-style "one physical pixel" step at DPR 2) therefore arrives rounded,
// and both the local and the global the widget observes are whole numbers. Qt 6
// carries the fractional value end to end. Fixtures that assert on a delivered
// position fold the requested one through this helper so they state the contract
// the running Qt version actually has.
[[nodiscard]] inline QPointF deliveredMousePosition(const QPointF& requested) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return requested;
#else
    return QPointF(std::round(requested.x()), std::round(requested.y()));
#endif
}

#endif // SNOW_SHOT_TESTS_SYNTHETIC_MOUSE_TEST_SUPPORT_H

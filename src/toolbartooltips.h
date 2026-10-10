#pragma once
#include <QAbstractButton>
#include <QCursor>
#include <QEvent>
#include <QHelpEvent>
#include <QToolTip>

class ToolbarTooltips : public QObject
{
public:
    explicit ToolbarTooltips(QWidget *toolbar) : QObject(toolbar)
    {
        for (auto button : toolbar->findChildren<QAbstractButton *>())
            button->installEventFilter(this);
    }
protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        auto button = static_cast<QAbstractButton *>(watched);
        if (event->type() == QEvent::Enter || event->type() == QEvent::ToolTip) {
            const QPoint point = event->type() == QEvent::ToolTip
                ? static_cast<QHelpEvent *>(event)->globalPos() : QCursor::pos();
            QToolTip::showText(point, button->toolTip(), button, button->rect());
            if (event->type() == QEvent::ToolTip) return true;
        } else if (event->type() == QEvent::Leave) {
            QToolTip::hideText();
        }
        return QObject::eventFilter(watched, event);
    }
};

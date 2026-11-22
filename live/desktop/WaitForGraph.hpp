#pragma once

#include "DeadlockDemo.hpp"

#include <QPainter>
#include <QPolygonF>
#include <QWidget>

#include <optional>
#include <utility>

class WaitForGraph : public QWidget {
    std::optional<bq::RecoveryDemoResult> result_;

public:
    explicit WaitForGraph(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(260);
        setAccessibleName("Wait-for graph");
    }

    void setCycle(std::optional<bq::RecoveryDemoResult> result) {
        result_ = std::move(result);
        setAccessibleDescription(result_ ? QString::fromStdString(result_->detection.lastCycle) : QString());
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), QColor("#fffdf9"));
        p.setPen(QColor("#322a23"));
        if (!result_) {
            p.drawText(rect(), Qt::AlignCenter, "Upload one deadlock file to capture the wait-for graph.");
            return;
        }

        const qreal mid = height() / 2.0;
        const qreal nodeWidth = qBound<qreal>(88, width() * .19, 152);
        const qreal margin = qMax<qreal>(18, (width() - 820.0) / 2);
        const QRectF first(margin, mid - 24, nodeWidth, 48);
        const QRectF second(width() - margin - nodeWidth, mid - 24, nodeWidth, 48);
        const qreal left = first.right(), right = second.left();

        QFont strong = font();
        strong.setBold(true);
        p.setFont(strong);
        p.drawText(QRectF(12, 8, width() - 24, 30), Qt::AlignCenter, "Detected cycle snapshot");
        for (const auto& edge : result_->detection.cycleEdges) {
            const bool forward = edge.waiter == result_->workers[0];
            const qreal y = mid + (forward ? -14 : 14);
            const qreal from = forward ? left : right;
            const qreal to = forward ? right : left;
            const qreal tail = to + (forward ? -10 : 10);
            const QColor color(edge.rejected ? "#aa382a" : "#4c5d43");
            QPen pen(color, 3);
            if (edge.rejected)
                pen.setStyle(Qt::DashLine);
            p.setPen(pen);
            p.drawLine(QPointF(from, y), QPointF(to, y));
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            QPolygonF arrow;
            arrow << QPointF(to, y) << QPointF(tail, y - 5) << QPointF(tail, y + 5);
            p.drawPolygon(arrow);
            p.setPen(color);
            p.setFont(font());
            const QString resource = QString::fromStdString(edge.resource);
            const QString label = edge.rejected ? "Rejected request for " + resource : "Waiting for " + resource;
            p.drawText(QRectF(left + 3, y + (forward ? -52 : 20), right - left - 6, 30),
                       Qt::AlignCenter | Qt::TextWordWrap, label);
        }
        p.setFont(strong);
        for (int i = 0; i < 2; ++i) {
            const auto node = i == 0 ? first : second;
            p.setPen(QColor("#b6aa9c"));
            p.setBrush(QColor("#f1e9dd"));
            p.drawRoundedRect(node, 8, 8);
            p.setPen(QColor("#322a23"));
            p.drawText(node, Qt::AlignCenter, QString("Worker %1").arg(i + 1));
        }
        p.setFont(font());
        p.drawText(QRectF(10, height() - 40, width() - 20, 30), Qt::AlignCenter,
                   "Arrows point from the requesting worker to the resource owner.");
    }
};

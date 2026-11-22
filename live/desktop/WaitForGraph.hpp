#pragma once

#include "DeadlockDemo.hpp"

#include <QPainter>
#include <QPolygonF>
#include <QWidget>

#include <optional>
#include <cmath>
#include <algorithm>
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

        const auto& edges = result_->detection.cycleEdges;
        if (edges.empty())
            return;
        const qreal centerX = width() / 2.0, centerY = height() / 2.0;
        const qreal radiusX = qMin<qreal>(width() / 2.0 - 58.0, 300.0);
        const qreal radiusY = qMin<qreal>(height() / 2.0 - 60.0, 170.0);
        const qreal nodeRadius = 26;
        std::vector<QPointF> nodes;
        for (std::size_t i = 0; i < edges.size(); ++i) {
            const auto angle = 6.283185307 * i / edges.size();
            nodes.emplace_back(centerX + radiusX * std::cos(angle), centerY + radiusY * std::sin(angle));
        }
        p.drawText(QRectF(12, 8, width() - 24, 30), Qt::AlignCenter,
                   result_->checksums.empty() ? "Active wait-for cycle" : "Captured wait-for cycle");
        for (std::size_t i = 0; i < edges.size(); ++i) {
            const auto& edge = edges[i];
            const QPointF delta = nodes[(i + 1) % nodes.size()] - nodes[i];
            const QPointF direction = delta / std::hypot(delta.x(), delta.y());
            const QPointF normal(-direction.y(), direction.x());
            const QPointF offset = nodes.size() == 2 ? normal * 12 : QPointF();
            const QPointF from = nodes[i] + direction * (nodeRadius - 4) + offset;
            const QPointF to = nodes[(i + 1) % nodes.size()] - direction * (nodeRadius - 4) + offset;
            const QPointF tail = to - direction * 12;
            const QColor color(edge.rejected ? "#aa382a" : "#775c32");
            QPen pen(color, 3, edge.rejected ? Qt::DashLine : Qt::SolidLine);
            p.setPen(pen);
            p.drawLine(from, to);
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            QPolygonF arrow;
            arrow << to << tail + normal * 5 << tail - normal * 5;
            p.drawPolygon(arrow);
            if (nodes.size() <= 4) {
                p.setPen(color);
                const auto label = (from + to) / 2 + normal * 15;
                const auto resource = QString::fromStdString(edge.resource);
                p.drawText(QRectF(label.x() - 90, label.y() - 10, 180, 20), Qt::AlignCenter,
                           p.fontMetrics().elidedText(resource, Qt::ElideRight, 178));
            }
        }
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const auto worker = std::find(result_->workers.begin(), result_->workers.end(), edges[i].waiter);
            const auto number = std::distance(result_->workers.begin(), worker) + 1;
            p.setPen(QColor("#b6aa9c"));
            p.setBrush(QColor("#f1e9dd"));
            p.drawEllipse(nodes[i], nodeRadius, nodeRadius);
            p.setPen(QColor("#322a23"));
            p.drawText(QRectF(nodes[i].x() - 25, nodes[i].y() - 25, 50, 50), Qt::AlignCenter,
                       QString("Job %1").arg(static_cast<int>(number)));
        }
    }
};

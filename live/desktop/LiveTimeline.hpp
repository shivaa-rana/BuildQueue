#pragma once

#include "BatchQueue.hpp"

#include <QColor>
#include <QFontMetrics>
#include <QPaintEvent>
#include <QPainter>
#include <QString>
#include <QWidget>

#include <algorithm>
#include <utility>

class LiveTimeline : public QWidget {
    using Job = bq::Job;
    using State = bq::State;
    using Clock = Job::Clock;
    static constexpr int rowHeight = 45;
    std::vector<Job> jobs_;
    Clock::time_point origin_, now_;
    double span_ = 1;

public:
    explicit LiveTimeline(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumWidth(650);
        setAccessibleName("Job execution timeline in first-dispatch order");
    }

    void setJobs(std::vector<Job> jobs, Clock::time_point origin, Clock::time_point now, double span) {
        std::stable_sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) {
            if (a.dispatchOrder && b.dispatchOrder)
                return a.dispatchOrder < b.dispatchOrder;
            if (bool(a.dispatchOrder) != bool(b.dispatchOrder))
                return a.dispatchOrder != 0;
            return a.id < b.id;
        });
        jobs_ = std::move(jobs);
        origin_ = origin;
        now_ = now;
        span_ = std::max(1.0, span);
        setMinimumHeight(std::max(130, 60 + rowHeight * static_cast<int>(jobs_.size())));
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(event->rect(), QColor("#fffdf9"));
        p.setPen(QColor("#322a23"));
        if (jobs_.empty()) {
            p.drawText(rect(), Qt::AlignCenter, "Add files to plot the batch.");
            return;
        }

        const int left = 252;
        const int right = width() - 48;
        const auto position = [&](Clock::time_point stamp) {
            const auto ms = std::chrono::duration<double, std::milli>(stamp - origin_).count();
            return left + (right - left) * std::clamp(ms / span_, 0.0, 1.0);
        };
        const bool seconds = span_ >= 1000;
        p.drawText(QRect(12, 6, left - 14, 26), Qt::AlignVCenter, "First dispatch / Job");
        for (int tick = 0; tick <= 5; ++tick) {
            const int x = left + (right - left) * tick / 5;
            p.setPen(QColor("#d7ccbe"));
            p.drawLine(x, 32, x, height());
            p.setPen(QColor("#4e4135"));
            p.drawText(QRect(x - 47, 5, 94, 25), Qt::AlignCenter,
                       QString::number(span_ * tick / (seconds ? 5000.0 : 5.0), 'f', seconds ? 2 : 1)
                       + (seconds ? " s" : " ms"));
        }

        const auto bars = [&](Clock::time_point start, Clock::time_point end, int y, const QColor& color) {
            const double x = position(start);
            const double w = std::min(double(right) - x, std::max(1.0, position(end) - x));
            if (end > start && w > 0)
                p.fillRect(QRectF(x, y, w, 17), color);
        };
        const QFontMetrics metrics(p.font());
        for (std::size_t i = 0; i < jobs_.size(); ++i) {
            const auto& job = jobs_[i];
            const int y = 38 + rowHeight * static_cast<int>(i);
            if (!event->rect().intersects(QRect(0, y, width(), rowHeight)))
                continue;
            const bool live = job.state == State::Running;
            const auto ordinal = job.dispatchOrder ? QString("#%1").arg(static_cast<qulonglong>(job.dispatchOrder))
                                                   : QString("-");
            const QString filename = QString::fromStdString(job.file);
            const auto shortName = filename.mid(filename.lastIndexOf('/') + 1);
            const QString state = live ? "Running" : job.state == State::Queued ? "Queued" :
                                  job.state == State::Completed ? "Completed" :
                                  job.state == State::Cancelled ? "Cancelled" : "Failed";
            p.setPen(QColor("#322a23"));
            p.drawText(QRect(12, y, left - 23, 20), Qt::AlignVCenter,
                       metrics.elidedText(ordinal + "  " + shortName, Qt::ElideMiddle, left - 23));
            const bool capped = job.timelineTruncated || (live && job.slices.size() >= 256);
            p.setPen(QColor("#5e5143"));
            p.drawText(QRect(12, y + 19, left - 23, 21), Qt::AlignVCenter,
                       capped ? state + " (timeline clipped)" : state);
            const auto end = capped ? job.slices.back().second :
                job.finished == Clock::time_point{} ? now_ : job.finished;
            bars(job.submitted, end, y + 11, QColor("#c58c51"));
            for (const auto& slice : job.slices)
                bars(slice.first, slice.second, y + 11, QColor("#536f4e"));
            if (live && !capped)
                bars(job.sliceStarted, now_, y + 11, QColor("#315c81"));
        }
    }
};

#include "BatchQueue.hpp"
#include "DeadlockDemo.hpp"
#include "LiveTimeline.hpp"
#include "WaitForGraph.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStringList>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <future>

namespace {
using namespace bq;
using Clock = Job::Clock;

enum Column { File, Status, Arrival, Run, Completion, Turnaround, Wait, Timeline,
              Size, Priority, Dispatch, Crc, Pid, Memory, Columns };
enum Role { JobId = Qt::UserRole, Start, Finish, Processing };

double milliseconds(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

QString duration(double ms) {
    if (ms < 1000)
        return QString::number(ms, 'f', 1) + " ms";
    return QString::number(ms / 1000, 'f', 2) + " s";
}

QString stateName(State state) {
    switch (state) {
        case State::Queued: return "Queued";
        case State::Running: return "Running";
        case State::Completed: return "Completed";
        case State::Failed: return "Failed";
        case State::Cancelled: return "Cancelled";
    }
    return {};
}

QString policyName(Policy policy) {
    switch (policy) {
        case Policy::Fcfs: return "FCFS";
        case Policy::Sjf: return "SJF";
        case Policy::Priority: return "Priority";
        case Policy::RoundRobin: return "Round Robin";
    }
    return {};
}

struct Timing {
    double offset;
    double waiting;
    double running;
    bool started;
};

Timing timing(const Job& job, Clock::time_point origin, Clock::time_point now) {
    const bool started = job.started != Clock::time_point{};
    return {milliseconds(job.submitted - origin),
            milliseconds(job.waitTime(now)), milliseconds(job.burstTime(now)), started};
}

QStringList fileLocations() {
    QStringList paths{QDir::rootPath(), QDir::homePath()};
    const QDir mounts("/mnt");
    if (!mounts.exists())
        return paths;
    paths.append(mounts.absolutePath());
    for (const auto& name : mounts.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (name.size() != 1 || !name.front().isLetter())
            continue;
        const auto drive = mounts.absoluteFilePath(name);
        paths.append(drive);
        const QDir profiles(drive + "/Users");
        if (!profiles.exists())
            continue;
        paths.append(profiles.absolutePath());
        const auto profileName = QDir(QDir::homePath()).dirName();
        const auto current = QDir::currentPath();
        for (const auto& profile : profiles.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const auto folder = profiles.absoluteFilePath(profile);
            if (profile.compare(profileName, Qt::CaseInsensitive) != 0 &&
                current != folder && !current.startsWith(folder + '/'))
                continue;
            paths.append(folder);
            for (const auto* name : {"Desktop", "Documents", "Downloads", "OneDrive"}) {
                const auto location = folder + '/' + name;
                if (QDir(location).exists())
                    paths.append(location);
            }
        }
    }
    paths.removeDuplicates();
    return paths;
}

class TimelineDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyleOptionViewItem background(option);
        initStyleOption(&background, index);
        background.text.clear();
        auto style = background.widget ? background.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &background, painter, background.widget);
        QRectF track = QRectF(option.rect).adjusted(9, 12, -9, -12);
        if (track.isEmpty())
            return;
        painter->save();
        painter->setClipRect(track);
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor("#eee7dc"));
        painter->drawRoundedRect(track, 4, 4);
        auto left = track.left() + track.width() * index.data(Start).toDouble();
        painter->setBrush(QColor("#c58c51"));
        painter->drawRoundedRect(QRectF(left, track.top(),
            track.width() * index.data(Finish).toDouble() - (left - track.left()), track.height()), 3, 3);
        painter->setBrush(QColor("#536f4e"));
        const auto intervals = index.data(Processing).toList();
        for (int i = 0; i + 1 < intervals.size(); i += 2) {
            const auto start = intervals[i].toDouble();
            const auto end = intervals[i + 1].toDouble();
            painter->drawRoundedRect(QRectF(track.left() + track.width() * start, track.top(),
                                           track.width() * (end - start), track.height()), 3, 3);
        }
        painter->restore();
    }
};

class PoolMap : public QWidget {
public:
    explicit PoolMap(QWidget* parent) : QWidget(parent) {
        setFixedHeight(14);
        setAccessibleName("Buffer pool block layout");
    }

    void setBlocks(const std::vector<BufferPool::Block>& blocks) {
        blocks_ = blocks;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        if (blocks_.empty())
            return;
        QPainter painter(this);
        const auto capacity = blocks_.back().offset + blocks_.back().size;
        const auto scale = static_cast<double>(width()) / capacity;
        painter.fillRect(rect(), QColor("#e6dfd2"));
        for (const auto& block : blocks_) {
            if (block.used)
                painter.fillRect(QRectF(block.offset * scale, 0, block.size * scale, height()),
                                 QColor("#536f4e"));
        }
    }

private:
    std::vector<BufferPool::Block> blocks_;
};

struct Comparison {
    Policy policy;
    std::vector<Job> jobs;
};

std::vector<Comparison> comparePolicies(const std::vector<JobRequest>& requests, int slots,
                                        int memoryMiB, Fit fit, const std::atomic<bool>* stopped) {
    BufferPool buffers;
    buffers.setFit(fit);
    std::vector<Comparison> results;
    for (auto policy : {Policy::Fcfs, Policy::Sjf, Policy::Priority, Policy::RoundRobin}) {
        if (stopped->load())
            return {};
        BatchQueue queue(slots, slots, [&](const std::string& path) {
            return Checksum::runPath(path, std::chrono::minutes(2), memoryMiB * bytesPerMiB, &buffers, stopped);
        }, Input::AbsolutePaths, [](const std::string& path) { return Checksum::pathSize(path); },
        [&](const std::string& path, ChecksumProgress& progress) {
            if (stopped->load()) {
                ChecksumResult interrupted;
                interrupted.exitCode = 125;
                return std::optional<ChecksumResult>(interrupted);
            }
            return Checksum::slicePath(path, progress, buffers);
        });
        queue.setPolicy(policy);
        // Submit all jobs together so every policy sees the same initial queue.
        queue.submit(requests);
        queue.waitUntilIdle();
        if (stopped->load())
            return {};
        auto jobs = queue.snapshot().jobs;
        std::sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) {
            return a.dispatchOrder < b.dispatchOrder;
        });
        results.push_back({policy, std::move(jobs)});
    }
    return results;
}

class Desktop : public QWidget {
    std::atomic<bool> stopped_{false};
    bool restartComparison_ = false;
    QString restartCase_;
    std::atomic<int> memoryMiB_{defaultMemoryMiB};
    BufferPool buffers_;
    Fit fit_ = Fit::First;
    BatchQueue queue_{4, 2, [this](const std::string& file) {
                          return Checksum::runPath(file, std::chrono::minutes(2),
                                                   memoryMiB_.load() * bytesPerMiB, &buffers_, &stopped_);
                      },
                      Input::AbsolutePaths, [](const std::string& file) { return Checksum::pathSize(file); },
                      [this](const std::string& file, ChecksumProgress& progress) {
                          return Checksum::slicePath(file, progress, buffers_);
                      }};
    QTableWidget* table_ = new QTableWidget(this);
    QTableWidget* comparisonTable_ = new QTableWidget(this);
    QTableWidget* memoryTable_ = new QTableWidget(this);
    QLabel* comparisonInfo_ = new QLabel("Finish a batch in Jobs to compare the four policies.", this);
    QLabel* queuedMetric_ = new QLabel(this);
    QLabel* completedMetric_ = new QLabel(this);
    QLabel* slotsMetric_ = new QLabel(this);
    QLabel* waitMetric_ = new QLabel(this);
    QStackedWidget* workspaces_ = new QStackedWidget(this);
    QStackedWidget* jobResults_ = new QStackedWidget(this);
    QLabel* solver_ = new QLabel(this);
    LiveTimeline* graph_ = new LiveTimeline(this);
    QPushButton* runRecovery_ = new QPushButton("Run bundled case", this);
    QPushButton* uploadLockJobs_ = new QPushButton("Upload deadlock file", this);
    QComboBox* demoCase_ = new QComboBox(this);
    WaitForGraph* waitGraph_ = new WaitForGraph(this);
    QLabel* recoveryResult_ = new QLabel("Upload one deadlock file to run its two live checksum jobs.", this);
    std::mutex cycleMutex_;
    std::optional<RecoveryDemoResult> activeCycle_;
    std::future<RecoveryDemoResult> recovery_;
    QCheckBox* waitForTimeout_ = new QCheckBox("Wait for timeout (10 s)", this);
    QString currentCase_;
    QPushButton* stopAll_ = new QPushButton("Stop all", this);
    QPushButton* resume_ = new QPushButton("Resume", this);
    QString lockJobName_;
    int timelinePage_ = -1;
    QLabel* timelineScale_ = new QLabel(this);
    QComboBox* timelinePolicy_ = new QComboBox(this);
    QPushButton* timelineReplay_ = new QPushButton("Rerun batch", this);
    QLabel* timelineRunPolicy_ = new QLabel(this);
    QLabel* policyHelp_ = new QLabel(this);
    QLabel* priorityLabel_ = new QLabel("Batch priority", this);
    QSpinBox* priority_ = new QSpinBox(this);
    QPushButton* replay_ = new QPushButton("Rerun batch", this);
    QPushButton* compare_ = new QPushButton("Compare all policies", this);
    QSpinBox* compareSlots_ = new QSpinBox(this);
    QLabel* poolInfo_ = new QLabel(this);
    PoolMap* poolMap_ = new PoolMap(this);
    std::future<std::vector<Comparison>> comparison_;
    QCheckBox* showEarlier_ = new QCheckBox("Show previous batch", this);
    QStatusBar* status_ = new QStatusBar(this);
    QFileDialog* picker_ = nullptr;
    std::uint64_t firstCurrentJob_ = 0;
    std::uint64_t firstPreviousJob_ = 0;

public:
    Desktop() {
        setWindowTitle("BuildQueue | Desktop checksum queue");
        setObjectName("dashboard");
        resize(1200, 740);
        setAcceptDrops(true);
        setStyleSheet(R"(
            QWidget#dashboard, QWidget[workspace="true"] { background: #f6f2eb; }
            QWidget#rail { background: #382e28; border-radius: 9px; }
            QWidget#rail QLabel { color: #ffffff; font-size: 20px; font-weight: 700; }
            QLabel, QCheckBox, QPushButton, QTableWidget, QHeaderView::section, QComboBox, QSpinBox { color: #352d26; font-size: 15px; }
            QWidget[panel="true"] { background: #ffffff; border: 1px solid #ddd2c3; border-radius: 8px; }
            QLabel#pageTitle { font-size: 25px; font-weight: 700; }
            QLabel#sectionTitle { font-size: 15px; font-weight: 700; }
            QWidget#queuedCard { border-left: 4px solid #c58c51; }
            QWidget#completedCard { border-left: 4px solid #536f4e; }
            QWidget#slotsCard { border-left: 4px solid #887352; }
            QWidget#waitCard { border-left: 4px solid #ad6049; }
            QLabel#metricLabel { font-weight: 600; }
            QLabel#metricValue { font-size: 23px; font-weight: 700; }
            QPushButton {
                padding: 7px 13px; color: #4f4034; background: #ffffff;
                border: 1px solid #d5c5b3; border-radius: 6px;
                font-weight: 600;
            }
            QPushButton:hover { background: #eee4d6; }
            QPushButton:disabled { color: #776a5d; background: #f1eae1; }
            QPushButton:focus { border-color: #887352; }
            QPushButton[policyChoice="true"]:checked, QPushButton#comparePolicies:enabled {
                color: white; background: #526447; border-color: #526447;
            }
            QPushButton#addFiles { background: #526447; color: #ffffff; border-color: #526447; padding: 9px 20px; }
            QPushButton#addFiles:hover { background: #42563b; }
            QWidget#rail QPushButton {
                background: transparent; color: #f0e5d8; text-align: left; font-size: 16px;
                padding: 9px 11px; border: 1px solid transparent;
            }
            QWidget#rail QPushButton:hover { background: #524135; }
            QWidget#rail QPushButton:checked { background: #615040; color: #ffffff; }
            QWidget#rail QPushButton:focus { border-color: #dfb783; }
            QSpinBox, QComboBox {
                color: #352d26; padding: 5px 8px; background: #fbf8f1;
                border: 1px solid #d5c5b3; border-radius: 6px;
            }
            QSpinBox:disabled, QLabel:disabled { color: #6e728a; }
            QTableWidget {
                color: #352d26; background: #ffffff; alternate-background-color: #faf6ef;
                border: 1px solid #ddd2c3; selection-background-color: #ecddc8;
                selection-color: #3d3026;
            }
            QTableWidget::item { padding: 0 7px; }
            QHeaderView::section {
                background: #ebe0d1; color: #544135; border: 0;
                border-bottom: 1px solid #ddd2c3; padding: 9px 7px; font-weight: 700;
            }
            QLabel#solver { color: #574638; background: #eee4d7; padding: 8px 12px; border-radius: 6px; }
            QLabel#hint { color: #6a5848; }
            QStatusBar { color: #574638; }
        )");
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(14, 14, 14, 12);
        layout->setSpacing(18);
        auto* rail = new QWidget(this);
        rail->setObjectName("rail");
        rail->setAttribute(Qt::WA_StyledBackground);
        rail->setFixedWidth(190);
        auto* navigation = new QVBoxLayout(rail);
        navigation->setContentsMargins(12, 17, 12, 12);
        navigation->setSpacing(6);
        navigation->addWidget(new QLabel("BuildQueue", rail));
        navigation->addSpacing(14);
        auto* pages = new QButtonGroup(this);
        layout->addWidget(rail);
        auto* body = new QVBoxLayout;
        body->setSpacing(12);
        layout->addLayout(body, 1);
        auto* header = new QHBoxLayout;
        auto* heading = new QLabel(this);
        heading->setObjectName("pageTitle");
        header->addWidget(heading);
        header->addStretch();
        header->addWidget(stopAll_);
        header->addWidget(resume_);
        resume_->setEnabled(false);
        connect(stopAll_, &QPushButton::clicked, this, [this] {
            queue_.pauseDispatch(true);
            restartComparison_ = comparison_.valid();
            restartCase_ = recovery_.valid() ? currentCase_ : QString();
            stopped_ = true;
            status_->showMessage("Stopping active work. Unfinished work will restart on Resume.");
            refresh();
        });
        connect(resume_, &QPushButton::clicked, this, [this] {
            stopped_ = false;
            queue_.pauseDispatch(false);
            status_->showMessage("Dispatch resumed.", 4000);
            if (restartComparison_)
                startComparison();
            if (!restartCase_.isEmpty())
                startLockedJobs(restartCase_);
            restartComparison_ = false;
            restartCase_.clear();
            refresh();
        });
        auto* add = new QPushButton("Add files", this);
        add->setObjectName("addFiles");
        add->setToolTip("Browse local and mounted Windows drives, or drop files on the window.");
        header->addWidget(add);
        body->addLayout(header);

        auto addWorkspace = [&](const QString& name) {
            auto* page = new QWidget(workspaces_);
            page->setProperty("workspace", true);
            page->setAttribute(Qt::WA_StyledBackground);
            auto* rows = new QVBoxLayout(page);
            rows->setContentsMargins(0, 0, 0, 0);
            rows->setSpacing(12);
            const int index = workspaces_->addWidget(page);
            auto* button = new QPushButton(name, rail);
            button->setCheckable(true);
            pages->addButton(button, index);
            navigation->addWidget(button);
            return rows;
        };
        auto* jobsRows = addWorkspace("Jobs");
        auto* compareRows = addWorkspace("Analysis");
        auto* timelineRows = addWorkspace("Timeline");
        timelinePage_ = workspaces_->count() - 1;
        auto* deadlockRows = addWorkspace("Deadlock");
        navigation->addStretch();
        connect(pages, &QButtonGroup::idClicked, workspaces_, &QStackedWidget::setCurrentIndex);
        auto showPage = [pages, heading](int index) {
            auto* button = pages->button(index);
            button->setChecked(true);
            heading->setText(button->text());
        };
        connect(workspaces_, &QStackedWidget::currentChanged, this, showPage);
        connect(workspaces_, &QStackedWidget::currentChanged, this, [this] { refresh(); });
        showPage(workspaces_->currentIndex());
        body->addWidget(workspaces_, 1);

        auto* metrics = new QHBoxLayout;
        metrics->setSpacing(9);
        auto addMetric = [&](const char* objectName, const QString& name, QLabel* value) {
            auto* card = new QWidget(this);
            card->setObjectName(objectName);
            card->setProperty("panel", true);
            card->setAttribute(Qt::WA_StyledBackground);
            auto* stack = new QVBoxLayout(card);
            stack->setContentsMargins(13, 7, 13, 7);
            stack->setSpacing(2);
            auto* label = new QLabel(name, card);
            label->setObjectName("metricLabel");
            value->setObjectName("metricValue");
            value->setAccessibleName(name);
            stack->addWidget(label);
            stack->addWidget(value);
            metrics->addWidget(card, 1);
        };
        addMetric("queuedCard", "Queued jobs", queuedMetric_);
        addMetric("completedCard", "Completed jobs", completedMetric_);
        addMetric("slotsCard", "Active slots", slotsMetric_);
        addMetric("waitCard", "Average wait", waitMetric_);
        jobsRows->addLayout(metrics);

        auto* controlsPanel = new QWidget(this);
        controlsPanel->setProperty("panel", true);
        controlsPanel->setAttribute(Qt::WA_StyledBackground);
        auto* controlRows = new QVBoxLayout(controlsPanel);
        controlRows->setContentsMargins(12, 9, 12, 9);
        controlRows->setSpacing(7);
        auto* controls = new QHBoxLayout;
        auto* policy = new QButtonGroup(this);
        auto addPolicy = [&](const QString& name, Policy value) {
            auto* button = new QPushButton(name, this);
            button->setProperty("policyChoice", true);
            button->setCheckable(true);
            policy->addButton(button, static_cast<int>(value));
            controls->addWidget(button);
            return button;
        };
        auto* scheduling = new QLabel("Scheduling", this);
        scheduling->setObjectName("sectionTitle");
        controls->addWidget(scheduling);
        auto* fcfs = addPolicy("FCFS", Policy::Fcfs);
        fcfs->setAccessibleName("First come, first served");
        fcfs->setChecked(true);
        addPolicy("SJF", Policy::Sjf)->setAccessibleName("Shortest job first");
        addPolicy("Priority", Policy::Priority);
        addPolicy("Round Robin", Policy::RoundRobin);
        priority_->setRange(0, 9);
        priority_->setToolTip("0 is the highest priority");
        auto* cancel = new QPushButton("Cancel selected", this);
        auto* clear = new QPushButton("Clear finished", this);
        controls->addSpacing(10);
        controls->addWidget(priorityLabel_);
        controls->addWidget(priority_);
        controls->addStretch();
        controlRows->addLayout(controls);
        policyHelp_->setObjectName("hint");
        policyHelp_->setWordWrap(true);
        controlRows->addWidget(policyHelp_);
        replay_->setToolTip("Rerun the current batch under the selected algorithm after all jobs finish.");
        showEarlier_->setToolTip("Compare the previous batch on its own elapsed-time scale.");
        auto* memoryLimit = new QSpinBox(this);
        memoryLimit->setRange(8, 4096);
        memoryLimit->setValue(defaultMemoryMiB);
        memoryLimit->setSuffix(" MiB");
        memoryLimit->setAccessibleName("Address-space limit per process");
        memoryLimit->setToolTip("Linux applies this virtual address-space limit to checksum jobs as they start.");
        connect(memoryLimit, &QSpinBox::valueChanged, this, [this](int value) { memoryMiB_.store(value); });
        jobsRows->addWidget(controlsPanel);

        auto* memoryPanel = new QWidget(this);
        memoryPanel->setProperty("panel", true);
        memoryPanel->setAttribute(Qt::WA_StyledBackground);
        auto* memoryRows = new QVBoxLayout(memoryPanel);
        memoryRows->setContentsMargins(12, 8, 12, 8);
        memoryRows->setSpacing(6);
        auto* comparisonControls = new QHBoxLayout;
        compare_->setObjectName("comparePolicies");
        comparisonControls->addWidget(compare_);
        comparisonControls->addWidget(new QLabel("Comparison slots", this));
        compareSlots_->setRange(1, 2);
        compareSlots_->setAccessibleName("Execution slots for policy comparison");
        comparisonControls->addWidget(compareSlots_);
        comparisonControls->addStretch();
        compareRows->addLayout(comparisonControls);
        comparisonInfo_->setObjectName("hint");
        comparisonInfo_->setTextFormat(Qt::PlainText);
        comparisonInfo_->setWordWrap(true);
        compareRows->addWidget(comparisonInfo_);
        comparisonTable_->setColumnCount(5);
        comparisonTable_->setHorizontalHeaderLabels(
            {"Policy", "Avg. wait", "Total elapsed", "Completed", "Dispatch order"});
        comparisonTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        comparisonTable_->verticalHeader()->hide();
        comparisonTable_->setAlternatingRowColors(true);
        comparisonTable_->setWordWrap(false);
        comparisonTable_->setShowGrid(false);
        comparisonTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        comparisonTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
        comparisonTable_->setMaximumHeight(220);
        compareRows->addWidget(comparisonTable_);
        auto* memoryControls = new QHBoxLayout;
        auto* memoryTitle = new QLabel("Checksum buffer pool", this);
        memoryTitle->setObjectName("sectionTitle");
        memoryRows->addWidget(memoryTitle);
        memoryControls->addWidget(new QLabel("Allocation policy", this));
        auto* fits = new QComboBox(this);
        fits->setAccessibleName("Buffer allocation strategy");
        fits->addItems({"First Fit", "Best Fit", "Worst Fit"});
        memoryControls->addWidget(fits);
        memoryControls->addStretch();
        memoryControls->addWidget(new QLabel("Process memory limit", this));
        memoryControls->addWidget(memoryLimit);
        memoryRows->addLayout(memoryControls);
        memoryRows->addWidget(poolMap_);
        poolInfo_->setWordWrap(true);
        poolInfo_->setObjectName("hint");
        memoryRows->addWidget(poolInfo_);
        compareRows->addWidget(memoryPanel);
        memoryTable_->setColumnCount(3);
        memoryTable_->setHorizontalHeaderLabels({"Offset", "Block size", "State"});
        memoryTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        memoryTable_->verticalHeader()->hide();
        memoryTable_->setAlternatingRowColors(true);
        memoryTable_->setShowGrid(false);
        memoryTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        memoryTable_->setMaximumHeight(190);
        compareRows->addWidget(memoryTable_);
        connect(fits, &QComboBox::currentIndexChanged, this, [this](int index) {
            fit_ = static_cast<Fit>(index);
            buffers_.setFit(fit_);
            refresh();
        });
        connect(compare_, &QPushButton::clicked, this, [this] { startComparison(); });

        table_->setColumnCount(Columns);
        table_->setHorizontalHeaderLabels({"File", "Status", "Arrival", "Burst", "Completion", "Turnaround",
                                           "Waiting", "Timeline", "Size (B)", "Priority", "Dispatch",
                                           "CRC checksum", "PID", "Peak RSS"});
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_->setSelectionMode(QAbstractItemView::SingleSelection);
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setAlternatingRowColors(true);
        table_->setWordWrap(false);
        table_->setShowGrid(false);
        table_->verticalHeader()->setDefaultSectionSize(41);
        table_->verticalHeader()->hide();
        table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table_->horizontalHeader()->setSectionResizeMode(File, QHeaderView::Stretch);
        table_->horizontalHeader()->setSectionResizeMode(Timeline, QHeaderView::Stretch);
        table_->setItemDelegateForColumn(Timeline, new TimelineDelegate(table_));
        auto* resultsHeading = new QLabel("Batch results", this);
        resultsHeading->setObjectName("sectionTitle");
        auto* resultsControls = new QHBoxLayout;
        resultsControls->addWidget(resultsHeading);
        resultsControls->addStretch();
        resultsControls->addWidget(replay_);
        resultsControls->addWidget(cancel);
        resultsControls->addWidget(clear);
        jobsRows->addLayout(resultsControls);
        auto* resultOptions = new QHBoxLayout;
        resultOptions->addWidget(showEarlier_);
        resultOptions->addStretch();
        auto* details = new QCheckBox("Show technical columns", this);
        resultOptions->addWidget(details);
        jobsRows->addLayout(resultOptions);
        auto showDetails = [this](bool visible) {
            for (auto column : {Size, Priority, Dispatch, Pid, Memory})
                table_->setColumnHidden(column, !visible);
        };
        connect(details, &QCheckBox::toggled, this, showDetails);
        showDetails(false);

        auto* empty = new QWidget(jobResults_);
        empty->setProperty("panel", true);
        empty->setAttribute(Qt::WA_StyledBackground);
        auto* emptyLayout = new QVBoxLayout(empty);
        auto* emptyTitle = new QLabel("No jobs yet", empty);
        emptyTitle->setObjectName("sectionTitle");
        auto* emptyHint = new QLabel("Add files or drop them here to start a checksum batch.", empty);
        emptyHint->setObjectName("hint");
        emptyLayout->addStretch();
        emptyLayout->addWidget(emptyTitle, 0, Qt::AlignHCenter);
        emptyLayout->addWidget(emptyHint, 0, Qt::AlignHCenter);
        emptyLayout->addStretch();
        jobResults_->addWidget(empty);
        jobResults_->addWidget(table_);
        jobsRows->addWidget(jobResults_, 1);
        timelineScale_->setObjectName("hint");
        jobsRows->addWidget(timelineScale_);

        solver_->setObjectName("solver");
        solver_->setTextFormat(Qt::PlainText);
        solver_->setWordWrap(true);
        compareRows->addStretch();
        auto* graphHint = new QLabel("Elapsed time from first submission. Tan: queued. Green: processing. Blue: running now.", this);
        graphHint->setWordWrap(true);
        auto* timelineControls = new QHBoxLayout;
        auto* timelineLabel = new QLabel("Scheduling policy", this);
        timelineLabel->setBuddy(timelinePolicy_);
        timelinePolicy_->setAccessibleName("Scheduling policy");
        for (const auto value : {Policy::Fcfs, Policy::Sjf, Policy::Priority, Policy::RoundRobin})
            timelinePolicy_->addItem(policyName(value), static_cast<int>(value));
        timelineReplay_->setToolTip("Run the current batch again using the selected scheduling policy.");
        timelineControls->addWidget(timelineLabel);
        timelineControls->addWidget(timelinePolicy_);
        timelineControls->addWidget(timelineReplay_);
        timelineControls->addStretch();
        timelineRows->addLayout(timelineControls);
        timelineRunPolicy_->setWordWrap(true);
        timelineRows->addWidget(timelineRunPolicy_);
        timelineRows->addWidget(graphHint);
        auto* scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setWidget(graph_);
        timelineRows->addWidget(scroll, 1);

        auto* demoHint = new QLabel("One deadlock file defines two jobs with opposite lock orders. "
                                    "BuildQueue detects the cycle, retries one job, and shows both checksum results.", this);
        demoHint->setWordWrap(true);
        deadlockRows->addWidget(demoHint);
        auto* demoControls = new QHBoxLayout;
        demoCase_->setAccessibleName("Bundled deadlock case");
        for (const auto& scenario : recoveryCases)
            demoCase_->addItem(QString::fromUtf8(scenario.title), QString::fromUtf8(scenario.filename));
        demoControls->addWidget(uploadLockJobs_);
        demoControls->addWidget(new QLabel("Or select a bundled case", this));
        demoControls->addWidget(demoCase_);
        demoControls->addWidget(runRecovery_);
        demoControls->addStretch();
        deadlockRows->addLayout(demoControls);
        deadlockRows->addWidget(waitForTimeout_);
        recoveryResult_->setObjectName("solver");
        recoveryResult_->setWordWrap(true);
        recoveryResult_->setTextFormat(Qt::PlainText);
        recoveryResult_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        deadlockRows->addWidget(recoveryResult_);
        deadlockRows->addWidget(waitGraph_, 1);
        compareRows->addWidget(solver_);
        deadlockRows->addStretch();
        connect(runRecovery_, &QPushButton::clicked, this, [this] { startRecoveryDemo(); });
        connect(uploadLockJobs_, &QPushButton::clicked, this, [this] {
            const auto file = QFileDialog::getOpenFileName(this, "Choose one deadlock file",
                demoDirectory().absolutePath(), "Deadlock files (*.txt);;All files (*)");
            if (!file.isEmpty())
                startLockedJobs(file);
        });
        body->addWidget(status_);

        connect(add, &QPushButton::clicked, this, [this] { chooseFiles(); });
        auto selectPolicy = [this, policy](int id) {
            try {
                choosePolicy(static_cast<Policy>(id));
            } catch (const std::exception& error) {
                status_->showMessage(QString::fromUtf8(error.what()), 7000);
            }
            const auto selected = static_cast<int>(queue_.snapshot().policy);
            policy->button(selected)->setChecked(true);
            const QSignalBlocker block(timelinePolicy_);
            timelinePolicy_->setCurrentIndex(timelinePolicy_->findData(selected));
        };
        connect(policy, &QButtonGroup::idClicked, this, selectPolicy);
        connect(timelinePolicy_, &QComboBox::currentIndexChanged, this, [this, selectPolicy](int index) {
            selectPolicy(timelinePolicy_->itemData(index).toInt());
        });
        connect(timelineReplay_, &QPushButton::clicked, this, [this] { rerunBatch(); });
        connect(cancel, &QPushButton::clicked, this, [this] {
            const auto* item = table_->item(table_->currentRow(), File);
            if (!item || !queue_.cancel(item->data(JobId).toULongLong()))
                status_->showMessage("Select a queued job to cancel.", 5000);
            refresh();
        });
        connect(clear, &QPushButton::clicked, this, [this] {
            queue_.clearFinished();
            refresh();
        });
        connect(replay_, &QPushButton::clicked, this, [this] { rerunBatch(); });
        connect(showEarlier_, &QCheckBox::toggled, this, [this] { refresh(); });
        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] {
            finishComparison();
            finishRecoveryDemo();
            refresh();
        });
        timer->start(300);
        choosePolicy(Policy::Fcfs);
        refresh();
    }

protected:
    void closeEvent(QCloseEvent* event) override {
        if (comparison_.valid() || recovery_.valid()) {
            status_->showMessage("Wait for the comparison or recovery demo to finish before closing.", 5000);
            event->ignore();
            return;
        }
        QWidget::closeEvent(event);
    }

    void dragEnterEvent(QDragEnterEvent* event) override {
        const auto urls = event->mimeData()->urls();
        if (std::any_of(urls.cbegin(), urls.cend(), [](const QUrl& url) { return url.isLocalFile(); }))
            event->acceptProposedAction();
    }

    void dropEvent(QDropEvent* event) override {
        QStringList paths;
        for (const auto& url : event->mimeData()->urls())
            if (url.isLocalFile())
                paths.append(url.toLocalFile());
        if (!paths.isEmpty()) {
            enqueue(paths);
            event->acceptProposedAction();
        }
    }

private:
    void startComparison() {
        if (stopped_ || comparison_.valid())
            return;
        const auto snapshot = queue_.snapshot();
        std::vector<JobRequest> batch;
        for (const auto& job : snapshot.jobs)
            if (job.id >= firstCurrentJob_ && job.state != State::Cancelled)
                batch.push_back({job.file, job.priority});
        if (batch.empty() || batch.size() > 100)
            return;
        try {
            comparison_ = std::async(std::launch::async, comparePolicies, std::move(batch),
                                     compareSlots_->value(), memoryMiB_.load(), fit_, &stopped_);
            compare_->setEnabled(false);
            comparisonTable_->setRowCount(0);
            comparisonInfo_->setText("Running all four policies on the same files…");
            status_->showMessage("Running FCFS, SJF and Priority on the current batch…");
        } catch (const std::exception& error) {
            status_->showMessage(QString::fromUtf8(error.what()), 7000);
        }
    }

    void finishComparison() {
        if (!comparison_.valid() ||
            comparison_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        status_->clearMessage();
        try {
            auto results = comparison_.get();
            if (results.empty() && stopped_)
                comparisonInfo_->setText("Comparison stopped. Resume restarts it.");
            else
                showComparison(results);
        } catch (const std::exception& error) {
            comparisonInfo_->setText("Comparison failed. " + QString::fromUtf8(error.what()));
            status_->showMessage(QString::fromUtf8(error.what()), 7000);
        }
    }

    void showComparison(const std::vector<Comparison>& results) {
        comparisonInfo_->setText("Results from the last comparison. Hover over dispatch order to see the full list.");
        comparisonTable_->setRowCount(static_cast<int>(results.size()));
        for (int row = 0; row < comparisonTable_->rowCount(); ++row) {
            const auto& result = results[row];
            const auto origin = result.jobs.front().submitted;
            double waits = 0, elapsed = 0;
            int completed = 0;
            QStringList order;
            for (const auto& job : result.jobs) {
                waits += milliseconds(job.waitTime(job.finished));
                elapsed = std::max(elapsed, milliseconds(job.finished - origin));
                completed += job.state == State::Completed;
                order.append(QFileInfo(QFile::decodeName(job.file.c_str())).fileName());
            }
            const QStringList values{policyName(result.policy), duration(waits / result.jobs.size()),
                duration(elapsed), QString("%1/%2").arg(completed).arg(static_cast<qulonglong>(result.jobs.size())),
                order.join(" -> ")};
            for (int col = 0; col < values.size(); ++col)
                comparisonTable_->setItem(row, col, new QTableWidgetItem(values[col]));
            comparisonTable_->item(row, 4)->setToolTip(order.join("\n"));
        }
        comparisonTable_->resizeRowsToContents();
    }

    void chooseFiles() {
        if (!picker_) {
            picker_ = new QFileDialog(this);
            picker_->setOption(QFileDialog::DontUseNativeDialog);
            picker_->setWindowModality(Qt::NonModal);
            picker_->setWindowTitle("Add files to BuildQueue");
            picker_->setFileMode(QFileDialog::ExistingFiles);
            picker_->setViewMode(QFileDialog::Detail);
            picker_->resize(820, 480);
            QList<QUrl> shortcuts;
            for (const auto& path : fileLocations())
                shortcuts.append(QUrl::fromLocalFile(path));
            picker_->setSidebarUrls(shortcuts);
            if (QDir("/mnt/c").exists())
                picker_->setDirectory("/mnt/c");
            connect(picker_, &QFileDialog::accepted, this, [this] {
                enqueue(picker_->selectedFiles());
            });
        }
        picker_->show();
        picker_->raise();
        picker_->activateWindow();
    }

    void choosePolicy(Policy policy) {
        queue_.setPolicy(policy);
        priority_->setEnabled(policy == Policy::Priority);
        priorityLabel_->setEnabled(policy == Policy::Priority);
        switch (policy) {
            case Policy::Fcfs: policyHelp_->setText("First come, first served. Jobs start in submission order."); break;
            case Policy::Sjf: policyHelp_->setText("Shortest job first. The smallest waiting file starts when a slot opens."); break;
            case Policy::Priority: policyHelp_->setText("Priority scheduling. Set the priority before adding each batch. 0 is highest."); break;
            case Policy::RoundRobin: policyHelp_->setText("Round Robin reads 1 MiB per turn, then puts unfinished jobs at the back of the queue."); break;
        }
    }

    void rerunBatch() {
        if (stopped_ || comparison_.valid())
            return;
        const auto snapshot = queue_.snapshot();
        if (std::any_of(snapshot.jobs.begin(), snapshot.jobs.end(), [](const Job& job) {
                return job.state == State::Queued || job.state == State::Running;
            }))
            return;
        std::vector<JobRequest> files;
        for (const auto& job : snapshot.jobs)
            if (job.id >= firstCurrentJob_ && job.state != State::Cancelled)
                files.push_back({job.file, job.priority});
        if (files.empty())
            return;

        try {
            const auto ids = queue_.submit(files);
            firstPreviousJob_ = firstCurrentJob_;
            firstCurrentJob_ = ids.front();
            showEarlier_->setChecked(false);
            status_->showMessage("Rerunning the batch with " + policyName(snapshot.policy) + ".", 5000);
            refresh();
        } catch (const std::exception& error) {
            status_->showMessage(QString::fromUtf8(error.what()), 7000);
        }
    }

    void enqueue(const QStringList& paths) {
        if (paths.isEmpty())
            return;
        if (comparison_.valid()) {
            status_->showMessage("Add files after the scheduling comparison finishes.", 5000);
            return;
        }
        std::vector<std::string> files;
        files.reserve(paths.size());
        for (const auto& path : paths) {
            const QFileInfo info(path);
            if (!info.isFile() || info.isSymLink()) {
                status_->showMessage("Choose regular files. Unavailable files and symlinks are skipped.", 7000);
                continue;
            }
            files.emplace_back(QFile::encodeName(info.absoluteFilePath()).toStdString());
        }
        if (files.empty())
            return;
        try {
            queue_.submit(files, priority_->value());
            showEarlier_->setChecked(false);
            workspaces_->setCurrentIndex(0);
            status_->showMessage(QString("Queued %1 file(s). Skipped %2.")
                .arg(static_cast<qlonglong>(files.size()))
                .arg(static_cast<qlonglong>(paths.size()) - static_cast<qlonglong>(files.size())), 5500);
            refresh();
        } catch (const std::exception& error) {
            status_->showMessage(QString::fromUtf8(error.what()), 7000);
        }
    }

    QTableWidgetItem* cell(QTableWidget* table, int row, int column, const QString& value) {
        auto* item = table->item(row, column);
        if (!item) {
            item = new QTableWidgetItem;
            table->setItem(row, column, item);
        }
        if (item->text() != value)
            item->setText(value);
        return item;
    }

    QTableWidgetItem* cell(int row, int column, const QString& value) {
        return cell(table_, row, column, value);
    }

    void refresh() {
        auto snapshot = queue_.snapshot();
        stopAll_->setEnabled(!stopped_);
        resume_->setEnabled(stopped_ && snapshot.running == 0 && !comparison_.valid() && !recovery_.valid());
        runRecovery_->setEnabled(!stopped_ && !recovery_.valid());
        uploadLockJobs_->setEnabled(!stopped_ && !recovery_.valid());
        waitForTimeout_->setEnabled(!recovery_.valid() && (!stopped_ || restartCase_.isEmpty()));
        const auto current = std::find_if(snapshot.jobs.begin(), snapshot.jobs.end(), [&](const Job& job) {
            return job.id >= firstCurrentJob_;
        });
        const auto previous = std::find_if(snapshot.jobs.begin(), current, [&](const Job& job) {
            return job.id >= firstPreviousJob_;
        });
        const bool busy = std::any_of(snapshot.jobs.begin(), snapshot.jobs.end(), [](const Job& job) {
            return job.state == State::Queued || job.state == State::Running;
        });
        const auto availableJobs = std::count_if(current, snapshot.jobs.end(),
                                                [](const Job& job) { return job.state != State::Cancelled; });
        const bool canRerun = !stopped_ && !busy && !comparison_.valid() && availableJobs > 0 && availableJobs <= 100;
        replay_->setEnabled(canRerun);
        timelineReplay_->setEnabled(canRerun);
        compare_->setEnabled(!stopped_ && !busy && !comparison_.valid() && availableJobs > 0 && availableJobs <= 100);
        const auto pool = buffers_.stats();
        poolMap_->setBlocks(pool.blocks);
        memoryTable_->setRowCount(static_cast<int>(pool.blocks.size()));
        QStringList blocks;
        for (int row = 0; row < memoryTable_->rowCount(); ++row) {
            const auto& block = pool.blocks[row];
            blocks.append(QString("%1–%2 KiB %3")
                .arg(static_cast<double>(block.offset) / 1024, 0, 'f', 1)
                .arg(static_cast<double>(block.offset + block.size) / 1024, 0, 'f', 1)
                .arg(block.used ? "used" : "free"));
            cell(memoryTable_, row, 0, QString("%1 B").arg(static_cast<qulonglong>(block.offset)));
            cell(memoryTable_, row, 1, QString("%1 B").arg(static_cast<qulonglong>(block.size)));
            cell(memoryTable_, row, 2, block.used ? "In use" : "Free");
        }
        poolMap_->setToolTip(blocks.join("\n"));
        poolInfo_->setText(QString("%1 KiB free · %2 KiB largest free block · %3 KiB peak used\n"
                                   "%4% fragmented · %5% peak fragmentation · %6 coalesces · Olive: in use")
            .arg(static_cast<double>(pool.free) / 1024, 0, 'f', 1)
            .arg(static_cast<double>(pool.largest) / 1024, 0, 'f', 1)
            .arg(static_cast<double>(pool.peak) / 1024, 0, 'f', 1)
            .arg(pool.fragmentation, 0, 'f', 1).arg(pool.peakFragmentation, 0, 'f', 1)
            .arg(static_cast<qulonglong>(pool.merges)));
        const bool hasPrevious = previous != current;
        const bool viewingPrevious = hasPrevious && showEarlier_->isChecked();
        showEarlier_->setEnabled(hasPrevious);
        if (viewingPrevious) {
            snapshot.jobs.erase(current, snapshot.jobs.end());
            snapshot.jobs.erase(snapshot.jobs.begin(), previous);
        } else {
            snapshot.jobs.erase(snapshot.jobs.begin(), current);
        }
        const auto now = Clock::now();
        const auto origin = snapshot.jobs.empty() ? now : snapshot.jobs.front().submitted;
        std::stable_sort(snapshot.jobs.begin(), snapshot.jobs.end(), [](const Job& a, const Job& b) {
            const auto end = [](const Job& job) {
                return job.finished == Clock::time_point{} ? Clock::time_point::max() : job.finished;
            };
            return end(a) < end(b);
        });
        std::vector<Timing> timings;
        timings.reserve(snapshot.jobs.size());
        double span = 1;
        for (const auto& job : snapshot.jobs) {
            const auto times = timing(job, origin, now);
            timings.push_back(times);
            span = std::max(span, times.offset + times.waiting + times.running);
        }
        if (workspaces_->currentIndex() == timelinePage_) {
            QStringList recorded;
            for (const auto& job : snapshot.jobs) {
                const auto name = policyName(job.dispatchPolicy);
                if (job.dispatchOrder && !recorded.contains(name))
                    recorded.append(name);
            }
            const auto batchLabel = viewingPrevious ? "Previous batch" : "Current batch";
            timelineRunPolicy_->setText(recorded.isEmpty()
                ? QString("%1 scheduling: no jobs dispatched yet.").arg(batchLabel)
                : QString("%1 scheduling: %2").arg(batchLabel).arg(recorded.join(", ")));
            graph_->setJobs(snapshot.jobs, origin, now, span);
        }

        const auto* selected = table_->item(table_->currentRow(), File);
        const auto selectedId = selected ? selected->data(JobId).toULongLong() : 0;
        table_->setRowCount(static_cast<int>(snapshot.jobs.size()));
        jobResults_->setCurrentIndex(snapshot.jobs.empty() ? 0 : 1);
        timelineScale_->setVisible(!snapshot.jobs.empty());
        int selectedRow = -1;
        int completed = 0;
        int queued = 0;
        double waitTotal = 0;
        double runTotal = 0;
        for (int row = 0; row < table_->rowCount(); ++row) {
            const auto& job = snapshot.jobs[row];
            const auto& times = timings[row];
            const QString path = QFile::decodeName(job.file.c_str());
            auto* file = cell(row, File, QFileInfo(path).fileName());
            file->setToolTip(path);
            file->setData(JobId, QVariant::fromValue<qulonglong>(job.id));
            if (job.id == selectedId)
                selectedRow = row;
            auto label = stateName(job.state);
            if (job.state == State::Failed && job.result.exitCode == 124)
                label = "Timed out";
            else if (stopped_ && job.state == State::Queued)
                label = "Held";
            else if (stopped_ && job.state == State::Running)
                label = "Stopping";
            auto* state = cell(row, Status, label);
            state->setToolTip(QString::fromStdString(job.result.error));
            if (job.state == State::Completed) {
                ++completed;
                waitTotal += times.waiting;
                runTotal += times.running;
            }
            if (job.state == State::Queued)
                ++queued;
            switch (job.state) {
                case State::Queued:
                    state->setForeground(QColor("#7c470b"));
                    state->setBackground(QColor("#fff1d2"));
                    break;
                case State::Running:
                    state->setForeground(QColor("#625021"));
                    state->setBackground(QColor("#f2e6c9"));
                    break;
                case State::Completed:
                    state->setForeground(QColor("#38533a"));
                    state->setBackground(QColor("#e4eddb"));
                    break;
                case State::Failed:
                    state->setForeground(QColor("#923b35"));
                    state->setBackground(QColor("#f3dfdb"));
                    break;
                case State::Cancelled:
                    state->setForeground(QColor("#61594c"));
                    state->setBackground(QColor("#eee8df"));
                    break;
            }
            cell(row, Size, job.estimatedBytes
                ? QString::number(static_cast<qulonglong>(*job.estimatedBytes)) : QString());
            cell(row, Priority, QString::number(job.priority));
            cell(row, Dispatch, times.started
                ? QString("#%1 · %2").arg(static_cast<qulonglong>(job.dispatchOrder)).arg(policyName(job.dispatchPolicy))
                : QString());
            cell(row, Crc, job.state == State::Completed ? QString::number(job.result.checksum) : QString());
            cell(row, Pid, job.result.pid > 0 ? QString::number(job.result.pid) : QString());
            auto* memory = cell(row, Memory, job.result.peakRssKiB
                ? QString("%1 KiB").arg(static_cast<qulonglong>(*job.result.peakRssKiB)) : QString());
            memory->setToolTip(job.result.addressSpaceLimitBytes
                ? QString("Peak resident memory. Address-space limit: %1 MiB.")
                    .arg(static_cast<qulonglong>(job.result.addressSpaceLimitBytes / bytesPerMiB))
                : QString());
            cell(row, Wait, duration(times.waiting));
            cell(row, Run, times.started ? duration(times.running) : QString());
            cell(row, Arrival, duration(times.offset));
            const bool finished = job.finished != Clock::time_point{};
            cell(row, Completion, finished ? duration(milliseconds(job.finished - origin)) : QString());
            cell(row, Turnaround, finished ? duration(milliseconds(job.finished - job.submitted)) : QString());
            auto* bar = cell(row, Timeline, {});
            bar->setData(Start, times.offset / span);
            const auto end = job.finished == Clock::time_point{} ? now : job.finished;
            const bool clipped = job.timelineTruncated || (job.state == State::Running && job.slices.size() == 256);
            bar->setData(Finish, milliseconds((clipped ? job.slices.back().second : end) - origin) / span);
            QVariantList intervals;
            for (const auto& [first, last] : job.slices) {
                intervals.append(milliseconds(first - origin) / span);
                intervals.append(milliseconds(last - origin) / span);
            }
            if (job.state == State::Running && job.slices.size() < 256) {
                intervals.append(milliseconds(job.sliceStarted - origin) / span);
                intervals.append(milliseconds(now - origin) / span);
            }
            bar->setData(Processing, intervals);
            bar->setToolTip(QString("Submitted at %1, queued for %2, processing %3")
                .arg(duration(times.offset), duration(times.waiting),
                     times.started ? duration(times.running) : QString("pending"))
                + (clipped ? QString("\nTimeline shows only the first 256 slices.") : QString()));
        }
        if (selectedId && selectedRow != table_->currentRow())
            table_->setCurrentCell(selectedRow, selectedRow < 0 ? -1 : File);
        queuedMetric_->setText(QString::number(queued));
        completedMetric_->setText(QString::number(completed));
        slotsMetric_->setText(QString("%1 / %2").arg(snapshot.running).arg(snapshot.slots));
        slotsMetric_->setToolTip(QString("%1 workers · peak %2 active slots").arg(snapshot.workers).arg(snapshot.peak));
        waitMetric_->setText(completed ? duration(waitTotal / completed) : QString("—"));
        waitMetric_->setToolTip(completed ? "Average run: " + duration(runTotal / completed) : QString());
        timelineScale_->setText(QString("%1 timeline: 0 to %2  ·  Tan: queued  ·  Olive: processing")
            .arg(viewingPrevious ? "Previous batch" : "Current batch", duration(span)));
        auto solverText = QString("Deadlock solver: wait-for graph cycle detection  |  Recovery: reject the request that closes the cycle  |  Detected: %1")
            .arg(static_cast<qulonglong>(snapshot.deadlocks.count));
        if (!snapshot.deadlocks.lastCycle.empty())
            solverText += "\n" + QString::fromStdString(snapshot.deadlocks.lastCycle);
        solver_->setText(solverText);
    }

    QDir demoDirectory() const {
        const QString relative = "demo-files/deadlock";
        QDir demo(QDir::current().filePath(relative));
        if (!demo.exists())
            demo = QDir(QDir(QCoreApplication::applicationDirPath()).filePath(relative));
        return demo;
    }

    void startRecoveryDemo() {
        startLockedJobs(demoDirectory().absoluteFilePath(demoCase_->currentData().toString()));
    }

    void startLockedJobs(const QString& file) {
        if (stopped_ || recovery_.valid())
            return;
        const auto path = QFile::encodeName(file).toStdString();
        try {
            lockJobName_ = QFileInfo(file).fileName();
            currentCase_ = file;
            const auto timeout = waitForTimeout_->isChecked() ? std::chrono::seconds(10) : std::chrono::seconds(0);
            recovery_ = std::async(std::launch::async, [this, path, timeout] {
                return runLockedChecksumFile(path, timeout, &stopped_, [this](const RecoveryDemoResult& cycle) {
                    std::lock_guard<std::mutex> lock(cycleMutex_);
                    activeCycle_ = cycle;
                });
            });
            runRecovery_->setEnabled(false);
            uploadLockJobs_->setEnabled(false);
            demoCase_->setEnabled(false);
            waitGraph_->setCycle(std::nullopt);
            recoveryResult_->setText("Starting lock jobs from " + lockJobName_ + "...");
        } catch (const std::exception& error) {
            recoveryResult_->setText(QString::fromUtf8(error.what()));
        }
    }

    void finishRecoveryDemo() {
        std::optional<RecoveryDemoResult> cycle;
        {
            std::lock_guard<std::mutex> lock(cycleMutex_);
            cycle.swap(activeCycle_);
        }
        if (cycle) {
            auto summary = QString("Deadlock detected in %1").arg(lockJobName_);
            for (const auto& edge : cycle->detection.cycleEdges) {
                const auto waiter = std::find(cycle->workers.begin(), cycle->workers.end(), edge.waiter);
                const auto owner = std::find(cycle->workers.begin(), cycle->workers.end(), edge.owner);
                summary += QString("\nJob %1: %2 %3 (held by Job %4)")
                    .arg(static_cast<int>(std::distance(cycle->workers.begin(), waiter) + 1))
                    .arg(edge.rejected ? "Rejected request for" : "Waiting for")
                    .arg(QString::fromStdString(edge.resource))
                    .arg(static_cast<int>(std::distance(cycle->workers.begin(), owner) + 1));
            }
            recoveryResult_->setText(summary);
            waitGraph_->setCycle(std::move(cycle));
        }
        if (!recovery_.valid() ||
            recovery_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        runRecovery_->setEnabled(!stopped_);
        uploadLockJobs_->setEnabled(!stopped_);
        demoCase_->setEnabled(true);
        try {
            const auto result = recovery_.get();
            const auto count = static_cast<int>(result.workers.size());
            if (result.stopped || result.timedOut) {
                auto summary = QString("%1: %2. %3 cycle(s) detected.")
                    .arg(lockJobName_).arg(result.stopped ? "Stopped" : "Timed out")
                    .arg(static_cast<qulonglong>(result.detection.count));
                for (int i = 0; i < count; ++i)
                    summary += QString("\nJob %1: %2").arg(i + 1)
                        .arg(result.checksums[i].exitCode == 0 ? "Completed" : result.stopped ? "Stopped" : "Timed out");
                recoveryResult_->setText(summary);
                if (result.detection.count)
                    waitGraph_->setCycle(result);
                return;
            }
            auto summary = QString("%1: Recovered. %2 of %3 jobs finished.\n"
                        "%4 cycle detected. Job %5 was retried after its lock was released.")
                    .arg(lockJobName_)
                    .arg(result.completed)
                    .arg(count)
                    .arg(static_cast<qulonglong>(result.detection.count))
                    .arg(result.victim);
            for (std::size_t i = 0; i < result.checksums.size(); ++i)
                summary += QString("\nJob %1: CRC %2 | %3 bytes")
                    .arg(static_cast<int>(i + 1))
                    .arg(result.checksums[i].checksum)
                    .arg(static_cast<qulonglong>(result.checksums[i].bytes));
            recoveryResult_->setText(summary);
            waitGraph_->setCycle(result);
        } catch (const std::exception& error) {
            recoveryResult_->setText(QString("Could not run the lock jobs: %1").arg(QString::fromUtf8(error.what())));
        }
    }
};
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Desktop window;
    window.show();
    return app.exec();
}

#include "ThreadPool.h"

// ------------------------------
// Task 移动构造/赋值
// ------------------------------
Task::Task(Task&& other) noexcept
    : func(std::move(other.func))
    , promise(std::move(other.promise))
    , m_level(std::move(other.m_level))
    , m_timeout(std::move(other.m_timeout)) {
}

Task& Task::operator=(Task&& other) noexcept {
    if (this != &other) {
        m_level = std::move(other.m_level);
        m_timeout = std::move(other.m_timeout);
        func = std::move(other.func);
        promise = std::move(other.promise);
    }
    return *this;
}

// ------------------------------
// MaxHeapTaskQueue
// ------------------------------
bool MaxHeapTaskQueue::empty() const {
    return que.empty();
}

std::size_t MaxHeapTaskQueue::size() const {
    return que.size();
}

const Task& MaxHeapTaskQueue::top() const {
    return que.top();
}

void MaxHeapTaskQueue::pop() {
    que.pop();
}

void MaxHeapTaskQueue::push(Task&& task) {
    que.push(std::move(task));
}

// ------------------------------
// MinHeapTaskQueue
// ------------------------------
bool MinHeapTaskQueue::empty() const {
    return que.empty();
}

std::size_t MinHeapTaskQueue::size() const {
    return que.size();
}

const Task& MinHeapTaskQueue::top() const {
    return que.top();
}

void MinHeapTaskQueue::pop() {
    que.pop();
}

void MinHeapTaskQueue::push(Task&& task) {
    que.push(std::move(task));
}

// ------------------------------
// TaskQueue
// ------------------------------
TaskQueue::TaskQueue(PriorityMode mode)
    : TaskQueue(-1, mode) {
}

TaskQueue::TaskQueue(int initCapacity, PriorityMode mode)
    : m_mode(mode) {
    if (initCapacity > 0) {
        maxCapacity.store(initCapacity);
        std::cout << "队列最大容量设置为:" << maxCapacity.load() << std::endl;
    }
    else {
        std::cerr << "[任务队列] 初始容量设置为无上限" << std::endl;
        maxCapacity = std::numeric_limits<int>::max();
    }
    // 初始化队列指针
    if (m_mode == MaxHeap) {
        taskQ = std::make_unique<MaxHeapTaskQueue>();
    }
    else {
        taskQ = std::make_unique<MinHeapTaskQueue>();
    }
    std::cout << "队列模式设置为" << (mode == MinHeap ? "LOW优先模式" : "HIGH优先模式") << std::endl;
}

void TaskQueue::bindConditionVariable(std::condition_variable* cond) {
    if (cond == nullptr || queueShutdown.load()) {
        std::cerr << "[任务队列] 条件变量为空或队列已关闭，绑定失败" << std::endl;
        return;
    }
    std::lock_guard<std::mutex> lock(queueMutex); // 加锁保护条件变量列表
    // 避免重复绑定（防止同一线程池多次绑定导致重复唤醒）
    for (auto existingCond : notifyConds) {
        if (existingCond == cond) {
            std::cerr << "[任务队列] 条件变量已绑定，忽略" << std::endl;
            return;
        }
    }
    notifyConds.push_back(cond);
    std::cout << "[任务队列] 成功绑定条件变量，当前绑定数：" << notifyConds.size() << std::endl;
}

void TaskQueue::unbindConditionVariable(std::condition_variable* cond) {
    if (cond == nullptr) return;
    std::lock_guard<std::mutex> lock(queueMutex); // 加锁保护条件变量列表
    auto it = std::find(notifyConds.begin(), notifyConds.end(), cond);
    if (it != notifyConds.end()) {
        notifyConds.erase(it);
        std::cout << "[任务队列] 成功解绑条件变量，当前绑定数：" << notifyConds.size() << std::endl;
    }
}

std::optional<Task> TaskQueue::taskTake(ThreadPoolPrivateKey key) {
    std::lock_guard<std::mutex> lock(queueMutex); // 加锁保护队列操作
    // 队列空：返回空
    if (taskQ->empty()) {
        return std::nullopt;
    }

    // 取出队首任务（移动语义，避免拷贝）
    Task task = std::move(const_cast<Task&>(taskQ->top()));
    taskQ->pop();
    queueSize--; // 任务数原子递减
    return task;
}

void TaskQueue::wakeupAllBoundPools() {
    std::lock_guard<std::mutex> lock(queueMutex); // 加锁保护条件变量列表
    for (auto cond : notifyConds) {
        if (cond != nullptr) {
            cond->notify_one(); // 每个线程池唤醒一个线程（减少上下文切换）
        }
    }
    if (!notifyConds.empty()) {
        std::cout << "[任务队列] 唤醒所有关联线程池（" << notifyConds.size() << "个），各唤醒一个线程" << std::endl;
    }
}

void TaskQueue::wakeupAllThreadsInBoundPools() {
    std::lock_guard<std::mutex> lock(queueMutex); // 加锁保护条件变量列表
    for (auto cond : notifyConds) {
        if (cond != nullptr) {
            cond->notify_all(); // 唤醒所有线程（避免线程阻塞在wait）
        }
    }
    std::cout << "[任务队列] 强制唤醒所有关联线程池的所有线程" << std::endl;
}

void TaskQueue::shutdownQueue() {
    if (queueShutdown.load()) return;
    queueShutdown.store(true); // 设置关闭标志（原子操作）
    wakeupAllThreadsInBoundPools(); // 唤醒所有线程处理剩余任务
    std::cout << "[任务队列] 队列已关闭，拒绝新任务，唤醒所有线程处理剩余任务" << std::endl;
}

bool TaskQueue::isQueueShutdown() const {
    return queueShutdown.load();
}

void TaskQueue::setMaxCapacity(int capacity) {
    if (capacity <= 0 || queueShutdown.load()) {
        std::cerr << "[任务队列] 容量非法或队列已关闭，设置失败" << std::endl;
        return;
    }
    std::lock_guard<std::mutex> lock(queueMutex);
    maxCapacity = capacity;
    std::cout << "[任务队列] 最大容量设置为：" << capacity << "（当前任务数：" << queueSize.load() << "）" << std::endl;
}

int TaskQueue::getCurrentSize() const {
    return queueSize.load();
}

int TaskQueue::getMaxCapacity() const {
    return maxCapacity.load();
}

bool TaskQueue::isQueueFull() {
    if (maxCapacity.load() == std::numeric_limits<int>::max()) {
        return false;
    }
    return queueSize.load() >= maxCapacity.load();
}

bool TaskQueue::empty() {
    return queueSize.load() == 0;
}

void TaskQueue::changeMode(PriorityMode mode) {
    // 非法传入模式
    if (mode != MaxHeap && mode != MinHeap) {
        std::cout << "非法的模式" << std::endl;
        return;
    }
    if (m_mode == mode) {
        std::cout << "切换失败，当前已是此模式" << std::endl;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        std::unique_ptr<BaseTaskQuene> newQue;
        if (mode == MinHeap) {
            newQue = std::make_unique<MinHeapTaskQueue>();
        }
        else {
            newQue = std::make_unique<MaxHeapTaskQueue>();
        }
        while (!taskQ->empty()) {
            newQue->push(std::move(const_cast<Task&>(taskQ->top())));
            taskQ->pop();
        }
        taskQ = std::move(newQue);
    }
    m_mode = mode;
    wakeupAllThreadsInBoundPools();
    std::cout << "队列一切已切换至" << (mode == MinHeap ? "LOW优先" : "HIGH优先") << std::endl;
}

void TaskQueue::recordTaskFinish(ThreadPoolPrivateKey key, int64_t us) {
    m_total_task_cnt.fetch_add(1, std::memory_order_relaxed);
    m_total_cost_us.fetch_add(us, std::memory_order_relaxed);
    int64_t oldMax = m_max_cost_us.load(std::memory_order_relaxed);
    while (us > oldMax) {
        if (m_max_cost_us.compare_exchange_weak(oldMax, us, std::memory_order_relaxed)) {
            break;
        }
    }
}

void TaskQueue::recordFailTaskCount(ThreadPoolPrivateKey key) {
    m_fail_task_cnt.fetch_add(1, std::memory_order_relaxed);
}

void TaskQueue::recordTimeoutTaskCount(ThreadPoolPrivateKey key) {
    m_timeout_task_cnt.fetch_add(1, std::memory_order_relaxed);
}

uint64_t TaskQueue::getTotalTaskCount() const {
    return m_total_task_cnt.load();
}

uint64_t TaskQueue::getFailTaskCount() const {
    return m_fail_task_cnt.load();
}

uint64_t TaskQueue::getTimeoutTaskCount() const {
    return m_timeout_task_cnt.load();
}

uint64_t TaskQueue::getMaxCostUs() const {
    return m_max_cost_us.load();
}

double TaskQueue::getAvgCostUs() const {
    auto total = m_total_task_cnt.load();
    return total ? (double)m_total_cost_us.load() / total : 0.0;
}

// ------------------------------
// ThreadPool::WorkerStatus
// ------------------------------
ThreadPool::WorkerStatus::WorkerStatus(std::thread&& t)
    : thread(std::move(t))
    , isFinish(false)
    , tid(this->thread.get_id()) {
}

ThreadPool::WorkerStatus::WorkerStatus(WorkerStatus&& other) noexcept
    : thread(std::move(other.thread))
    , isFinish(other.isFinish.load())
    , tid(other.tid) {
}

ThreadPool::WorkerStatus& ThreadPool::WorkerStatus::operator=(WorkerStatus&& other) noexcept {
    if (this != &other) {
        thread = std::move(other.thread);
        isFinish = other.isFinish.load();
        tid = other.tid;
    }
    return *this;
}

// ------------------------------
// ThreadPool::BusyNumGuard
// ------------------------------
ThreadPool::BusyNumGuard::BusyNumGuard(std::atomic_int& busyNum, std::mutex& lock)
    : m_busyNum(busyNum)
    , m_lock(lock) {
    std::lock_guard<std::mutex> guard(m_lock);
    m_busyNum++;
}

ThreadPool::BusyNumGuard::~BusyNumGuard() {
    std::lock_guard<std::mutex> guard(m_lock);
    m_busyNum--;
}

// ------------------------------
// ThreadPool
// ------------------------------
ThreadPool::ThreadPool(std::string name, TaskQueue* taskq, PoolMode mode, int livenum)
    : PoolName(name)
    , poolmode(mode) {
    // 参数合法性检查：线程数>0、任务队列非空、模式为FIXED、队列未关闭
    if (livenum <= 0 || taskq == nullptr || mode != PoolMode::FIXED || taskq->isQueueShutdown()) {
        std::cerr << PoolName << "[ThreadPool] 初始化失败：参数非法或队列已关闭" << std::endl;
        exit(EXIT_FAILURE); // 初始化失败直接退出（避免后续错误）
    }

    minNum = livenum;  // FIXED模式：核心线程数=传入的线程数
    maxNum = livenum;  // FIXED模式：最大线程数=核心线程数
    liveNum = livenum; // 初始存活线程数=核心线程数
    TaskQ = taskq;     // 绑定任务队列
    isInitTrue = true; // 标记初始化成功

    TaskQ->bindConditionVariable(&notEmpty); // 将线程池的条件变量绑定到队列

    workersThread.reserve(livenum); // 预分配线程列表容量（避免频繁扩容）
    // 创建核心工作线程
    for (int i = 0; i < livenum; ++i) {
        workersThread.emplace_back(
            std::thread([this]() { this->workerFunc(); }) // 线程执行workerFunc
        );
        std::cout << PoolName << "[初始化] 创建核心线程，tid=" << workersThread.back().tid << std::endl;
    }
    // 创建线程保护者
    fixedGuardThread = std::thread([this] {this->fixedGuardFunc(); });
    std::cout << PoolName << "[初始化] 创建守护者线程，tid=" << fixedGuardThread.get_id() << std::endl;
}

ThreadPool::ThreadPool(std::string name, TaskQueue* taskq, PoolMode mode, int min, int max)
    : PoolName(name)
    , poolmode(mode) {
    // 参数合法性检查：核心线程数>0、最大>=核心、任务队列非空、模式为CACHED、队列未关闭
    if (min <= 0 || max < min || taskq == nullptr || mode != PoolMode::CACHED || taskq->isQueueShutdown()) {
        std::cerr << PoolName << "[ThreadPool] 初始化失败：参数非法或队列已关闭" << std::endl;
        exit(EXIT_FAILURE);
    }

    minNum = min;      // 核心线程数（最小存活数）
    maxNum = max;      // 最大线程数（扩容上限）
    liveNum = min;     // 初始存活线程数=核心线程数
    TaskQ = taskq;     // 绑定任务队列
    isInitTrue = true; // 标记初始化成功

    TaskQ->bindConditionVariable(&notEmpty); // 绑定条件变量到队列

    workersThread.reserve(max); // 预分配最大容量（避免扩容开销）
    // 创建核心工作线程
    for (int i = 0; i < min; ++i) {
        workersThread.emplace_back(
            std::thread([this]() { this->workerFunc(); })
        );
        std::cout << PoolName << "[初始化] 创建核心线程，tid=" << workersThread.back().tid << std::endl;
    }

    // 创建管理者线程（负责动态扩缩容）
    managerThread = std::thread([this] { this->managerFunc(); });
    std::cout << PoolName << "[初始化] 创建管理者线程，tid=" << managerThread.get_id() << std::endl;
}

ThreadPool::~ThreadPool() {
    std::cout << PoolName << "[析构] 线程池开始销毁，自动解绑任务队列关联" << std::endl;

    // 关闭线程池：回收所有工作线程和管理者线程
    shutdownPool();
    // 解绑条件变量：避免队列持有已销毁线程池的条件变量（野指针）
    if (TaskQ != nullptr) {
        TaskQ->unbindConditionVariable(&notEmpty);
    }
    std::cout << PoolName << "[析构] 线程池销毁完成" << std::endl;
}

void ThreadPool::shutdownPool() {
    if (shutdown.load() || !isInitTrue) {
        return; // 已关闭或未初始化，直接返回
    }

    shutdown.store(true); // 设置关闭标志（原子操作）
    std::cout << PoolName << "[关机] 开始关闭线程池" << std::endl;

    // 唤醒所有线程：通过队列唤醒所有关联线程（避免遗漏）
    if (TaskQ != nullptr) {
        TaskQ->wakeupAllThreadsInBoundPools();
    }
    else {
        notEmpty.notify_all(); // 队列空时直接唤醒自身线程
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 避免虚假唤醒
    // 二次唤醒：确保所有线程都能收到关闭信号（防止第一次唤醒失败）
    if (TaskQ != nullptr) {
        TaskQ->wakeupAllThreadsInBoundPools();
    }
    else {
        notEmpty.notify_all();
    }

    // 回收工作线程：加锁保护线程列表
    {
        std::lock_guard<std::mutex> lock(poolMutex);
        for (auto& worker : workersThread) {
            if (worker.thread.joinable()) { // 线程可join（未被回收）
                worker.thread.join(); // 阻塞等待线程退出
                std::cout << PoolName << "[关机] 回收线程，tid=" << worker.tid << std::endl;
            }
        }
        workersThread.clear(); // 清空线程列表
    }
    // 回收管理者线程（仅CACHED模式有）
    if (poolmode == PoolMode::CACHED && managerThread.joinable()) {
        std::thread::id tid = managerThread.get_id();
        managerThread.join();
        std::cout << PoolName << "[关机] 回收管理者线程，tid=" << tid << std::endl;
    }
    // 回收守护者线程（仅FIXED模式有）
    if (poolmode == PoolMode::FIXED && fixedGuardThread.joinable()) {
        std::thread::id tid = fixedGuardThread.get_id();
        fixedRebuildSem.release();
        fixedGuardThread.join();
        std::cout << PoolName << "[关机] 回收守护者线程，tid=" << tid << std::endl;
    }

    std::cout << PoolName << "[关机] 线程池关闭完成，回收" << liveNum.load() << "个核心线程" << std::endl;
    isInitTrue = false; // 标记未初始化
}

std::string ThreadPool::getPoolName() const {
    return PoolName;
}

void ThreadPool::resumePool() {
    if (!shutdown.load()) {
        std::cerr << PoolName << "[重启] 无法重启：未关闭" << std::endl;
        return;
    }

    shutdown.store(false); // 重置关闭标志
    exitNum.store(0);      // 重置待退出数
    liveNum.store(minNum); // 重置存活数为核心数
    isInitTrue = true;     // 标记初始化成功

    // 重建核心工作线程
    {
        std::lock_guard<std::mutex> lock(poolMutex);
        workersThread.reserve(maxNum);
        for (int i = 0; i < minNum; ++i) {
            workersThread.emplace_back(
                std::thread([this]() { this->workerFunc(); })
            );
            std::cout << PoolName << "[重启] 创建核心线程，tid=" << workersThread.back().tid << std::endl;
        }
    }

    // 重建管理者线程（仅CACHED模式）
    if (poolmode == PoolMode::CACHED) {
        if (managerThread.joinable()) {
            managerThread.join();
        }
        managerThread = std::thread([this]() { this->managerFunc(); });
        std::cout << PoolName << "[重启] 创建管理者线程，tid=" << managerThread.get_id() << std::endl;
    }

    // 重建守护者线程（仅FIXED模式）
    if (poolmode == PoolMode::FIXED) {
        if (fixedGuardThread.joinable()) {
            fixedGuardThread.join();
        }
        fixedGuardThread = std::thread([this]() { this->fixedGuardFunc(); });
        std::cout << PoolName << "[重启] 创建守护者线程，tid=" << fixedGuardThread.get_id() << std::endl;
    }
}

void ThreadPool::setEverytimeAddCount(int num) {
    if (poolmode != PoolMode::CACHED) {
        std::cerr << PoolName << "[灵活调节] 仅CACHED模式支持" << std::endl;
        return;
    }
    if (num <= 0) {
        std::cerr << PoolName << "[灵活调节] 扩容数必须>0" << std::endl;
        return;
    }
    std::lock_guard<std::mutex> lock(poolMutex);
    ADDCOUNT = num;
    std::cout << PoolName << "[灵活调节] 扩容数设置为：" << num << std::endl;
}

void ThreadPool::Rename(std::string name) {
    if (!shutdown.load()) {
        std::cerr << PoolName << "[重命名] 仅关闭后支持重命名" << std::endl;
        return;
    }
    std::cout << PoolName << "[重命名] 重命名为：" << name << std::endl;
    PoolName = name;
}

int ThreadPool::getLiveNum() const {
    return liveNum.load();
}

int ThreadPool::getBusyNum() const {
    return busyNum.load();
}

int ThreadPool::getQueueCurrentSize() const {
    return TaskQ->getCurrentSize();
}

int ThreadPool::getQueueMaxCapacity() const {
    return TaskQ->getMaxCapacity();
}

void ThreadPool::changeMode(TaskQueue::PriorityMode mode) {
    TaskQ->changeMode(mode);
}

void ThreadPool::setQueueMaxCapacity(int capacity) {
    if (TaskQ == nullptr || shutdown.load() || TaskQ->isQueueShutdown()) {
        std::cerr << PoolName << "[任务队列改变容量] 设置失败：无效状态" << std::endl;
        return;
    }
    TaskQ->setMaxCapacity(capacity);
}

void ThreadPool::workerFunc() {
    std::thread::id curTid = std::this_thread::get_id(); // 获取当前线程ID
    std::cout << PoolName << "[核心线程] 线程启动，tid=" << curTid << std::endl;

    try {
        // 循环：未关闭则持续取任务（关闭后退出）
        while (!shutdown.load()) {
            // 超时等待：1秒超时，避免永久阻塞（多池竞争时公平性）
            std::unique_lock<std::mutex> lock(poolMutex);
            // 等待条件：队列非空 / 线程池关闭 / 缩容（CACHED模式）
            bool waitResult = notEmpty.wait_for(lock, std::chrono::seconds(1), [this]() {
                return !TaskQ->empty() || shutdown.load() || (exitNum.load() > 0 && liveNum.load() > minNum.load());
                });

            // 优先检测关闭信号：线程池关闭→退出
            if (shutdown.load()) {
                lock.unlock();
                break;
            }

            // CACHED模式缩容：当前线程是待退出线程（exitNum>0且存活数>核心数）
            if (exitNum.load() > 0 && liveNum.load() > minNum.load()) {
                exitNum--; // 待退出数减1
                liveNum--; // 存活数减1
                lock.unlock();
                std::cout << PoolName << "[核心线程] 缩容退出，tid=" << curTid << "，剩余线程数=" << liveNum.load() << std::endl;
                break;
            }

            // 超时且队列空：重试（避免永久阻塞）
            if (!waitResult && TaskQ->empty()) {
                lock.unlock();
                continue;
            }

            // 从队列取任务（非阻塞，已加锁）
            auto taskOpt = TaskQ->taskTake(ThreadPoolPrivateKey{});
            lock.unlock(); // 解锁：任务执行不需要持有poolMutex

            // 无任务：继续循环
            if (!taskOpt.has_value()) {
                continue;
            }

            // 执行任务：用BusyNumGuard自动管理忙线程数
            Task task = std::move(taskOpt.value());
            {
                BusyNumGuard guard(busyNum, busyMutex); // 忙线程数+1
                std::any result;
                bool hasResult = false;
                // 任务开始时间
                auto taskStart = std::chrono::steady_clock::now();
                try {
                    result = task.func(); // 执行任务，获取结果
                    hasResult = true;
                }
                catch (...) {
                    TaskQ->recordFailTaskCount(ThreadPoolPrivateKey{});
                    // 捕获任务执行异常，传递给future
                    if (task.promise != nullptr) {
                        task.promise->set_exception(std::current_exception());
                    }
                    std::cerr << PoolName << "[核心线程] 任务异常，tid=" << curTid << std::endl;
                }
                // 任务结束时间
                auto taskEnd = std::chrono::steady_clock::now();
                // 消耗时间
                int64_t costUs = (int64_t)std::chrono::duration_cast<std::chrono::microseconds>(taskEnd - taskStart).count();
                TaskQ->recordTaskFinish(ThreadPoolPrivateKey{}, costUs);
                TaskResult tr;
                tr.isTimeout = (task.m_timeout.count() != 0 &&
                    costUs > std::chrono::duration_cast<std::chrono::microseconds>(task.m_timeout).count());
                if (hasResult) {
                    tr.value = std::move(result);
                }
                // 有限时间且超时了  超时算成功执行完任务
                if (tr.isTimeout) {
                    TaskQ->recordTimeoutTaskCount(ThreadPoolPrivateKey{});
                }
                task.promise->set_value(std::move(tr));
            } // BusyNumGuard析构：忙线程数-1

            std::cout << PoolName << "[核心线程] 任务完成，tid=" << curTid << "，剩余任务数=" << TaskQ->getCurrentSize() << std::endl;
        }
    }
    catch (const std::exception& e) {
        // 捕获线程执行异常（如锁异常）
        std::cerr << PoolName << "[核心线程] 线程异常退出，tid=" << curTid << "，原因：" << e.what() << std::endl;
        liveNum--; // 存活数减1（异常退出）
    }
    catch (...) {
        // 捕获未知异常
        std::cerr << PoolName << "[核心线程] 线程未知异常退出，tid=" << curTid << std::endl;
        liveNum--;
    }

    // 标记线程为已完成
    std::lock_guard<std::mutex> lock(poolMutex);
    for (auto& worker : workersThread) {
        if (worker.tid == curTid) {
            worker.isFinish = true;
            break;
        }
    }

    std::cout << PoolName << "[核心线程] 线程退出，tid=" << curTid << std::endl;
    if (poolmode == PoolMode::FIXED && !shutdown.load()) {
        std::cout << PoolName << "发送线程退出信号，信号量计数+1" << std::endl;
        fixedRebuildSem.release(); // 也就是post
    }
}

void ThreadPool::fixedGuardFunc() {
    std::thread::id curTid = std::this_thread::get_id();
    std::cout << PoolName << "[守护者] [启动]，tid=" << curTid << std::endl;
    while (!shutdown.load()) {
        fixedRebuildSem.acquire(); // 也就是wait
        if (shutdown.load()) {
            break;
        }

        if (liveNum.load() < maxNum.load()) {
            std::lock_guard<std::mutex> lock(poolMutex);
            std::cout << PoolName << "[守护者] 收到线程退出信号，开始清理+补充" << std::endl;
            workersThread.emplace_back(
                std::thread([this]() { this->workerFunc(); })
            );
            liveNum++;
            std::cout << PoolName << "[守护者] [rebuild]，tid=" << workersThread.back().tid << std::endl;
        }
        {
            std::lock_guard<std::mutex> lock(poolMutex); // 加锁保护线程列表
            auto it = workersThread.begin();
            while (it != workersThread.end()) {
                if (it->isFinish) {
                    std::thread::id tid = it->tid;
                    // 回收线程系统资源：必须join，避免僵尸线程
                    if (it->thread.joinable()) {
                        it->thread.join();
                        std::cout << PoolName << "[守护者][清理] 回收已完成线程，tid=" << tid << std::endl;
                    }
                    // 从列表中删除线程状态对象：释放容器内存
                    it = workersThread.erase(it);
                    std::cout << PoolName << "[守护者][清理] 删除线程状态对象，tid=" << tid << std::endl;
                }
                else {
                    ++it;
                }
            }
        }
    }
    std::cout << PoolName << "[守护者线程] [退出]，tid=" << curTid << std::endl;
}

void ThreadPool::managerFunc() {
    std::thread::id curTid = std::this_thread::get_id();
    std::cout << PoolName << "[管理者] [启动]，tid=" << curTid << std::endl;

    // 循环：未关闭且队列未关闭→持续监控
    while (!shutdown.load() && !TaskQ->isQueueShutdown()) {
        std::this_thread::sleep_for(std::chrono::seconds(3)); // 每3秒监控一次（避免频繁检查）

        // 获取当前线程池状态（原子变量加载，线程安全）
        int curLive = liveNum.load();
        int curBusy = busyNum.load();
        int curQueue = TaskQ->getCurrentSize();

        // 打印监控日志
        std::cout << PoolName << "\n[管理者] [监控]：存活=" << curLive << "，忙=" << curBusy << "，队列任务数=" << curQueue << std::endl;

        // 扩容逻辑：
        // 1. 存活数 < 核心数（异常情况，补充核心线程）
        // 2. 队列任务数 > 存活数（任务积压）且 存活数 < 最大数（未达扩容上限）
        if (curLive < minNum.load() || (curQueue > curLive && curLive < maxNum.load())) {
            std::lock_guard<std::mutex> lock(poolMutex); // 加锁保护线程列表
            int addCount = 0;
            int targetLive = std::min(curLive + ADDCOUNT, maxNum.load()); // 扩容目标（不超过最大数）
            // 循环创建线程，直到达到目标或任务积压缓解
            while (curLive < targetLive && (TaskQ->getCurrentSize() > curLive || curLive < minNum.load())) {
                workersThread.emplace_back(
                    std::thread([this]() { this->workerFunc(); })
                );
                curLive++;
                addCount++;
                liveNum++; // 存活数原子递增
                std::cout << PoolName << "[管理者] [扩容]，tid=" << workersThread.back().tid << "，当前存活=" << curLive << std::endl;
            }
        }

        // 缩容逻辑：忙线程数 * 2 < 存活数（线程空闲过多）且 存活数 > 核心数（未低于核心数）
        if (curBusy * 2 < curLive && curLive > minNum.load()) {
            std::lock_guard<std::mutex> lock(poolMutex); // 加锁保护exitNum
            int needExit = std::min(ADDCOUNT, curLive - minNum.load()); // 待退出数（不超过空闲数）
            if (needExit > 0) {
                exitNum += needExit; // 标记待退出线程数
                TaskQ->wakeupAllBoundPools(); // 唤醒线程，触发缩容判断
                std::cout << PoolName << "[管理者] [缩容]，标记" << needExit << "个线程退出" << std::endl;
            }
        }

        // 清理已完成线程：移除isFinish=true且不可join的线程（避免列表冗余）
        {
            std::lock_guard<std::mutex> lock(poolMutex); // 加锁保护线程列表
            auto it = workersThread.begin();
            int cleanCount = 0;
            while (it != workersThread.end()) {
                // 仅清理已标记为完成的线程（isFinish=true）
                if (it->isFinish) {
                    std::thread::id tid = it->tid;
                    // 回收线程系统资源：必须join，避免僵尸线程
                    if (it->thread.joinable()) {
                        it->thread.join();
                        std::cout << PoolName << "[管理者][清理] 回收已完成线程，tid=" << tid << std::endl;
                    }
                    // 从列表中删除线程状态对象：释放容器内存
                    it = workersThread.erase(it);
                    cleanCount++;
                    std::cout << PoolName << "[管理者][清理] 删除线程状态对象，tid=" << tid << std::endl;
                }
                else {
                    ++it;
                }
            }

            if (cleanCount > 0) {
                std::cout << PoolName << "[管理者][清理] 本次清理" << cleanCount << "个已完成线程，剩余列表大小=" << workersThread.size() << std::endl;
            }
        }
    }
    std::cout << PoolName << "[管理者线程] [退出]，tid=" << curTid << std::endl;
}
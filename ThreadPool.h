#ifndef H_THREADPOOL
#define H_THREADPOOL

// 标准库头文件：涵盖线程、同步、容器、类型转换等核心功能
#include <iostream>
#include <vector>
#include <queue>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <utility>
#include <functional>
#include <future>
#include <any>
#include <algorithm>
#include <string>
#include <exception>
#include <limits>
#include <semaphore>
#include <optional>

struct TaskResult {
    bool isTimeout = false;
    std::any value = { 0 };
};

// 总用时
using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;
using Us = std::chrono::microseconds;

// 线程池钥匙，给队列调用
class ThreadPool;
class ThreadPoolPrivateKey {
    friend class ThreadPool; // 只放行线程池构造
    ThreadPoolPrivateKey() = default;
};

// 线程池工作模式枚举：支持两种核心调度策略
enum class PoolMode {
    FIXED,   // 固定线程数模式：核心线程数=最大线程数，无动态扩缩容
    CACHED   // 缓存线程数模式：支持动态扩缩容，根据任务量调整线程数
};

// 任务封装结构体：统一管理任意类型任务（支持有/无返回值、异常传递）
struct Task {
    enum PriorityLevel {
        LOW,
        MEDIUM,
        HIGH
    };

    PriorityLevel m_level = LOW;
    std::function<std::any()> func; // 类型擦除后的任务函数：无参数，返回any（兼容任意返回值）
    std::shared_ptr<std::promise<TaskResult>> promise; // 用于传递任务结果/异常的promise（共享所有权）
    Ms m_timeout{ 0 }; // 初始化为不限时间

    Task() = default; // 默认构造函数

    // 模板构造函数：接收任意函数+任意参数，封装为统一的无参任务
    template<typename Func, typename... Args>
    explicit Task(Task::PriorityLevel level, std::chrono::milliseconds timeout, Func&& func, Args&& ... args)
        : m_level(level)
        , m_timeout(timeout) {
        // 完美转发参数，绑定为无参lambda（类型擦除核心）
        this->func = [func = std::forward<Func>(func), ... args = std::forward<Args>(args)]() -> std::any {
            // 编译期判断任务返回值是否为void：避免无返回值任务的类型兼容问题
            if constexpr (std::is_same_v<std::invoke_result_t<Func, Args...>, void>) {
                std::invoke(func, args...); // 执行无返回值任务
                return std::any(); // 返回空any，统一接口
            }
            else {
                return std::invoke(func, args...); // 执行有返回值任务，结果存入any
            }
            };
        // 创建promise，与future绑定（用户通过future获取结果结构体）
        this->promise = std::make_shared<std::promise<TaskResult>>();
    }

    // 移动构造函数：支持任务对象的移动语义（避免拷贝开销）
    Task(Task&& other) noexcept;

    // 移动赋值运算符：支持任务对象的移动赋值
    Task& operator=(Task&& other) noexcept;

    // 禁止拷贝构造和拷贝赋值：任务对象不可拷贝（避免资源竞争）
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
};

// 添加优先任务基类来实现优先顺序变换功能
struct BaseTaskQuene {
    virtual ~BaseTaskQuene() = default;
    virtual bool empty() const = 0;
    virtual std::size_t size() const = 0;
    virtual const Task& top() const = 0;
    virtual void pop() = 0;
    virtual void push(Task&& task) = 0;
};

// 大根堆类
struct MaxHeapTaskQueue : BaseTaskQuene {
    struct Cmp {
        bool operator()(const Task& a, const Task& b) const { // less <号
            return a.m_level < b.m_level;
        }
    };
    std::priority_queue<Task, std::vector<Task>, Cmp> que;
    bool empty() const override;
    std::size_t size() const override;
    const Task& top() const override;
    void pop() override;
    void push(Task&& task) override;
};

// 小根堆类
struct MinHeapTaskQueue : BaseTaskQuene {
    struct Cmp {
        bool operator()(const Task& a, const Task& b) const { // greater >号
            return a.m_level > b.m_level;
        }
    };
    std::priority_queue<Task, std::vector<Task>, Cmp> que;
    bool empty() const override;
    std::size_t size() const override;
    const Task& top() const override;
    void pop() override;
    void push(Task&& task) override;
};

// 线程安全的任务队列：支持多线程池共享、容量控制、自动唤醒
class TaskQueue {
public:
    enum PriorityMode {
        MinHeap,  // 小根堆 LOW优先
        MaxHeap   // 大根堆 HIGH优先
    };

private:
    std::unique_ptr<BaseTaskQuene> taskQ;
    std::atomic_int queueSize = 0;         // 队列中任务数量（原子变量，线程安全读写）
    std::atomic_int maxCapacity = std::numeric_limits<int>::max(); // 队列最大容量（默认无上限）
    std::mutex queueMutex;                 // 保护队列操作的互斥锁（避免数据竞争）
    std::vector<std::condition_variable*> notifyConds; // 多线程池共享核心：存储所有关联线程池的条件变量
    std::atomic_bool queueShutdown = false; // 队列关闭标志（独立于线程池，拒绝新任务）
    PriorityMode m_mode = MaxHeap;

    // 统计
    std::atomic<uint64_t> m_total_task_cnt{ 0 };   // 总执行任务数
    std::atomic<uint64_t> m_fail_task_cnt{ 0 };     // 异常失败任务数
    std::atomic<uint64_t> m_timeout_task_cnt{ 0 };  // 超时任务数
    std::atomic<int64_t>  m_total_cost_us{ 0 };     // 所有任务总耗时（微秒）
    std::atomic<int64_t>  m_max_cost_us{ 0 };       // 单任务最大耗时（微秒）

public:
    // 无限大模式但是设置模式构造
    explicit TaskQueue(PriorityMode mode = MaxHeap);

    // 带初始容量的构造函数：指定队列最大容量
    explicit TaskQueue(int initCapacity, PriorityMode mode = MaxHeap);

    ~TaskQueue() = default; // 析构函数：无需额外操作（容器自动释放）

    // 绑定线程池的条件变量：多线程池共享时，每个线程池必须调用此接口
    void bindConditionVariable(std::condition_variable* cond);

    // 解绑线程池的条件变量：线程池销毁/关机时调用，避免野指针
    void unbindConditionVariable(std::condition_variable* cond);

    // 核心接口：提交任意任务（兼容有/无返回值），返回future供用户获取结果
    template<typename Func, typename... Args>
    std::future<TaskResult> submitTask(Task::PriorityLevel level, std::chrono::milliseconds timeout, Func&& func, Args&&... args) {
        // 队列已关闭：拒绝提交新任务
        if (queueShutdown.load()) {
            std::cerr << "[任务队列] 队列已关闭，拒绝提交任务" << std::endl;
            std::promise<TaskResult> emptyPromise;
            emptyPromise.set_exception(std::make_exception_ptr(std::runtime_error("TaskQueue is shutdown")));
            return emptyPromise.get_future();
        }

        // 队列已满：拒绝提交新任务（避免内存溢出）
        if (isQueueFull()) {
            std::cerr << "[任务队列] 队列已满（当前：" << queueSize.load() << "，最大：" << maxCapacity << "），拒绝提交" << std::endl;
            std::promise<TaskResult> emptyPromise;
            emptyPromise.set_exception(std::make_exception_ptr(std::runtime_error("TaskQueue is full")));
            return emptyPromise.get_future();
        }

        // 封装任务：创建Task对象，绑定函数和参数
        Task task(level, timeout, std::forward<Func>(func), std::forward<Args>(args)...);
        std::future<TaskResult> future = task.promise->get_future(); // 获取与promise绑定的future

        // 任务入队：加锁保证线程安全
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            taskQ->push(std::move(task)); // 移动语义，避免拷贝
            queueSize++; // 任务数原子递增
        }

        wakeupAllBoundPools(); // 唤醒所有关联线程池的空闲线程（多池共享核心）
        return future;
    }

    // 工作线程调用：从队列取出任务（非阻塞，无任务返回nullopt）
    std::optional<Task> taskTake(ThreadPoolPrivateKey key);

    // 唤醒所有关联线程池的一个空闲线程：任务入队时调用，确保所有线程池感知任务
    void wakeupAllBoundPools();

    // 强制唤醒所有关联线程池的所有线程：线程池/队列关闭时调用，确保线程正常退出
    void wakeupAllThreadsInBoundPools();

    // 关闭队列：拒绝新任务，唤醒所有线程处理剩余任务
    void shutdownQueue();

    // 判断队列是否已关闭：供外部线程池检查状态
    bool isQueueShutdown() const;

    // 设置队列最大容量：支持动态调整（运行时可修改）
    void setMaxCapacity(int capacity);

    // 获取当前队列任务数（线程安全）
    int getCurrentSize() const;

    // 获取队列最大容量（线程安全）
    int getMaxCapacity() const;

    // 判断队列是否满
    bool isQueueFull();

    // 判断队列是否为空（线程安全）
    bool empty();

    void changeMode(PriorityMode mode);

    // 对线程池记录操作
    // 总执行任务数加一,累加所有任务耗时
    void recordTaskFinish(ThreadPoolPrivateKey key, int64_t us);

    // 异常失败任务数数加一
    void recordFailTaskCount(ThreadPoolPrivateKey key);

    // 超时任务数加一
    void recordTimeoutTaskCount(ThreadPoolPrivateKey key);

    // 对外查询记录接口
    uint64_t getTotalTaskCount() const;
    uint64_t getFailTaskCount() const;
    uint64_t getTimeoutTaskCount() const;
    uint64_t getMaxCostUs() const;
    double getAvgCostUs() const;

    // 禁止拷贝和移动：队列是核心资源，不可拷贝
    TaskQueue(const TaskQueue&) = delete;
    TaskQueue& operator=(const TaskQueue&) = delete;
    TaskQueue(TaskQueue&&) = delete;
    TaskQueue& operator=(TaskQueue&&) = delete;
};

// 线程池核心类：管理工作线程、调度任务、动态扩缩容（支持多池共享任务队列）
class ThreadPool {
private:
    // 工作线程状态结构体：存储线程对象、是否完成、线程ID
    struct WorkerStatus {
        std::thread thread;       // 工作线程对象
        std::atomic_bool isFinish; // 线程是否已完成（用于清理）
        std::thread::id tid;      // 线程ID（用于标识和调试）

        WorkerStatus() : isFinish(false), tid(std::thread::id()) {} // 默认构造

        // 带线程对象的构造函数：移动语义接收线程
        WorkerStatus(std::thread&& t);

        // 移动构造函数：支持WorkerStatus对象的移动
        WorkerStatus(WorkerStatus&& other) noexcept;

        // 移动赋值运算符：支持WorkerStatus对象的移动赋值
        WorkerStatus& operator=(WorkerStatus&& other) noexcept;

        // 禁止拷贝：线程对象不可拷贝
        WorkerStatus(const WorkerStatus&) = delete;
        WorkerStatus& operator=(const WorkerStatus&) = delete;
    };

    // RAII嵌套类：自动管理忙线程数（构造+1，析构-1，异常安全）
    class BusyNumGuard {
    public:
        // 构造：忙线程数+1（加锁保证线程安全）
        BusyNumGuard(std::atomic_int& busyNum, std::mutex& lock);

        // 析构：忙线程数-1（无论正常返回还是异常，都会执行）
        ~BusyNumGuard();

        // 禁止拷贝：RAII对象不可拷贝
        BusyNumGuard(const BusyNumGuard&) = delete;
        BusyNumGuard& operator=(const BusyNumGuard&) = delete;

    private:
        std::atomic_int& m_busyNum; // 引用线程池的忙线程数
        std::mutex& m_lock;         // 保护忙线程数的互斥锁
    };

private:
    std::atomic_int liveNum = 0;     // 存活的工作线程数（原子变量，线程安全）
    std::atomic_int busyNum = 0;     // 正在执行任务的忙线程数（原子变量）
    std::atomic_int exitNum = 0;     // 待退出的线程数（CACHED模式缩容用）
    std::atomic_int minNum = 0;      // 核心线程数（最小存活线程数）
    std::atomic_int maxNum = 0;      // 最大线程数（CACHED模式上限）
    std::atomic_bool shutdown = false; // 线程池关闭标志（原子变量）

    std::string PoolName;            // 线程池名称（用于日志和调试）
    std::mutex poolMutex;            // 保护线程池状态的互斥锁（如工作线程列表）
    std::mutex busyMutex;            // 保护忙线程数的互斥锁（配合BusyNumGuard）
    std::condition_variable notEmpty; // 线程池的条件变量（任务队列非空时唤醒）

    std::thread managerThread;       // 管理者线程（CACHED模式：动态扩缩容）
    std::vector<WorkerStatus> workersThread; // 工作线程列表（存储所有工作线程）
    std::thread fixedGuardThread;    // fixed管理者

    TaskQueue* TaskQ = nullptr;      // 关联的任务队列（支持多池共享）
    PoolMode poolmode;               // 线程池工作模式（FIXED/CACHED）
    int ADDCOUNT = 2;                // CACHED模式每次扩容的线程数
    bool isInitTrue = false;         // 线程池是否初始化成功

    std::counting_semaphore<10> fixedRebuildSem{ 0 }; // 信号量

public:
    // FIXED模式构造函数：固定线程数（核心线程数=最大线程数）
    ThreadPool(std::string name, TaskQueue* taskq, PoolMode mode, int livenum = std::thread::hardware_concurrency());

    // CACHED模式构造函数：动态扩缩容（核心线程数+最大线程数）
    ThreadPool(std::string name, TaskQueue* taskq, PoolMode mode, int min, int max);

    // 析构函数：核心！自动解绑队列关联+关闭线程池（确保资源回收）
    ~ThreadPool();

    // 核心接口：提交任务（对外暴露，兼容任意函数+参数）
    template<typename Func, typename... Args>
    std::future<TaskResult> submitTask(Func&& func, Args&&... args) {
        return submitTask(Task::PriorityLevel::LOW, 0ms, std::forward<Func>(func), std::forward<Args>(args)...);
    }

    template<typename Func, typename... Args>
    std::future<TaskResult> submitTask(Task::PriorityLevel level, std::chrono::milliseconds timeout, Func&& func, Args&&... args) {
        // 合法性检查：未初始化、已关闭、队列空、队列已关闭 → 拒绝提交
        if (!isInitTrue || shutdown.load() || TaskQ == nullptr || TaskQ->isQueueShutdown()
            || TaskQ->isQueueFull()) {
            std::cerr << PoolName << "[提交任务] 线程池无效或队列已关闭，拒绝提交" << std::endl;
            std::promise<TaskResult> emptyPromise;
            emptyPromise.set_exception(std::make_exception_ptr(std::runtime_error("Submit failed")));
            return emptyPromise.get_future();
        }
        // 委托给任务队列的submitTask（统一逻辑）
        return TaskQ->submitTask(level, timeout, std::forward<Func>(func), std::forward<Args>(args)...);
    }

    // 关闭线程池：优雅退出，回收所有线程（可手动调用，析构时自动调用）
    void shutdownPool();

    // 辅助接口：获取线程池名称（用于测试和日志）
    std::string getPoolName() const;

    // 重启线程池：仅支持已关闭且队列未关闭的线程池
    void resumePool();

    // 设置CACHED模式每次扩容的线程数
    void setEverytimeAddCount(int num);

    // 线程池重命名：仅支持已关闭的线程池
    void Rename(std::string name);

    // 获取存活线程数（对外接口）
    int getLiveNum() const;

    // 获取忙线程数（对外接口）
    int getBusyNum() const;

    // 获取队列当前任务数（对外接口）
    int getQueueCurrentSize() const;

    // 获取队列最大容量（对外接口）
    int getQueueMaxCapacity() const;

    // 转换模式，统一交给队列处理
    void changeMode(TaskQueue::PriorityMode mode);

    // 设置队列最大容量（对外接口）
    void setQueueMaxCapacity(int capacity);

private:
    // 工作线程核心逻辑：循环取任务→执行任务→处理结果/异常
    void workerFunc();

    // FIEXD模式守护者(对标managerFunc以解决因异常而退出线程没有及时删除和补充的)
    void fixedGuardFunc();

    // 管理者线程逻辑（仅CACHED模式）：动态扩缩容、清理已完成线程
    void managerFunc();
};

#endif // !H_THREADPOOL
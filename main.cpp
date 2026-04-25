#include "ThreadPool.h"
#include <iostream>
#include <vector>
#include <chrono>
#include <string>
#include <cassert>
#include <thread>
#include <atomic>

using namespace std;
using namespace std::chrono;

void printTestSeparator(const string& testName) {
    cout << "\n==================================== " << testName << " ====================================" << endl;
}

#if 1
// ------------------------------
// 测试1：基础功能（无返回值 / 有返回值）
// ------------------------------
void testBasicFunction() {
    printTestSeparator("测试1：基础功能测试");
    TaskQueue queue(5);
    ThreadPool pool("BasicPool", &queue, PoolMode::FIXED, 2);

    // 无返回值
    auto f1 = pool.submitTask(Task::LOW, 0ms, []() {
        this_thread::sleep_for(200ms);
        cout << "无返回值任务执行完成" << endl;
        });
    f1.get();

    // int 返回值
    auto f2 = pool.submitTask(Task::LOW, 0ms, [](int a, int b) {
        this_thread::sleep_for(200ms);
        return a + b;
        }, 10, 20);
    auto res2 = f2.get();
    int sum = any_cast<int>(res2.value);
    assert(sum == 30);
    cout << "10+20 = " << sum << "  断言通过" << endl;

    // string 返回值
    auto f3 = pool.submitTask(Task::LOW, 0ms, [](const string& a, const string& b) {
        return a + b;
        }, "Hello ", "ThreadPool");
    auto res3 = f3.get();
    string str = any_cast<string>(res3.value);
    assert(str == "Hello ThreadPool");
    cout << "字符串结果：" << str << "  断言通过" << endl;

    pool.shutdownPool();
    cout << "测试1 通过！" << endl;
}
#endif
#if 1
// ------------------------------
// 测试2：多线程池共享队列
// ------------------------------
void testMultiPoolShareQueue() {
    printTestSeparator("测试2：多线程池共享任务队列");

    TaskQueue q(10);
    ThreadPool p1("Pool1", &q, PoolMode::FIXED, 2);
    ThreadPool p2("Pool2", &q, PoolMode::FIXED, 3);
    ThreadPool p3("Pool3", &q, PoolMode::CACHED, 1, 4);

    vector<future<TaskResult>> futures;
    atomic<int> cnt = 0;

    for (int i = 0; i < 10; ++i) {
        futures.push_back(q.submitTask(Task::LOW, 0ms, [i, &cnt]() {
            this_thread::sleep_for(200ms);
            cout << "共享任务 " << i << " 完成" << endl;
            cnt++;
            }));
    }

    for (auto& f : futures) f.get();
    assert(cnt == 10);
    cout << "所有共享任务执行完成" << endl;

    p1.shutdownPool();
    p2.shutdownPool();
    p3.shutdownPool();
    cout << "测试2 通过！" << endl;
}
#endif
#if 1
// ------------------------------
// 测试3：Cached 模式动态扩缩容
// ------------------------------
void testCachedModeScale() {
    printTestSeparator("测试3：CACHED 模式扩缩容");

    TaskQueue q(20);
    ThreadPool pool("CachedPool", &q, PoolMode::CACHED, 1, 5);
    pool.setEverytimeAddCount(1);

    vector<future<TaskResult>> futures;
    for (int i = 0; i < 8; ++i) {
        futures.push_back(pool.submitTask([]() {
            this_thread::sleep_for(2s);
            }));
    }

    this_thread::sleep_for(4s);
    int live = pool.getLiveNum();
    cout << "扩容后线程数：" << live << endl;
    assert(live > 1 && live <= 5);

    for (auto& f : futures) f.get();
    this_thread::sleep_for(6s);
    cout << "缩容后线程数：" << pool.getLiveNum() << endl;
    assert(pool.getLiveNum() == 1);

    pool.shutdownPool();
    cout << "测试3 通过！" << endl;
}
#endif
#if 1
// ------------------------------
// 测试4：队列满拒绝策略
// ------------------------------
void testQueueCapacityLimit() {
    printTestSeparator("测试4：队列容量限制 & 拒绝策略");

    const int MAX_Q = 5;
    TaskQueue q(MAX_Q);
    ThreadPool pool("LimitPool", &q, PoolMode::FIXED, 1);

    vector<future<TaskResult>> futures;
    int reject = 0;

    for (int i = 0; i < 10; ++i) {
        futures.push_back(pool.submitTask([]() {
            this_thread::sleep_for(1s);
            }));
    }

    for (auto& f : futures) {
        try {
            f.get();
        }
        catch (...) {
            reject++;
        }
    }

    cout << "被拒绝任务数：" << reject << endl;
    cout << "测试4 通过！" << endl;
}
#endif
#if 1
// ------------------------------
// 测试5：任务异常传递
// ------------------------------
void testExceptionTransfer() {
    printTestSeparator("测试5：任务异常传递");

    TaskQueue q(5);
    ThreadPool pool("ExPool", &q, PoolMode::FIXED, 2);

    auto f = pool.submitTask([]() {
        this_thread::sleep_for(200ms);
        throw runtime_error("测试异常");
        });

    try {
        f.get();
    }
    catch (const runtime_error& e) {
        cout << "成功捕获异常：" << e.what() << endl;
    }

    pool.shutdownPool();
    cout << "测试5 通过！" << endl;
}
#endif
#if 1
// ------------------------------
// 测试6：线程池重启、析构、解绑
// ------------------------------
void testPoolDestructUnbind() {
    printTestSeparator("测试6：析构解绑 & 重启");

    TaskQueue q(5);
    {
        ThreadPool p("TempPool", &q, PoolMode::FIXED, 2);
        p.submitTask([]() { this_thread::sleep_for(200ms); }).get();
    }

    ThreadPool p2("Pool2", &q, PoolMode::FIXED, 2);
    p2.shutdownPool();
    p2.resumePool();
    auto f = p2.submitTask([]() { return 123; });
    auto res = f.get();
    assert(any_cast<int>(res.value) == 123);

    p2.shutdownPool();
    cout << "测试6 通过！" << endl;
}
#endif
#if 1
// ------------------------------
// 测试7：队列关闭
// ------------------------------
void testQueueShutdown() {
    printTestSeparator("测试7：队列关闭测试");

    TaskQueue q(5);
    ThreadPool pool("QuitPool", &q, PoolMode::FIXED, 2);

    auto f1 = pool.submitTask([]() { this_thread::sleep_for(200ms); });
    auto f2 = pool.submitTask([]() { this_thread::sleep_for(200ms); });
    q.shutdownQueue();

    auto f3 = pool.submitTask([]() {});
    try {
        f3.get();
    }
    catch (...) {
        cout << "队列关闭，新任务被拒绝 ?" << endl;
    }

    f1.get();
    f2.get();
    assert(q.empty());
    cout << "测试7 通过！" << endl;
}
#endif
#if 1
// ------------------------------
// 测试8：高并发测试
// ------------------------------
void testHighConcurrency() {
    printTestSeparator("测试8：高并发压力测试");

    TaskQueue q(100);
    ThreadPool p1("P1", &q, PoolMode::FIXED, 3);
    ThreadPool p2("P2", &q, PoolMode::CACHED, 4, 4);

    vector<future<TaskResult>> futures;
    atomic<int> ok = 0;

    for (int i = 0; i < 100; ++i) {
        futures.push_back(p1.submitTask([&ok]() {
            this_thread::sleep_for(100ms);
            ok++;
            }));
    }

    for (auto& f : futures) f.get();
    assert(ok == 100);
    cout << "100个并发任务全部完成" << endl;

    p1.shutdownPool();
    p2.shutdownPool();
    cout << "测试8 通过！" << endl;
}
#endif
#if 1
// ------------------------------
// 测试9：优先级队列（MAX <-> MIN）
// ------------------------------
void testPriorityQueue() {
    printTestSeparator("测试9：优先级队列切换测试");

    TaskQueue q(100, TaskQueue::MaxHeap);
    ThreadPool pool("PriorityPool", &q, PoolMode::FIXED, 3);

    vector<future<TaskResult>> futures;

    // 先塞一堆低优先级
    for (int i = 0; i < 10; ++i) {
        futures.push_back(pool.submitTask(Task::LOW, 0ms, [i]() {
            cout << "[ LOW ] 任务执行 " << i << endl;
            this_thread::sleep_for(500ms);
            }));
    }

    // 再塞高优先级
    for (int i = 0; i < 10; ++i) {
        futures.push_back(pool.submitTask(Task::HIGH, 0ms, [i]() {
            cout << "[ HIGH ] 任务执行 " << i << endl;
            this_thread::sleep_for(500ms);
            }));
    }

    this_thread::sleep_for(3s);
    cout << "\n----- 切换为低优先级优先（MIN 堆）-----\n";
    q.changeMode(TaskQueue::MinHeap);

    for (auto& f : futures) f.get();
    cout << "测试9 通过！" << endl;
}
#endif
#if 1
// ------------------------------
// 测试10：执行超时检测（你最核心新功能）
// ------------------------------
void testTimeout() {
    printTestSeparator("测试10：任务执行超时测试");

    TaskQueue q(10);
    ThreadPool pool("TimeoutPool", &q, PoolMode::FIXED, 2);

    // 不超时
    auto f1 = pool.submitTask(Task::LOW, 1000ms, []() {
        this_thread::sleep_for(200ms);
        return 666;
        });

    // 超时（限定500ms，睡1000ms）
    auto f2 = pool.submitTask(Task::HIGH, 500ms, []() {
        this_thread::sleep_for(1000ms);
        });

    auto res1 = f1.get();
    auto res2 = f2.get();

    cout << "任务1 超时：" << boolalpha << res1.isTimeout << endl;
    cout << "任务2 超时：" << boolalpha << res2.isTimeout << endl;

    assert(res1.isTimeout == false);
    assert(res2.isTimeout == true);

    cout << "超时检测完全正确 ?" << endl;

    pool.shutdownPool();
    cout << "测试10 通过！" << endl;
}
#endif
#if 0
int main() {
    // 执行所有测试
    std::cout << "每一项测试都需要手动按下回车键后继续" << std::endl;
    std::getchar();
    testBasicFunction();
    std::cout << "按下回车键后继续下一项测试" << std::endl;
    std::getchar();
    testMultiPoolShareQueue();
    std::cout << "按下回车键后继续下一项测试" << std::endl;
    std::getchar();
    testCachedModeScale();
    std::cout << "按下回车键后继续下一项测试" << std::endl;
    std::getchar();
    testQueueCapacityLimit();
    std::cout << "按下回车键后继续下一项测试" << std::endl;
    std::getchar();
    testExceptionTransfer();
    std::cout << "按下回车键后继续下一项测试" << std::endl;
    std::getchar();
    testPoolDestructUnbind();
    std::cout << "按下回车键后继续下一项测试" << std::endl;
    std::getchar();
    testQueueShutdown();
    std::cout << "按下回车键后继续下一项测试" << std::endl;
    std::getchar();
    testHighConcurrency();
    std::cout << "按下回车键后继续下一项测试" << std::endl;
    std::getchar();
    testPriorityQueue();
    std::cout << "按下回车键后继续下一项测试" << std::endl;
    std::getchar();
    testTimeout();
    std::cout << "\n==================================== 所有测试全部通过！====================================" << std::endl;
    return 0;
}
#else //这是在测试关闭队列后线程池能否处理队内剩余的任务 
int add(int a, int b) {
    std::cout << "开始执行 add(" << a << ", " << b << ")" << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 稍微改短一点，测试更快
    return a + b;
}

int main() {
    std::cout << "========== 测试11：队列积压任务 + 线程池重启 ==========" << std::endl;

    // 1. 创建队列（容量100）
    TaskQueue taskq(100);

    // 注意：现在返回的是 std::future<TaskResult>
    std::vector<std::future<TaskResult>> futures;

    // 2. 创建线程池（FIXED模式，1个线程）
    ThreadPool pool("RebuildTestPool", &taskq, PoolMode::FIXED, 1);

    // 3. 立刻关闭线程池（模拟：池意外关闭，但队列还在）
    std::cout << "\n[操作] 立刻关闭线程池" << std::endl;
    pool.shutdownPool();

    // 4. 往队列里塞 10 个任务（队列还没关，可以正常入队）
    std::cout << "\n[操作] 往队列里提交 10 个任务（池已关闭，队列仍接收）" << std::endl;
    for (int i = 1; i <= 10; ++i) {
        // 注意：现在 submitTask 需要显式传 优先级 + 超时时间
        futures.emplace_back(
            taskq.submitTask(Task::PriorityLevel::LOW, 0ms, add, i, i + 100)
        );
    }

    // 5. 关闭队列（拒绝新任务，只处理积压）
    std::cout << "\n[操作] 关闭队列，拒绝新任务" << std::endl;
    taskq.shutdownQueue();

    // 6. 重启线程池（处理队列里积压的 10 个任务）
    std::cout << "\n[操作] 重启线程池，开始处理积压任务" << std::endl;
    pool.resumePool();

    // 7. 等待所有任务完成，求和
    int sum = 0;
    std::cout << "\n[等待] 等待所有任务执行完成并求和..." << std::endl;
    for (auto& f : futures) {
        // 注意：现在要先拿 TaskResult，再从 .value 里取 any
        TaskResult res = f.get();
        if (!res.isTimeout) {
            sum += std::any_cast<int>(res.value);
        }
    }

    // 验证结果：1+101 + 2+102 + ... + 10+110 = (1+...+10) + (101+...+110) = 55 + 1055 = 1110
    std::cout << "\n========== 程序结束，sum = " << sum << " (预期结果：1110) ==========" << std::endl;

    // 简单断言验证
    assert(sum == 1110);
    std::cout << "断言通过！测试成功！" << std::endl;

    return 0;
}
#endif
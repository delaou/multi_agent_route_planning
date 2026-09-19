#include <iostream>
#include <vector>
#include <chrono>
#include <thread>
#include "ThreadPool.h"

using namespace std;

// // 模拟计算任务
// int compute(int x) {
//     this_thread::sleep_for(chrono::milliseconds(100));
//     cout << "[Thread " << this_thread::get_id() << "] compute: " << x << endl;
//     return x * x;
// }

// // 无返回值任务
// void printTask(int id) {
//     this_thread::sleep_for(chrono::milliseconds(50));
//     cout << "[Thread " << this_thread::get_id() << "] printTask: " << id << endl;
// }

// // 返回字符串
// string stringTask(string msg) {
//     this_thread::sleep_for(chrono::milliseconds(80));
//     return "Processed: " + msg;
// }

// int main() {
//     cout << "===== ThreadPool Test Start =====" << endl;

//     ThreadPool pool(4);

//     vector<future<int>> results;

//     // =============================
//     // 1️⃣ 测试：带返回值（int）
//     // =============================
//     for (int i = 0; i < 10; ++i) {
//         results.emplace_back(
//             pool.enqueue(compute, i)
//         );
//     }

//     // =============================
//     // 2️⃣ 测试：无返回值（void）
//     // =============================
//     for (int i = 0; i < 5; ++i) {
//         pool.enqueue(printTask, i);
//     }

//     // =============================
//     // 3️⃣ 测试：lambda + string返回
//     // =============================
//     auto futureStr = pool.enqueue(stringTask, "Hello ThreadPool");

//     // =============================
//     // 4️⃣ 测试：lambda表达式
//     // =============================
//     auto futureLambda = pool.enqueue([](int a, int b) {
//         std::this_thread::sleep_for(std::chrono::milliseconds(120));
//         return a + b;
//     }, 3, 7);

//     // =============================
//     // 5️⃣ 获取结果
//     // =============================
//     cout << "\n===== Results =====" << endl;

//     for (auto& f : results) {
//         cout << "Result: " << f.get() << endl;
//     }

//     cout << "String result: " << futureStr.get() << endl;
//     cout << "Lambda result: " << futureLambda.get() << endl;

//     cout << "===== ThreadPool Test End =====" << endl;

//     return 0;
// }
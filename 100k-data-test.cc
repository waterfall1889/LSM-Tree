#include "kvstore.h"
#include <fstream>
#include <iostream>
#include <chrono>
#include <vector>

using namespace std;
using namespace std::chrono;

int main() {
    KVStore store("data");

    // 各阶段计时变量
    auto t_start = high_resolution_clock::now();

    auto t1 = high_resolution_clock::now();
    store.reset();
    auto t2 = high_resolution_clock::now();

    string string_path = "./data/cleaned_text_100k.txt";
    string vector_path = "./data/embedding_100k.txt";

    auto t3 = high_resolution_clock::now();
    store.read_build_from_text(string_path, vector_path, 2000);
    auto t4 = high_resolution_clock::now();

    cout << "Finished reading from text!" << endl;

    vector<long long> knn_times;
    vector<long long> hnsw_times;

    auto t5 = high_resolution_clock::now();
    for (int i = 0; i < 100; ++i) {
        string target = store.get(i);

        // knn 计时
        auto q1 = high_resolution_clock::now();
        store.search_knn(target, 3);
        auto q2 = high_resolution_clock::now();
        knn_times.push_back(duration_cast<microseconds>(q2 - q1).count());

        // hnsw 计时
        auto q3 = high_resolution_clock::now();
        store.search_knn_hnsw(target, 3);
        auto q4 = high_resolution_clock::now();
        hnsw_times.push_back(duration_cast<microseconds>(q4 - q3).count());
    }
    auto t6 = high_resolution_clock::now();

    // 平均耗时
    long long avg_knn = 0, avg_hnsw = 0;
    for (int i = 0; i < 100; ++i) {
        avg_knn += knn_times[i];
        avg_hnsw += hnsw_times[i];
    }
    avg_knn /= 100;
    avg_hnsw /= 100;

    // 总耗时
    auto t_end = high_resolution_clock::now();

    cout << "\n================== TIME REPORT ==================\n";
    cout << "[1] reset() time: " 
         << duration_cast<milliseconds>(t2 - t1).count() << " ms\n";
    cout << "[2] read_build_from_text() time: " 
         << duration_cast<milliseconds>(t4 - t3).count() << " ms\n";
    cout << "[3] Total 100 queries time: " 
         << duration_cast<milliseconds>(t6 - t5).count() << " ms\n";
    cout << "    ├─ Average KNN search:  " << avg_knn  << " μs\n";
    cout << "    └─ Average HNSW search: " << avg_hnsw << " μs\n";
    cout << "[4] Overall program time: " 
         << duration_cast<milliseconds>(t_end - t_start).count() << " ms\n";
    cout << "Total size: "<< store.vectorMap.size() << endl;
    cout << "=================================================\n";

    return 0;
}


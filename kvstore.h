#pragma once

#include "kvstore_api.h"
#include "skiplist.h"
#include "sstable.h"
#include "sstablehead.h"
#include "threadpool.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <future>
#include <iostream>
#include <map>
#include <queue>
#include <random>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class SearchLayers {
public:
    int all_counts        = 0;
    int M                 = 14; // 每个节点的最小连接数
    int M_max             = 18; // 每个节点的最大连接数
    int m_L               = 6;  // 最大层数
    int efConstruction    = 70; // 构建时的候选集合大小
    const std::string DEL = "~DELETED~";

    struct Node {
        uint32_t nodeID;
        uint64_t nodeKey;
        std::string value;
        std::vector<float> data;
        std::vector<std::vector<Node *>> connections; // 分层连接
        std::vector<Node *> nearPoints;
        int level;
        int isValid;

        Node(uint64_t k, const std::string &val, const std::vector<float> &var, int level) :
            nodeKey(k),
            value(val),
            data(var),
            level(level) {
            connections.resize(level + 1);
            isValid = 1;
        }
    };

    std::vector<Node *> allNodes;
    std::vector<std::vector<float>> deleted_nodes; // deleted nodes
    Node *entryPoint = nullptr;

    std::mt19937 rng; // Mersenne Twister 伪随机生成器
    std::uniform_int_distribution<int> dist;
    ThreadPool pool;

    SearchLayers() : rng(7), dist(0, m_L), pool(6) {
        all_counts = 0;
    }

    // 保持原有随机层数生成方式
    int getLevel() {
        return dist(rng);
    }

    // 余弦相似度计算
    float getSimilarity(const std::vector<float> &v1, const std::vector<float> &v2) const {
        assert(v1.size() == v2.size());
        float dot = 0.0f, norm1 = 0.0f, norm2 = 0.0f;
        for (size_t i = 0; i < v1.size(); ++i) {
            dot += v1[i] * v2[i];
            norm1 += v1[i] * v1[i];
            norm2 += v2[i] * v2[i];
        }
        if (norm1 == 0 || norm2 == 0)
            return 0;
        return dot / (std::sqrt(norm1) * std::sqrt(norm2));
    }

    void insertNode(uint64_t k, const std::string &val, const std::vector<float> &var) {
        using namespace std::chrono;

        auto t_start       = high_resolution_clock::now();
        auto t_check_start = t_start;

        // 阶段 1：查找是否已存在
        Node *target = nullptr;
        for (Node *node : allNodes) {
            if (node && node->nodeKey == k) {
                target = node;
                break;
            }
        }

        if (target) {
            if (val == DEL) {
                target->isValid = 0;
                std::cout << "del:" << target->nodeKey << std::endl;
                return;
            } else {
                target->isValid = 0;
                std::cout << "reinsert:" << target->nodeKey << std::endl;
            }
        }
        if (val == DEL)
            return;

        auto t_check_end = high_resolution_clock::now();

        // 阶段 2：创建节点
        auto t_create_start = high_resolution_clock::now();
        int level           = getLevel();
        if (this->allNodes.empty())
            level = m_L;
        Node *newNode   = new Node(k, val, var, level);
        newNode->nodeID = all_counts++;
        allNodes.push_back(newNode);
        auto t_create_end = high_resolution_clock::now();

        if (!entryPoint) {
            entryPoint = newNode;
            std::cout << "[InsertNode] First entry added\n";
            return;
        }

        // 阶段 3：从顶层向下搜索合适入口
        auto t_search_entry_start = high_resolution_clock::now();
        Node *ep                  = entryPoint;
        for (int l = m_L; l > level; --l) {
            std::vector<Node *> result;
            searchLayer(newNode, ep, l, 1, result); // 每层只找 1 个邻居
            if (!result.empty())
                ep = result[0];
        }
        auto t_search_entry_end = high_resolution_clock::now();

        // 阶段 4：并行建立每一层的连接
        auto t_connect_total_start = high_resolution_clock::now();

        // 使用线程池并行执行每一层的连接建立
        std::vector<std::future<void>> futures;
        for (int l = std::min(level, m_L); l >= 0; --l) {
            // 使用你定义的线程池来处理并行任务
            futures.emplace_back(pool.enqueue([this, newNode, l] {
                auto t_layer_start = high_resolution_clock::now();

                // 针对当前层，先搜索出候选节点
                std::vector<Node *> candidates;
                this->searchLayer(newNode, entryPoint, l, efConstruction, candidates);

                // 进行相似度排序
                std::sort(candidates.begin(), candidates.end(), [&](Node *a, Node *b) {
                    return getSimilarity(newNode->data, a->data) > getSimilarity(newNode->data, b->data);
                });

                // 修剪候选节点，保留最多 M 个
                if (candidates.size() > M) {
                    candidates.resize(M);
                }

                // 添加连接到当前层的节点
                for (Node *neighbor : candidates) {
                    if (std::find(newNode->connections[l].begin(), newNode->connections[l].end(), neighbor) ==
                        newNode->connections[l].end())
                        newNode->connections[l].push_back(neighbor);
                    if (std::find(neighbor->connections[l].begin(), neighbor->connections[l].end(), newNode) ==
                        neighbor->connections[l].end())
                        neighbor->connections[l].push_back(newNode);
                }

                // 修剪每个连接
                this->pruneConnections(newNode, l);
                for (Node *neighbor : candidates) {
                    this->pruneConnections(neighbor, l);
                }

                auto t_layer_end = high_resolution_clock::now();
            }));
        }

        // 等待所有层完成
        for (auto &f : futures) {
            f.get();
        }

        auto t_connect_total_end = high_resolution_clock::now();

        // 阶段 4：建立每一层的连接
        /*auto t_connect_total_start = high_resolution_clock::now();
        for (int l = std::min(level, m_L); l >= 0; --l) {
            auto t_layer_start = high_resolution_clock::now();

            std::vector<Node *> candidates;
            searchLayer(newNode, ep, l, efConstruction, candidates);

            if (std::find(candidates.begin(), candidates.end(), ep) == candidates.end()) {
                candidates.push_back(ep);
            }

            std::sort(candidates.begin(), candidates.end(), [&](Node *a, Node *b) {
                return getSimilarity(newNode->data, a->data) > getSimilarity(newNode->data, b->data);
            });

            if (candidates.size() > M) {
                candidates.resize(M);
            }

            for (Node *neighbor : candidates) {
                if (std::find(newNode->connections[l].begin(), newNode->connections[l].end(), neighbor) ==
                    newNode->connections[l].end())
                    newNode->connections[l].push_back(neighbor);
                if (std::find(neighbor->connections[l].begin(), neighbor->connections[l].end(), newNode) ==
                    neighbor->connections[l].end())
                    neighbor->connections[l].push_back(newNode);
            }

            pruneConnections(newNode, l);
            for (Node *neighbor : candidates)
                pruneConnections(neighbor, l);

            if (!candidates.empty())
                ep = candidates[0];

            auto t_layer_end = high_resolution_clock::now();
        }
        auto t_connect_total_end = high_resolution_clock::now();*/

        // 阶段 5：更新入口点
        auto t_entry_update_start = high_resolution_clock::now();
        if (level > entryPoint->level) {
            entryPoint = newNode;
        }
        auto t_entry_update_end = high_resolution_clock::now();

        // 最终统计
        auto t_end = high_resolution_clock::now();
        /*std::cout << "[InsertNode] key = " << k << "\n";
        std::cout << "  Total Insert Time    : " << std::chrono::duration<double, std::milli>(t_end - t_start).count()
                  << " ms\n";*/
    }

    void searchLayer(Node *q, Node *ep, int level, int ef, std::vector<Node *> &results) {
        // 使用小顶堆，存储相似度和节点
        std::priority_queue<
            std::pair<float, Node *>,
            std::vector<std::pair<float, Node *>>,
            std::greater<std::pair<float, Node *>>>
            candidates;

        std::unordered_set<Node *> visited; // 已访问节点集合
        std::queue<Node *> tmp;             // BFS 队列

        // 初始化：将入口节点加入堆和队列
        candidates.push({getSimilarity(q->data, ep->data), ep});
        tmp.push(ep);
        visited.insert(ep); // 将入口节点标记为已访问

        // BFS 搜索，逐步扩展候选节点
        while (!tmp.empty()) {
            Node *curNode = tmp.front(); // 获取队列头部元素
            tmp.pop();

            // 遍历当前节点的邻居
            for (Node *neighbor : curNode->connections[level]) {
                if (visited.find(neighbor) == visited.end()) {
                    tmp.push(neighbor);       // 将未访问过的邻居加入队列
                    visited.insert(neighbor); // 标记邻居为已访问
                }
            }

            // 计算当前节点与查询节点的相似度
            float sim = getSimilarity(q->data, curNode->data);

            // 如果堆的大小小于 ef 或者当前相似度更大，加入新节点
            if (candidates.size() < ef) {
                candidates.push({sim, curNode});
            } else if (sim > candidates.top().first) {
                candidates.pop();                // 移除最小的节点
                candidates.push({sim, curNode}); // 加入新节点
            }
        }

        // 收集最接近的 ef 个节点
        while (!candidates.empty()) {
            results.push_back(candidates.top().second);
            candidates.pop();
        }

        // 结果已经按照相似度从小到大排列，需要反转为从大到小
        std::reverse(results.begin(), results.end());
    }

    // 改进的连接修剪
    void pruneConnections(Node *node, int level) {
        auto &conns = node->connections[level];
        if (conns.size() <= M_max)
            return;

        // 按相似度排序
        std::sort(conns.begin(), conns.end(), [&](Node *a, Node *b) {
            return getSimilarity(node->data, a->data) > getSimilarity(node->data, b->data);
        });

        // 双向修剪
        for (auto it = conns.begin() + M_max; it != conns.end(); ++it) {
            Node *neighbor = *it;

            // 从邻居连接中移除当前节点
            auto &neighborConns = neighbor->connections[level];
            neighborConns.erase(std::remove(neighborConns.begin(), neighborConns.end(), node), neighborConns.end());
        }

        // 修剪当前节点连接
        conns.resize(M_max);
    }

    std::vector<std::pair<uint64_t, std::string>> search_knn(const std::vector<float> &queryData, int k) {
        using namespace std::chrono;

        auto t_all_start = high_resolution_clock::now();

        auto t1 = high_resolution_clock::now(); // 初始化阶段开始
        std::vector<std::pair<uint64_t, std::string>> results;
        if (!entryPoint) {
            return results;
        }
        Node *current = entryPoint;
        std::vector<Node *> visited;
        auto t2 = high_resolution_clock::now(); // 初始化阶段结束

        auto t3 = high_resolution_clock::now(); // 高层到底层阶段开始
        for (int l = m_L; l >= 1; --l) {
            const std::vector<Node *> &neighbors = current->connections[l];
            std::vector<std::pair<float, Node *>> candidates;

            for (Node *neighbor : neighbors) {
                float sim = getSimilarity(queryData, neighbor->data);
                candidates.emplace_back(sim, neighbor);
            }

            std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) {
                return a.first > b.first;
            });

            static std::mt19937 gen(std::random_device{}());
            static std::uniform_real_distribution<float> dist(0.0f, 1.0f);
            constexpr int explore_width = 3;

            float bestSim = getSimilarity(queryData, current->data);
            Node *next    = current;

            for (int i = 0; i < std::min(explore_width, (int)candidates.size()); ++i) {
                float sim = candidates[i].first;
                if (sim > bestSim || (sim == bestSim && dist(gen) < 0.2f)) {
                    bestSim = sim;
                    next    = candidates[i].second;
                }
            }

            if (next != current) {
                current = next;
                visited.push_back(current);
            }
        }
        auto t4 = high_resolution_clock::now(); // 高层到底层阶段结束

        auto t5 = high_resolution_clock::now(); // 最底层阶段开始
        std::priority_queue<
            std::pair<float, Node *>,
            std::vector<std::pair<float, Node *>>,
            std::greater<std::pair<float, Node *>>>
            candidates;

        std::unordered_set<Node *> visitedSet;
        std::queue<Node *> tmp;

        candidates.push({getSimilarity(queryData, current->data), current});
        tmp.push(current);
        visitedSet.insert(current);

        while (!tmp.empty() && candidates.size() < efConstruction) {
            Node *curNode = tmp.front();
            tmp.pop();

            for (Node *neighbor : curNode->connections[0]) {
                if (visitedSet.find(neighbor) == visitedSet.end()) {
                    tmp.push(neighbor);
                    visitedSet.insert(neighbor);

                    float sim = getSimilarity(queryData, neighbor->data);
                    candidates.push({sim, neighbor});
                }
            }
        }
        auto t6 = high_resolution_clock::now(); // 最底层阶段结束

        auto t7 = high_resolution_clock::now(); // 得到结果阶段开始
        std::vector<Node *> tempResults;
        while (!candidates.empty()) {
            tempResults.push_back(candidates.top().second);
            candidates.pop();
        }
        std::reverse(tempResults.begin(), tempResults.end());

        int m = 0;
        for (int i = 0; i < (int)tempResults.size() && m < k;) {
            if (tempResults[i]->isValid) {
                results.push_back({tempResults[i]->nodeKey, tempResults[i]->value});
                m++;
            }
            ++i;
        }
        auto t8 = high_resolution_clock::now(); // 得到结果阶段结束

        auto t_all_end = high_resolution_clock::now();

        /*std::cout << "search_knn 计时报告:\n";
        std::cout << "  初始化阶段       : " << duration<double, std::milli>(t2 - t1).count() << " ms\n";
        std::cout << "  高层到底层阶段   : " << duration<double, std::milli>(t4 - t3).count() << " ms\n";
        std::cout << "  最底层扩展阶段   : " << duration<double, std::milli>(t6 - t5).count() << " ms\n";
        std::cout << "  结果处理阶段     : " << duration<double, std::milli>(t8 - t7).count() << " ms\n";
        std::cout << "  总耗时           : " << duration<double, std::milli>(t_all_end - t_all_start).count()
                  << " ms\n";*/

        return results;
    }

    // 清理所有节点
    void clear() {
        for (auto node : allNodes)
            delete node;
        allNodes.clear();
        deleted_nodes.clear();
        entryPoint = nullptr;
    }

    void printInfo() const {
        std::cout << "===== SearchLayers Debug Info =====\n";
        for (const auto &node : allNodes) {
            std::cout << "NodeID: " << node->nodeID << " NodeKey: " << node->nodeKey
                      << ", isValid: " << (node->isValid ? "true" : "false") << ", level: " << node->level << "\n";

            for (int l = 0; l <= node->level; ++l) {
                std::cout << "  Level " << l << ": ";
                if (node->connections[l].empty()) {
                    std::cout << "(none)";
                } else {
                    for (const auto &neighbor : node->connections[l]) {
                        std::cout << neighbor->nodeID << " ";
                    }
                }
                std::cout << "\n";
            }
        }

        if (entryPoint)
            std::cout << "Entry: " << entryPoint->nodeID << ", State: " << entryPoint->isValid << "\n";
        else
            std::cout << "Entry: (null)\n";

        std::cout << "===== End of Info =====\n";
    }
};

class KVStore : public KVStoreAPI {
    // You can add your implementation here
private:
    skiplist *s = new skiplist(0.5); // memtable
    // std::vector<sstablehead> sstableIndex;  // sstable的表头缓存

    std::vector<sstablehead> sstableIndex[15]; // the sshead for each level

    int totalLevel = -1; // 层数
    std::vector<std::string> tmp_vec;
    std::vector<uint64_t> tmp_key;

public:
    // buffer-tmp-map
    std::unordered_map<uint64_t, std::vector<float>> bufferMap;
    // a map to manage the vector
    // key-vector
    std::unordered_map<uint64_t, std::vector<float>> vectorMap;

    SearchLayers searchRoute;

    KVStore(const std::string &dir);

    ~KVStore();

    void put(uint64_t key, const std::string &s) override;

    std::string get(uint64_t key) override;

    bool del(uint64_t key) override;

    void reset() override;

    void scan(uint64_t key1, uint64_t key2, std::list<std::pair<uint64_t, std::string>> &list) override;

    void compaction();
    bool compaction(int level);

    void delsstable(std::string filename);  // 从缓存中删除filename.sst， 并物理删除
    void addsstable(sstable ss, int level); // 将ss加入缓存

    std::string fetchString(std::string file, int startOffset, uint32_t len);

    static std::string getFile_newName(sstable &ss);

    void sortTable(int curLevel);

    std::vector<std::pair<std::uint64_t, std::string>> search_knn(std::string query, int k); // use as similarity

    float getSimilarity(const std::vector<float> &v1, const std::vector<float> &v2) const;

    std::vector<std::pair<std::uint64_t, std::string>> search_knn_hnsw(std::string query, int k);

    void merge_vector(); // merge the tmp and transfer into vectors.

    void collectIntoFiles(const std::string &data_root);
    // store all pairs in vectorMap.

    void load_embedding_from_disk(const std::string &data_root);
    // load into vectorMap

    void save_hnsw_index_to_disk(const std::string &hnsw_data_root);

    void load_hnsw_index_from_disk(const std::string &hnsw_data_root);

    void removeDirectoryRecursive(const std::string &path);

    void read_build_from_text(const std::string &string_path, const std::string &vector_path, uint64_t max);
    // read into buffer and then merge into the map.
};

struct DataBlock {
public:
    uint64_t key;
    std::vector<float> embedding; // size is equal to the dimension(768 in this model)
};

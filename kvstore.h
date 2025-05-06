#pragma once

#include "kvstore_api.h"
#include "skiplist.h"
#include "sstable.h"
#include "sstablehead.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <ctime>
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
    const int M              = 6;  // 每个节点的最小连接数
    const int M_max          = 8;  // 每个节点的最大连接数
    const int m_L            = 6;  // 最大层数
    const int efConstruction = 50; // 构建时的候选集合大小
    const std::string DEL = "~DELETED~";
    

    struct Node {
        uint64_t nodeKey;
        std::string value;
        std::vector<float> data;
        std::vector<std::vector<Node *>> connections; // 分层连接
        std::vector<Node *> nearPoints;
        int level;

        Node(uint64_t k, const std::string &val, const std::vector<float> &var, int level) :
            nodeKey(k),
            value(val),
            data(var),
            level(level) {
            connections.resize(level + 1);
        }
    };

    std::vector<Node *> allNodes;
    std::vector<std::vector<float>> deleted_nodes; // deleted nodes
    Node *entryPoint = nullptr;

    SearchLayers() {
        srand(time(nullptr));
    } // 初始化随机种子

    bool isDeleted(const std::vector<float> &v) {
        for (const auto& del_vec : deleted_nodes) {
            if (v.size() != del_vec.size()) 
                continue;
            bool equal = true;
            for (size_t i = 0; i < v.size(); ++i) {
                if (v[i] != del_vec[i]) {
                    equal = false;
                    break;
                }
            }
            if (equal) return true;
        }
        return false;
    }

    // 保持原有随机层数生成方式
    int getLevel() {
        return rand() % (m_L + 1); // 0到m_L之间的随机数
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

    // 插入逻辑
    void insertNode(uint64_t k, const std::string &val, const std::vector<float> &var) {
        //first need to search whether the key exists.
        Node *target = nullptr;
        for (Node* node : allNodes) {
            if (node && node->nodeKey == k) {
                target = node;
                break;
            }
        }
        
        if(target){
            if(val == DEL){
                this->deleted_nodes.push_back(target->data);
                return;
            }
            else{
                this->deleted_nodes.push_back(target->data);
            }
        }

        // if not and the mark is not DEL, then insert.
        int level = getLevel();
        if (this->allNodes.empty()) {
            level = m_L;
        }

        Node *newNode = new Node(k, val, var, level);
        allNodes.push_back(newNode);

        if (!entryPoint) {
            entryPoint = newNode;
            return;
        }

        // 阶段1：从顶层向下搜索到 level+1 层
        Node *ep = entryPoint;
        for (int l = m_L; l > level; --l) {
            std::vector<Node *> result;
            searchLayer(newNode, ep, l, 1, result); // 每层只找1个最近邻
            if (!result.empty()) {
                ep = result[0];
            }
        }

        // 阶段2：从当前层向下建立连接
        for (int l = std::min(level, m_L); l >= 0; --l) {
            std::vector<Node *> candidates;

            // 使用动态更新的入口点进行搜索
            searchLayer(newNode, ep, l, efConstruction, candidates);
            // std::cout << "size:"<<candidates.size()<<std::endl;

            // 合并当前层候选集（添加ep自身）
            if (std::find(candidates.begin(), candidates.end(), ep) == candidates.end()) {
                candidates.push_back(ep);
            }

            // 按相似度排序
            std::sort(candidates.begin(), candidates.end(), [&](Node *a, Node *b) {
                return getSimilarity(newNode->data, a->data) > getSimilarity(newNode->data, b->data);
            });

            // 保留前M个
            if (candidates.size() > M) {
                candidates.resize(M);
            }

            // 建立双向连接
            for (Node *neighbor : candidates) {
                // 添加新节点到邻居的连接
                if (std::find(newNode->connections[l].begin(), newNode->connections[l].end(), neighbor) ==
                    newNode->connections[l].end()) {

                    newNode->connections[l].push_back(neighbor);
                }

                // 添加邻居到新节点的连接
                if (std::find(neighbor->connections[l].begin(), neighbor->connections[l].end(), newNode) ==
                    neighbor->connections[l].end()) {
                    neighbor->connections[l].push_back(newNode);
                }
            }

            // 修剪所有相关节点的连接
            pruneConnections(newNode, l);
            for (Node *neighbor : candidates) {
                pruneConnections(neighbor, l);
            }

            // 更新下一层的入口点
            if (!candidates.empty())
                ep = candidates[0];
        }

        // 更新全局入口点（如果新节点在更高层）
        if (level > entryPoint->level) {
            entryPoint = newNode;
        }
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
            if(!isDeleted(candidates.top().second->data)){
                results.push_back(candidates.top().second);
            }
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
        auto start = std::chrono::high_resolution_clock::now();

        std::vector<std::pair<uint64_t, std::string>> results;
        if (!entryPoint)
            return results; // 如果入口点为空，直接返回空结果

        Node *current = entryPoint;
        std::vector<Node *> visited; // 记录访问的节点

        // 从第 m_L 层开始逐层搜索，直到最底层（层级为0）
        for (int l = m_L; l >= 1; --l) {
            Node *next    = nullptr;
            float bestSim = -1;

            // 找到当前层与查询节点最接近的一个节点
            for (Node *neighbor : current->connections[l]) {
                float sim = getSimilarity(queryData, neighbor->data);
                if (sim > bestSim) {
                    bestSim = sim;
                    next    = neighbor;
                }
            }

            // 更新当前节点为下一层的入口点
            if (next) {
                current = next;
                visited.push_back(current);
            }
        }

        // 使用小顶堆存储相似度和节点，保证堆中的元素是按相似度升序排列的
        std::priority_queue<
            std::pair<float, Node *>,
            std::vector<std::pair<float, Node *>>,
            std::greater<std::pair<float, Node *>>>
            candidates;

        std::unordered_set<Node *> visitedSet; // 已访问节点集合
        std::queue<Node *> tmp;                // BFS 队列

        // 初始化：将入口节点加入堆和队列
        candidates.push({getSimilarity(queryData, current->data), current});
        tmp.push(current);
        visitedSet.insert(current); // 将入口节点标记为已访问

        // BFS 搜索，逐步扩展候选节点
        while (!tmp.empty() && candidates.size() < efConstruction) {
            Node *curNode = tmp.front(); // 获取队列头部元素
            tmp.pop();

            // 遍历当前节点的邻居（这里假设 level=0，即最底层的邻居）
            for (Node *neighbor : curNode->connections[0]) {
                if (visitedSet.find(neighbor) == visitedSet.end()) {
                    tmp.push(neighbor);          // 将未访问过的邻居加入队列
                    visitedSet.insert(neighbor); // 标记邻居为已访问

                    // 计算当前节点与查询节点的相似度
                    float sim = getSimilarity(queryData, neighbor->data);
                    candidates.push({sim, neighbor});
                }
            }
        }

        // 收集最接近的 ef 个节点
        std::vector<Node *> tempResults;
        while (!candidates.empty()) {
            tempResults.push_back(candidates.top().second);
            candidates.pop();
        }

        // 结果已经按相似度从小到大排列，反转为从大到小
        std::reverse(tempResults.begin(), tempResults.end());

        // 选取前 k 个最接近的节点
        for (int i = 0; i < std::min(k, (int)tempResults.size()); ++i) {
            results.push_back({tempResults[i]->nodeKey, tempResults[i]->value});
        }
        auto end = std::chrono::high_resolution_clock::now();
        // 计算时间差并转换为微秒
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        // std::cout << "Function execution time: " << duration.count() << " microseconds" << std::endl;
        // std::cout << duration.count() << std::endl;
        return results;
    }

    // 清理所有节点
    void clear() {
        for (auto node : allNodes)
            delete node;
        allNodes.clear();
        entryPoint = nullptr;
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
    //store all pairs in vectorMap.

    void load_embedding_from_disk(const std::string &data_root);
    //load into vectorMap
};

struct DataBlock{
public:
    uint64_t key;
    std::vector<float> embedding; // size is equal to the dimension(768 in this model) 
};
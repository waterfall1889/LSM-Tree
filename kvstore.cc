#include "kvstore.h"

#include "embedding.h"
#include "skiplist.h"
#include "sstable.h"
#include "utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <queue>
#include <set>
#include <string>
#include <utility>
using namespace std::chrono;

static const std::string DEL = "~DELETED~";
const uint32_t MAXSIZE       = 2 * 1024 * 1024;

struct poi {
    int sstableId; // vector中第几个sstable
    int pos;       // 该sstable的第几个key-offset
    uint64_t time;
    Index index;
};

struct cmpPoi {
    bool operator()(const poi &a, const poi &b) {
        if (a.index.key == b.index.key)
            return a.time < b.time;
        return a.index.key > b.index.key;
    }
};

KVStore::KVStore(const std::string &dir) :
    KVStoreAPI(dir) // read from sstables
{
    for (totalLevel = 0;; ++totalLevel) {
        std::string path = dir + "/level-" + std::to_string(totalLevel) + "/";
        std::vector<std::string> files;
        if (!utils::dirExists(path)) {
            totalLevel--;
            break; // stop read
        }
        int nums = utils::scanDir(path, files);
        sstablehead cur;
        for (int i = 0; i < nums; ++i) {       // 读每一个文件头
            std::string url = path + files[i]; // url, 每一个文件名
            cur.loadFileHead(url.data());
            sstableIndex[totalLevel].push_back(cur);
            TIME = std::max(TIME, cur.getTime()); // 更新时间戳
        }
    }
}

KVStore::~KVStore() {
    sstable ss(s);
    if (!ss.getCnt())
        return; // empty sstable
    std::string path = std::string("./data/level-0/");
    if (!utils::dirExists(path)) {
        utils::_mkdir(path.data());
        totalLevel = 0;
    }
    ss.putFile(ss.getFilename().data());
    compaction();   // 从0层开始尝试合并
    /*merge_vector(); // merge all

    collectIntoFiles("embedding_data");
    save_hnsw_index_to_disk("hnsw_data");*/
}

/**
 * Insert/Update the key-value pair.
 * No return values for simplicity.
 */
void KVStore::put(uint64_t key, const std::string &val) {
    uint32_t nxtsize = s->getBytes();
    std::string res  = s->search(key);
    if (!res.length()) { // new add
        nxtsize += 12 + val.length();
    } else
        nxtsize = nxtsize - res.length() + val.length(); // change string

    if (nxtsize + 10240 + 32 <= MAXSIZE) {
        s->insert(key, val); // 小于等于（不超过） 2MB
        tmp_vec.push_back(val);
        tmp_key.push_back(key);
    } else {
        tmp_vec.push_back(val);
        tmp_key.push_back(key);

        //merge_vector();

        sstable ss(s);
        s->reset();
        std::string url  = ss.getFilename();
        std::string path = "./data/level-0";
        if (!utils::dirExists(path)) {
            utils::mkdir(path.data());
            totalLevel = 0;
        }
        addsstable(ss, 0);      // 加入缓存
        ss.putFile(url.data()); // 加入磁盘
        compaction();
        s->insert(key, val);
    }
}

/**
 * Returns the (string) value of the given key.
 * An empty string indicates not found.
 */
std::string KVStore::get(uint64_t key) //
{
    uint64_t time = 0;
    int goalOffset;
    uint32_t goalLen;
    std::string goalUrl;
    std::string res = s->search(key);
    if (res.length()) { // 在memtable中找到, 或者是deleted，说明最近被删除过，
                        // 不用查sstable
        if (res == DEL)
            return "";
        return res;
    }
    for (int level = 0; level <= totalLevel; ++level) {
        for (sstablehead it : sstableIndex[level]) {
            if (key < it.getMinV() || key > it.getMaxV())
                continue;
            uint32_t len;
            int offset = it.searchOffset(key, len);
            if (offset == -1) {
                if (!level)
                    continue;
                else
                    break;
            }
            // sstable ss;
            // ss.loadFile(it.getFilename().data());
            if (it.getTime() > time) { // find the latest head
                time       = it.getTime();
                goalUrl    = it.getFilename();
                goalOffset = offset + 32 + 10240 + 12 * it.getCnt();
                goalLen    = len;
            }
        }
        if (time)
            break; // only a test for found
    }
    if (!goalUrl.length())
        return ""; // not found a sstable
    res = fetchString(goalUrl, goalOffset, goalLen);
    if (res == DEL)
        return "";
    return res;
}

/**
 * Delete the given key-value pair if it exists.
 * Returns false iff the key is not found.
 */
bool KVStore::del(uint64_t key) {
    std::string res = get(key);
    if (!res.length())
        return false; // not exist
    put(key, DEL);    // put a del marker
    return true;
}

void KVStore::removeDirectoryRecursive(const std::string &path) {
    std::vector<std::string> entries;
    if (!utils::scanDir(path, entries)) {
        utils::rmdir(path.c_str()); // 删除空目录本身
        return;
    }

    for (const auto &entry : entries) {
        std::string fullPath = path + "/" + entry;
        if (utils::dirExists(fullPath)) {
            removeDirectoryRecursive(fullPath); // 递归删除子目录
        } else {
            utils::rmfile(fullPath.c_str()); // 删除文件
        }
    }
    utils::rmdir(path.c_str()); // 删除空目录本身
}

void KVStore::reset() {
    s->reset(); // 先清空 memtable
    bufferMap.clear();
    vectorMap.clear();
    tmp_vec.clear();
    tmp_key.clear();

    // 清空搜索路径
    this->searchRoute.clear();

    // 清空每一层的 SSTable 文件
    std::vector<std::string> files;

    for (int level = 0; level <= totalLevel; ++level) {
        std::string path = std::string("./data/level-") + std::to_string(level);
        int size         = utils::scanDir(path, files);
        for (int i = 0; i < size; ++i) {
            std::string file = path + "/" + files[i];
            utils::rmfile(file.c_str());
        }
        utils::rmdir(path.c_str());
        sstableIndex[level].clear();
    }

    // 删除 vectors 目录及其中的所有文件
    std::string vecDir = "./embedding_data/vectors";
    if (utils::dirExists(vecDir)) {
        files.clear();
        int size = utils::scanDir(vecDir, files);
        for (int i = 0; i < size; ++i) {
            std::string file = vecDir + "/" + files[i];
            utils::rmfile(file.c_str());
        }
        utils::rmdir(vecDir.c_str());
    }

    // delete hnsws
    std::string hnsw_dir = "./hnsw_data";
    if (utils::dirExists(hnsw_dir)) {
        removeDirectoryRecursive(hnsw_dir);
    }

    totalLevel = -1;
}

/**
 * Return a list including all the key-value pair between key1 and key2.
 * keys in the list should be in an ascending order.
 * An empty string indicates not found.
 */

struct myPair {
    uint64_t key, time;
    int id, index;
    std::string filename;

    myPair(uint64_t key, uint64_t time, int index, int id,
           std::string file) { // construct function
        this->time     = time;
        this->key      = key;
        this->id       = id;
        this->index    = index;
        this->filename = file;
    }
};

struct cmp {
    bool operator()(myPair &a, myPair &b) {
        if (a.key == b.key)
            return a.time < b.time;
        return a.key > b.key;
    }
};

void KVStore::scan(uint64_t key1, uint64_t key2, std::list<std::pair<uint64_t, std::string>> &list) {
    std::vector<std::pair<uint64_t, std::string>> mem;
    // std::set<myPair> heap; // 维护一个指针最小堆
    std::priority_queue<myPair, std::vector<myPair>, cmp> heap;
    // std::vector<sstable> ssts;
    std::vector<sstablehead> sshs;
    s->scan(key1, key2, mem);   // add in mem
    std::vector<int> head, end; // [head, end)
    int cnt = 0;
    if (mem.size())
        heap.push(myPair(mem[0].first, INF, 0, -1, "qwq"));
    for (int level = 0; level <= totalLevel; ++level) {
        for (sstablehead it : sstableIndex[level]) {
            if (key1 > it.getMaxV() || key2 < it.getMinV())
                continue; // 无交集
            int hIndex = it.lowerBound(key1);
            int tIndex = it.lowerBound(key2);
            if (hIndex < it.getCnt()) { // 此sstable可用
                std::string url = it.getFilename();
                heap.push(myPair(it.getKey(hIndex), it.getTime(), hIndex, cnt++, url));
                head.push_back(hIndex);
                if (it.search(key2) == tIndex)
                    tIndex++; // tIndex为第一个不可的
                end.push_back(tIndex);
                // ssts.push_back(ss); // 加入ss
                sshs.push_back(it);
            }
        }
    }
    uint64_t lastKey = INF; // only choose the latest key
    while (!heap.empty()) { // 维护堆
        myPair cur = heap.top();
        heap.pop();
        if (cur.id >= 0) { // from sst
            if (cur.key != lastKey) {
                lastKey         = cur.key;
                uint32_t start  = sshs[cur.id].getOffset(cur.index - 1);
                uint32_t len    = sshs[cur.id].getOffset(cur.index) - start;
                uint32_t scnt   = sshs[cur.id].getCnt();
                std::string res = fetchString(cur.filename, 10240 + 32 + scnt * 12 + start, len);
                if (res.length() && res != DEL)
                    list.emplace_back(cur.key, res);
            }
            if (cur.index + 1 < end[cur.id]) { // add next one to heap
                heap.push(myPair(sshs[cur.id].getKey(cur.index + 1), cur.time, cur.index + 1, cur.id, cur.filename));
            }
        } else { // from mem
            if (cur.key != lastKey) {
                lastKey         = cur.key;
                std::string res = mem[cur.index].second;
                if (res.length() && res != DEL)
                    list.emplace_back(cur.key, mem[cur.index].second);
            }
            if (cur.index < mem.size() - 1) {
                heap.push(myPair(mem[cur.index + 1].first, cur.time, cur.index + 1, -1, cur.filename));
            }
        }
    }
}

void KVStore::compaction() {
    int curLevel = 0;
    if (sstableIndex[0].size() <= 2) {
        return;
    }
    // TODO here
    while (curLevel <= this->totalLevel) {
        if (!compaction(curLevel)) {
            std::cerr << "failed:error occurred in compaction of level-" << curLevel << std::endl;
            return;
        }
        // 处理下一层
        curLevel++;
    }
}

bool KVStore::compaction(int curLevel) {
    // 合并成功返回真；否则返回假；
    // 最大限制
    int maxLevelSize = (1 << (curLevel + 1));
    // 超出的数量
    int excess = sstableIndex[curLevel].size() - maxLevelSize;
    if (excess <= 0) {
        return true; // 不需要合并
    }

    // 当前层需要合并
    std::vector<sstablehead> currentLevelSSTs;
    currentLevelSSTs.clear();
    if (curLevel == 0) {
        // 选择所有
        for (const auto &item : sstableIndex[0]) {
            currentLevelSSTs.push_back(item);
        }
        sstableIndex[curLevel].clear(); // 缓存清除
    }

    else if (curLevel > 0) {
        // 其他层选择时间戳最小的超出部分
        // 定义lamda函数
        sortTable(curLevel);
        // 直接插入选中的SSTable
        currentLevelSSTs.insert(
            currentLevelSSTs.end(), sstableIndex[curLevel].begin(), sstableIndex[curLevel].begin() + excess
        );

        // 删除选中的部分
        sstableIndex[curLevel].erase(sstableIndex[curLevel].begin(), sstableIndex[curLevel].begin() + excess);
    }

    // 下一层
    std::vector<sstablehead> nextLevelOverlap;
    nextLevelOverlap.clear();
    if (curLevel + 1 <= totalLevel) {
        // 下一层不存在时不进行统计
        uint64_t minKey = UINT64_MAX;
        uint64_t maxKey = 0;
        for (const auto &sst : currentLevelSSTs) {
            if (minKey > sst.getMinV()) {
                minKey = sst.getMinV();
            }
            if (maxKey < sst.getMaxV()) {
                maxKey = sst.getMaxV();
            }
            // 得到当前的值域
        }
        for (const auto &sst : sstableIndex[curLevel + 1]) {
            if (sst.getMaxV() >= minKey && sst.getMinV() <= maxKey) {
                nextLevelOverlap.push_back(sst);
            } else {
                continue;
            }
        }
    }

    // 所有相关的SSTable
    std::vector<sstablehead> allSSTables;
    allSSTables.clear();
    for (const auto &sstable : currentLevelSSTs) {
        allSSTables.push_back(sstable);
    }
    for (const auto &sstable : nextLevelOverlap) {
        allSSTables.push_back(sstable);
    }

    // 归并排序
    size_t position = 0;
    std::priority_queue<poi, std::vector<poi>, cmpPoi> mergeQueue;

    for (auto &sstable : allSSTables) {
        if (sstable.getCnt() == 0) {
            continue; // 跳过空或无效的SSTable
        }
        if (sstable.getKey(0) == UINT64_MAX) {
            continue;
        }
        Index tmpIndex{sstable.getKey(0), 0};
        poi tmp{static_cast<int>(position), 0, sstable.getTime(), tmpIndex};
        mergeQueue.push(tmp);
        position++;
    }

    std::vector<std::pair<uint64_t, std::string>> mergedData; // 存储数据
    std::string lastValue;
    uint64_t lastTime = 0;
    uint64_t lastKey  = UINT64_MAX;

    // 合并数据
    while (!mergeQueue.empty()) {
        auto top = mergeQueue.top();
        mergeQueue.pop();
        if (top.sstableId > allSSTables.size()) {
            return false;
        }
        auto &sstable = allSSTables[top.sstableId];

        // 如果SSTable为空或位置超出范围，跳过
        if (sstable.getCnt() == 0) {
            continue;
        }
        if (top.pos >= sstable.getCnt()) {
            continue;
        }

        uint64_t key   = sstable.getKey(top.pos);
        uint64_t time  = sstable.getTime();
        uint32_t start = 0;
        if (top.pos != 0) {
            start = sstable.getOffset(top.pos - 1);
        }
        uint32_t len = sstable.getOffset(top.pos) - start;

        // 获取值
        int StartOffSet = 10240 + 32 + sstable.getCnt() * 12 + start;
        std::string value;
        value = fetchString(sstable.getFilename(), StartOffSet, len);

        // 跳过删除标记（对于最后一层）
        if (value == "~DELETED~" && curLevel == totalLevel) {
            continue;
        }

        // 如果是新的键或时间戳更新且时间戳更大，则更新记录
        if (key != lastKey || time > lastTime) {
            if (lastKey != UINT64_MAX) {
                std::pair p{lastKey, lastValue};
                mergedData.emplace_back(p);
            }
            lastKey   = key;
            lastValue = value;
            lastTime  = time;
        } else if (key == lastKey && time > lastTime) {
            // 对于相同key的记录，选择时间戳最大的记录
            lastKey   = key;
            lastValue = value;
            lastTime  = time;
        }

        // 处理下一条记录
        if (top.pos < sstable.getCnt() - 1) {
            uint64_t nextKey  = sstable.getKey(top.pos + 1);
            uint64_t nextTime = sstable.getTime();
            Index nextIndex{nextKey, 0};
            poi nextIssue{top.sstableId, top.pos + 1, nextTime, nextIndex};
            mergeQueue.push(nextIssue);
        }
    }

    if (lastKey != UINT64_MAX) {
        std::pair newPair{lastKey, lastValue};
        mergedData.push_back(newPair);
    }

    // 下一层
    std::vector<sstable> newSSTables;
    skiplist tempList(0.5);
    newSSTables.clear();
    tempList.reset();
    uint32_t currentBytes = 0;
    if (mergedData.empty()) {
        return true; // 合并后为空
    }
    for (const auto &entry : mergedData) {
        // 插入
        size_t maxSize = MAXSIZE - 32 - 10240;
        if (maxSize < currentBytes + 12 + entry.second.size()) {
            sstable newSST(&tempList);
            newSSTables.push_back(newSST);
            tempList.reset();
            currentBytes = 0;
        }
        tempList.insert(entry.first, entry.second);
        currentBytes = 12 + entry.second.size() + currentBytes;
    }
    // 剩余的数据从跳表转移到新文件
    if (tempList.getBytes() > 0) {
        sstable newSST(&tempList);
        newSSTables.push_back(newSST);
    }
    // 检测是否成功转移
    if (newSSTables.empty()) {
        std::cout << "Warning: No valid SSTable generated after merging data." << std::endl;
        return true;
    }

    for (auto &sst : currentLevelSSTs) {
        try {
            // 尝试删除文件
            delsstable(sst.getFilename());
        } catch (const std::exception &e) {
            std::cerr << "Error deleting file " << sst.getFilename() << ": " << e.what() << std::endl;
            return false; // 删除失败，返回失败
        }
    }
    for (auto &sst : nextLevelOverlap) {
        try {
            // 尝试删除文件
            delsstable(sst.getFilename());
        } catch (const std::exception &e) {
            std::cerr << "Error deleting file " << sst.getFilename() << ": " << e.what() << std::endl;
            return false; // 删除失败，返回失败
        }
    }

    // 添加到下一层
    int nextLevel         = curLevel + 1;
    std::string targetDir = "./data/level-" + std::to_string(nextLevel) + "/";
    // 目录如果不存在就新建
    if (nextLevel > totalLevel) {
        utils::mkdir((std::string("./data/level-") + std::to_string(nextLevel)).c_str());
        totalLevel++;
    }

    for (auto &ss : newSSTables) {
        std::string filename = getFile_newName(ss);
        std::string url      = "./data/level-" + std::to_string(nextLevel) + "/" + filename;
        ss.setFilename(url);
        ss.putFile(url.data());
        addsstable(ss, nextLevel);
    }

    return true;
}

std::string KVStore::getFile_newName(sstable &ss) {
    // 获取时间戳
    uint64_t timestamp = ss.getTime();
    // 基于时间戳生成文件名
    std::string filename = std::to_string(timestamp) + ".sst"; // 使用时间戳作为文件名
    return filename;
}

void KVStore::sortTable(int curLevel) {
    auto compareFunction = [](const sstablehead &a, const sstablehead &b) {
        if (a.getTime() == b.getTime()) {
            return a.getMinV() < b.getMinV();
        }
        return a.getTime() < b.getTime();
    };
    std::sort(sstableIndex[curLevel].begin(), sstableIndex[curLevel].end(), compareFunction);
}

void KVStore::delsstable(std::string filename) {
    for (int level = 0; level <= totalLevel; ++level) {
        int size = sstableIndex[level].size(), flag = 0;
        for (int i = 0; i < size; ++i) {
            if (sstableIndex[level][i].getFilename() == filename) {
                sstableIndex[level].erase(sstableIndex[level].begin() + i);
                flag = 1;
                break;
            }
        }
        if (flag)
            break;
    }
    int flag = utils::rmfile(filename.data());
    if (flag != 0) {
        std::cout << "delete fail!" << std::endl;
        std::cout << strerror(errno) << std::endl;
    }
}

void KVStore::addsstable(sstable ss, int level) {
    sstableIndex[level].push_back(ss.getHead());
}

char strBuf[2097152];

/**
 * @brief Fetches a substring from a file starting at a given offset.
 *
 * This function opens a file in binary read mode, seeks to the specified start offset,
 * reads a specified number of bytes into a buffer, and returns the buffer as a string.
 *
 * @param file The path to the file from which to read the substring.
 * @param startOffset The offset in the file from which to start reading.
 * @param len The number of bytes to read from the file.
 * @return A string containing the read bytes.
 */
std::string KVStore::fetchString(std::string file, int startOffset, uint32_t len) {
    // TODO here
    // open file in binary mode
    std::ifstream headFile(file, std::ios::binary);
    // test if the file is opened correctly
    if (!headFile) {
        std::cerr << "Error: Unable to open file " << file << std::endl;
        return "";
    }
    // find the offset
    headFile.seekg(startOffset, std::ios::beg);
    // test if the offset is valid
    if (!headFile) {
        std::cerr << "Error: Unable to seek to the offset " << startOffset << std::endl;
        return "";
    }
    // read the string
    headFile.read(strBuf, len);
    // test if it is successful to read
    if (!headFile) {
        std::cerr << "Error: Unable to read " << len << " bytes from file" << std::endl;
        return "";
    }
    // transfer to string
    std::string result(strBuf, len);
    // close file
    headFile.close();
    // return
    return result;
}

std::vector<std::pair<std::uint64_t, std::string>> KVStore::search_knn(std::string query, int k) {
    using namespace std::chrono;

    auto t_all_start = high_resolution_clock::now();

    auto t1 = high_resolution_clock::now();
    merge_vector();
    auto t2 = high_resolution_clock::now();

    // 阶段二：计算嵌入
    auto t3 = high_resolution_clock::now();
    auto query_vector = embedding_single(query);
    auto t4 = high_resolution_clock::now();

    // 阶段三：计算相似度
    auto t5 = high_resolution_clock::now();
    std::vector<std::pair<float, std::uint64_t>> similarities;
    for (const auto &entry : vectorMap) {
        float similarity = getSimilarity(query_vector, entry.second);
        similarities.push_back({similarity, entry.first});
    }
    auto t6 = high_resolution_clock::now();

    // 阶段四：排序
    auto t7 = high_resolution_clock::now();
    std::sort(similarities.begin(), similarities.end(), std::greater<>());
    auto t8 = high_resolution_clock::now();

    // 阶段五：获取结果值
    auto t9 = high_resolution_clock::now();
    std::vector<std::pair<std::uint64_t, std::string>> result;
    for (int i = 0; i < k && i < similarities.size(); ++i) {
        std::string value = get(similarities[i].second);
        result.push_back({similarities[i].second, value});
    }
    auto t10 = high_resolution_clock::now();

    auto t_all_end = high_resolution_clock::now();

    /*std::cout << "KVStore::search_knn 计时报告:\n";
    std::cout << "  向量归并阶段     : " << duration<double, std::milli>(t2 - t1).count() << " ms\n";
    std::cout << "  嵌入生成阶段     : " << duration<double, std::milli>(t4 - t3).count() << " ms\n";
    std::cout << "  相似度计算阶段   : " << duration<double, std::milli>(t6 - t5).count() << " ms\n";
    std::cout << "  排序阶段         : " << duration<double, std::milli>(t8 - t7).count() << " ms\n";
    std::cout << "  结果构建阶段     : " << duration<double, std::milli>(t10 - t9).count() << " ms\n";
    std::cout << "  总耗时           : " << duration<double, std::milli>(t_all_end - t_all_start).count() << " ms\n";*/

    return result;
}


float KVStore::getSimilarity(const std::vector<float> &v1, const std::vector<float> &v2) const {
    auto it1 = v1.begin();
    auto it2 = v2.begin();
    float n1 = 0.0f;
    float n2 = 0.0f;
    float s  = 0.0f;
    while (it1 != v1.end()) {
        s += (*it1) * (*it2);
        n1 += (*it1) * (*it1);
        n2 += (*it2) * (*it2);
        it1++;
        it2++;
    }
    if (n1 == 0 && n2 == 0) {
        return 0;
    }
    return s / (std::sqrt(n1) * std::sqrt(n2));
}

std::vector<std::pair<std::uint64_t, std::string>> KVStore::search_knn_hnsw(std::string query, int k) {
    merge_vector();
    auto query_vector = embedding_single(query); // 获取查询向量
    return this->searchRoute.search_knn(query_vector, k);
}

void KVStore::merge_vector() {
    if (tmp_key.empty() && bufferMap.empty()) {
        return;
    }

    std::string joined_prompts = join(tmp_vec, "\n");
    auto embeddings            = embedding_batch(joined_prompts);

    // 将嵌入向量放入 bufferMap
    auto key_read    = tmp_key.begin();
    auto string_read = tmp_vec.begin();
    auto vector_read = embeddings.begin();
    while (key_read != tmp_key.end()) {
        bufferMap[*key_read] = *vector_read;
        searchRoute.insertNode((*key_read), (*string_read), (*vector_read));
        ++key_read;
        ++string_read;
        ++vector_read;
    }

    // 清空 tmp 缓存
    tmp_vec.clear();
    tmp_key.clear();

    // 将 bufferMap 中所有数据转移至 vectorMap（替换或插入）
    for (const auto &[key, vec] : bufferMap) {
        vectorMap[key] = vec; // 替换已有向量
    }

    bufferMap.clear(); // 清空 buffer
}

void KVStore::collectIntoFiles(const std::string &data_root) {
    if (this->vectorMap.empty()) {
        return;
    }

    std::string vectorsDir = data_root + "/vectors";

    // 判断文件是否已存在（通过检查 vectors 目录是否存在）
    bool needWriteDim = !utils::dirExists(vectorsDir); // 判断 vectors 目录是否存在

    // 如果 vectors 目录不存在，则创建
    if (!utils::dirExists(vectorsDir)) {
        if (utils::mkdir(vectorsDir.c_str()) != 0) {
            std::cerr << "Failed to make directory: " << vectorsDir << std::endl;
            return;
        }
    }

    std::string filePath = vectorsDir + "/vectors.txt";

    // 打开文件以追加模式写入
    std::ofstream outFile(filePath, std::ios::binary | std::ios::app);
    if (!outFile) {
        std::cerr << "Failed to open file: " << filePath << std::endl;
        return;
    }

    const uint64_t dim = 768;

    // 仅在第一次创建文件时写入维度信息
    if (needWriteDim) {
        // std::cout << "NEED" <<std ::endl;
        outFile.write(reinterpret_cast<const char *>(&dim), sizeof(uint64_t));
    }

    // 遍历写入 vectorMap 中的每一项
    for (const auto &[key, vec] : vectorMap) {
        if (vec.size() != dim) {
            std::cerr << "Invalid vector size for key: " << key << std::endl;
            continue;
        }

        outFile.write(reinterpret_cast<const char *>(&key), sizeof(uint64_t));
        outFile.write(reinterpret_cast<const char *>(vec.data()), sizeof(float) * dim);
    }

    outFile.close();
}

void KVStore::load_embedding_from_disk(const std::string &data_root) {
    std::string filePath = data_root + "/vectors/vectors.txt";

    std::ifstream headFile(filePath, std::ios::binary | std::ios::ate);
    if (!headFile) {
        std::cerr << "Failed to open vector file: " << filePath << std::endl;
        return;
    }

    std::streamsize fileSize = headFile.tellg();
    if (fileSize < sizeof(uint64_t)) {
        std::cerr << "File too small: missing dimension info." << std::endl;
        return;
    }

    // Step 1: 获取向量维度 dim
    headFile.seekg(0, std::ios::beg);
    uint64_t dim = 0;
    headFile.read(reinterpret_cast<char *>(&dim), sizeof(uint64_t));

    // Step 2: 定义每个块的大小
    const std::streamsize blockSize  = sizeof(uint64_t) + sizeof(float) * dim;
    const std::streamsize blockStart = sizeof(uint64_t); // block 开始偏移（跳过前8字节 dim）

    std::streamsize totalBlocks = (fileSize - blockStart) / blockSize;

    std::unordered_set<uint64_t> seenKeys;

    // Step 3: 从后往前遍历数据块
    for (int64_t i = totalBlocks - 1; i >= 0; --i) {
        std::streamoff offset = blockStart + i * blockSize;
        headFile.seekg(offset, std::ios::beg);

        uint64_t key;
        headFile.read(reinterpret_cast<char *>(&key), sizeof(uint64_t));

        // 已加载该 key，跳过
        if (seenKeys.count(key))
            continue;

        std::vector<float> vec(dim);
        headFile.read(reinterpret_cast<char *>(vec.data()), sizeof(float) * dim);

        // Step 5: 检查是否为删除标志
        bool isDeleted =
            std::all_of(vec.begin(), vec.end(), [](float val) { return val == std::numeric_limits<float>::max(); });

        if (!isDeleted) {
            vectorMap[key] = std::move(vec);
        }

        seenKeys.insert(key);
    }

    headFile.close();
}

void KVStore::save_hnsw_index_to_disk(const std::string &hnsw_data_root) {
    if (this->searchRoute.allNodes.empty()) {
        return;
    }
    // 如果 目录不存在，则创建
    if (!utils::dirExists(hnsw_data_root)) {
        if (utils::mkdir(hnsw_data_root.c_str()) != 0) {
            std::cerr << "Failed to make directory: " << hnsw_data_root << std::endl;
            return;
        }
    }
    // global information
    std::string globalfilePath = hnsw_data_root + "/global_header.bin";
    std::ofstream outFile(globalfilePath, std::ios::binary | std::ios::trunc);
    if (!outFile) {
        std::cerr << "Failed to open file: " << globalfilePath << std::endl;
        return;
    }
    const uint32_t dim     = 768;
    const uint32_t MapSize = searchRoute.allNodes.size();
    outFile.write(reinterpret_cast<const char *>(&searchRoute.M), sizeof(uint32_t));
    outFile.write(reinterpret_cast<const char *>(&searchRoute.M_max), sizeof(uint32_t));
    outFile.write(reinterpret_cast<const char *>(&searchRoute.efConstruction), sizeof(uint32_t));
    outFile.write(reinterpret_cast<const char *>(&searchRoute.m_L), sizeof(uint32_t));
    outFile.write(reinterpret_cast<const char *>(&MapSize), sizeof(uint32_t));
    outFile.write(reinterpret_cast<const char *>(&dim), sizeof(uint32_t));

    // consider the entryPoint and save the main key
    // std::cout << "Entry Key:" << searchRoute.entryPoint->nodeKey << std::endl;
    uint32_t entry = 0;
    if (searchRoute.entryPoint) {
        entry = searchRoute.entryPoint->nodeID;
    }
    outFile.write(reinterpret_cast<const char *>(&entry), sizeof(uint32_t));
    outFile.write(reinterpret_cast<const char *>(&searchRoute.all_counts), sizeof(uint32_t));
    outFile.close();

    // deleted nodes
    std::string deleted_Path = hnsw_data_root + "/deleted_nodes.bin";
    std::ofstream outFile2(deleted_Path, std::ios::binary | std::ios::trunc);
    if (!outFile2) {
        std::cerr << "Failed to open file: " << deleted_Path << std::endl;
        return;
    }

    for (const auto &vec : searchRoute.deleted_nodes) {
        if (vec.size() != dim) {
            continue;
        }
        outFile2.write(reinterpret_cast<const char *>(vec.data()), sizeof(float) * dim);
    }
    outFile2.close();

    // node information
    std::string node_root = hnsw_data_root + "/nodes";
    if (!utils::dirExists(node_root)) {
        if (utils::mkdir(node_root.c_str()) != 0) {
            std::cerr << "Failed to make directory: " << node_root << std::endl;
            return;
        }
    }
    for (int node_id = 0; node_id < this->searchRoute.allNodes.size(); ++node_id) {
        std::string node_sub_root = hnsw_data_root + "/nodes/" + std::to_string(node_id);
        if (!utils::dirExists(node_sub_root)) {
            if (utils::mkdir(node_sub_root.c_str()) != 0) {
                std::cerr << "Failed to make directory: " << node_sub_root << std::endl;
                return;
            }
        }

        // node head
        std::string node_header_path = hnsw_data_root + "/nodes/" + std::to_string(node_id) + "/header.bin";
        std::ofstream outFileNode(node_header_path, std::ios::binary | std::ios::trunc);
        if (!outFileNode) {
            std::cerr << "Failed to open file: " << deleted_Path << std::endl;
            return;
        }
        outFileNode.write(
            reinterpret_cast<const char *>(&this->searchRoute.allNodes[node_id]->level), sizeof(uint32_t)
        );
        outFileNode.write(
            reinterpret_cast<const char *>(&this->searchRoute.allNodes[node_id]->nodeKey), sizeof(uint64_t)
        );
        outFileNode.write(
            reinterpret_cast<const char *>(&this->searchRoute.allNodes[node_id]->isValid), sizeof(uint64_t)
        );
        outFileNode.write(
            reinterpret_cast<const char *>(&this->searchRoute.allNodes[node_id]->nodeID), sizeof(uint32_t)
        );
        outFileNode.close();

        // node vec
        std::string Sdata_path = node_sub_root + "/data.bin";
        std::ofstream dataFile(Sdata_path, std::ios::binary | std::ios::trunc);
        if (!dataFile) {
            std::cerr << "Failed to open file: " << Sdata_path << std::endl;
            return;
        }
        const auto &vec = this->searchRoute.allNodes[node_id]->data;
        if (vec.size() != dim) {
            std::cerr << "Vector dimension mismatch at node " << node_id << std::endl;
            return;
        }
        dataFile.write(reinterpret_cast<const char *>(vec.data()), sizeof(float) * dim);
        dataFile.close();

        // node string
        std::string value_path = node_sub_root + "/value.bin";
        std::ofstream valueFile(value_path, std::ios::binary | std::ios::trunc);
        if (!valueFile) {
            std::cerr << "Failed to open file: " << value_path << std::endl;
            return;
        }
        const std::string &str = this->searchRoute.allNodes[node_id]->value;
        uint32_t len           = static_cast<uint32_t>(str.size());
        valueFile.write(reinterpret_cast<const char *>(&len), sizeof(uint32_t)); // 写长度
        valueFile.write(str.data(), len);                                        // 写数据
        valueFile.close();

        // node layers
        std::string node_edges = node_sub_root + "/edges";
        if (!utils::dirExists(node_edges)) {
            if (utils::mkdir(node_edges.c_str()) != 0) {
                std::cerr << "Failed to make directory: " << node_edges << std::endl;
                return;
            }
        }

        int maxL = searchRoute.allNodes[node_id]->level;
        // std::cout << "Node " << node_id <<std::endl;
        for (int s = 0; s <= maxL; ++s) {
            std::string edge_path = node_edges + "/" + std::to_string(s) + ".bin";
            std::ofstream edgeFile(edge_path, std::ios::binary | std::ios::trunc);

            if (!edgeFile) {
                std::cerr << "Failed to open file: " << edge_path << std::endl;
                return;
            }
            uint32_t BlockTotal = searchRoute.allNodes[node_id]->connections[s].size();
            edgeFile.write(reinterpret_cast<const char *>(&BlockTotal), sizeof(uint32_t));
            // std::cout <<"Level "<<s<<":";
            for (const auto &tmp_nodes : searchRoute.allNodes[node_id]->connections[s]) {
                uint32_t tmpKey = tmp_nodes->nodeID;
                // std::cout << tmpKey << " ";
                edgeFile.write(reinterpret_cast<const char *>(&tmpKey), sizeof(uint32_t));
            }
            // std::cout << std::endl;
            edgeFile.close();
        }
    }
}

void KVStore::load_hnsw_index_from_disk(const std::string &hnsw_data_root) {
    // load vectors first
    load_embedding_from_disk("./embedding_data");

    uint32_t mapSize = 0;
    uint32_t dim     = 0;
    uint32_t entryID = 0;
    uint32_t alls    = 0;
    this->searchRoute.clear();

    // read the global information
    std::string global_head = hnsw_data_root + "global_header.bin";
    std::ifstream headFile(global_head, std::ios::binary);
    if (!headFile) {
        std::cerr << "Failed to open file:" << global_head << std::endl;
        return;
    }

    const int mount_head = 8;
    std::vector<uint32_t> vec(mount_head);

    headFile.read(reinterpret_cast<char *>(vec.data()), sizeof(uint32_t) * mount_head);

    if (!headFile || vec.size() != 8) {
        std::cerr << "Unable to read:" << global_head << std::endl;
    } else {
        searchRoute.M              = vec[0];
        searchRoute.M_max          = vec[1];
        searchRoute.efConstruction = vec[2];
        searchRoute.m_L            = vec[3];
        mapSize                    = vec[4];
        dim                        = vec[5];
        entryID                    = vec[6];
        alls                       = vec[7];
    }
    searchRoute.all_counts = alls;

    headFile.close();

    // read nodes:
    searchRoute.allNodes.resize(mapSize, nullptr);
    std::string node_root_path = hnsw_data_root + "nodes";
    for (uint32_t id = 0; id < mapSize; ++id) {
        std::string node_bag = node_root_path + "/" + std::to_string(id); // sub_dir
        if (!utils::dirExists(node_bag)) {
            std::cout << "Failed to find :" << node_bag;
            return;
        }

        // fill with header
        std::string node_head_path = node_bag + "/header.bin";
        std::ifstream nodeFile(node_head_path, std::ios::binary);
        if (!nodeFile) {
            std::cerr << "Failed to open file:" << node_head_path << std::endl;
            return;
        }

        uint32_t node_level;
        uint64_t node_key;
        uint64_t validness;
        uint64_t nodeiD;

        nodeFile.read(reinterpret_cast<char *>(&node_level), sizeof(uint32_t));
        nodeFile.read(reinterpret_cast<char *>(&node_key), sizeof(uint64_t));
        nodeFile.read(reinterpret_cast<char *>(&validness), sizeof(uint64_t));
        nodeFile.read(reinterpret_cast<char *>(&nodeiD), sizeof(uint32_t));

        // fill with data(vec)
        std::string node_vec_path = node_bag + "/data.bin";
        std::ifstream nodeVecFile(node_vec_path, std::ios::binary);
        if (!nodeVecFile) {
            std::cerr << "Failed to open file:" << node_vec_path << std::endl;
            return;
        }
        std::vector<float> tVecNode(dim);
        nodeVecFile.read(reinterpret_cast<char *>(tVecNode.data()), sizeof(float) * dim);
        nodeVecFile.close();

        // fill with value(string)
        std::string node_value_path = node_bag + "/value.bin";
        std::ifstream nodeValFile(node_value_path, std::ios::binary);
        if (!nodeValFile) {
            std::cerr << "Failed to open file:" << node_value_path << std::endl;
            return;
        }
        uint32_t str_len = 0;
        nodeValFile.read(reinterpret_cast<char *>(&str_len), sizeof(uint32_t));
        std::string value_str(str_len, '\0');
        nodeValFile.read(&value_str[0], str_len);
        nodeValFile.close();

        // 创建节点对象并填入
        auto *newNode    = new SearchLayers::Node(node_key, std::move(value_str), std::move(tVecNode), node_level);
        newNode->isValid = validness;
        searchRoute.allNodes[id] = newNode;
        newNode->nodeID          = nodeiD;
    }

    for (uint32_t id = 0; id < mapSize; ++id) {
        auto *curNode = searchRoute.allNodes[id];
        if (!curNode) {
            std::cerr << "Invalid node at id: " << id << std::endl;
            continue;
        }

        std::string node_bag = node_root_path + "/" + std::to_string(id);
        for (int curlevel = 0; curlevel <= curNode->level; ++curlevel) {
            std::string edge_path = node_bag + "/edges/" + std::to_string(curlevel) + ".bin";
            std::ifstream edgeFile(edge_path, std::ios::binary);
            if (!edgeFile) {
                std::cerr << "Failed to open file: " << edge_path << std::endl;
                return;
            }

            uint32_t edge_count = 0;
            edgeFile.read(reinterpret_cast<char *>(&edge_count), sizeof(uint32_t));

            std::vector<uint32_t> neighbor_ids(edge_count);
            edgeFile.read(reinterpret_cast<char *>(neighbor_ids.data()), edge_count * sizeof(uint32_t));

            for (uint32_t nid : neighbor_ids) {
                if (nid >= mapSize || searchRoute.allNodes[nid] == nullptr) {
                    std::cerr << "Invalid neighbor id: " << nid << " for node " << id << " at level " << curlevel
                              << std::endl;
                    continue;
                }
                curNode->connections[curlevel].push_back(searchRoute.allNodes[nid]);
            }
        }
    }

    // set entryPoint
    if (searchRoute.allNodes.size() <= entryID) {
        std::cerr << "Entry not exist!" << std::endl;
        return;
    }
    searchRoute.entryPoint = searchRoute.allNodes[entryID];
}

std::vector<float> parseEmbeddingLine(const std::string &line) {
    std::vector<float> embedding;
    size_t start = line.find('[');
    size_t end   = line.find(']');
    if (start == std::string::npos || end == std::string::npos || end <= start) {
        throw std::runtime_error("Malformed embedding line.");
    }

    std::string content = line.substr(start + 1, end - start - 1);
    std::stringstream ss(content);
    std::string token;
    while (std::getline(ss, token, ',')) {
        embedding.push_back(std::stof(token));
    }

    if (embedding.size() != 768) {
        throw std::runtime_error("Expected 768 dimensions but got " + std::to_string(embedding.size()));
    }

    return embedding;
}

// consider read from the two big data file.

void KVStore::read_build_from_text(const std::string &string_path, const std::string &vector_path,uint64_t max) {
    // read into buffer and then merge into the map.
    // read the text

    std::ifstream textFile(string_path);
    std::ifstream embFile(vector_path);

    if (!textFile.is_open() || !embFile.is_open()) {
        throw std::runtime_error("Failed to open one or both input files.");
    }

    std::string textLine, embLine;
    size_t lineIndex = 0;
    uint32_t count   = 0;

    // 在函数开始前定义累计时间变量
    auto start_total = high_resolution_clock::now();

    uint64_t total_parse_time_us      = 0;
    uint64_t total_insertNode_time_us = 0;
    uint64_t total_memInsert_time_us  = 0;
    uint64_t total_compact_time_ms    = 0;
    uint32_t compact_count            = 0;

    // 主循环中加入计时
    while (std::getline(textFile, textLine) && std::getline(embFile, embLine) && count < max) {
        if (lineIndex % 2 == 0) {
            try {
                auto start_parse       = high_resolution_clock::now();
                std::vector<float> vec = parseEmbeddingLine(embLine);
                vectorMap[count] = vec;
                auto end_parse         = high_resolution_clock::now();
                total_parse_time_us += duration_cast<microseconds>(end_parse - start_parse).count();

                auto start_insertNode = high_resolution_clock::now();
                this->searchRoute.insertNode(count, textLine, vec);
                auto end_insertNode = high_resolution_clock::now();
                total_insertNode_time_us += duration_cast<microseconds>(end_insertNode - start_insertNode).count();

                auto start_memInsert = high_resolution_clock::now();
                uint32_t nxtsize     = s->getBytes();
                std::string res      = s->search(count);
                if (!res.length()) {
                    nxtsize += 12 + textLine.length();
                } else {
                    nxtsize = nxtsize - res.length() + textLine.length();
                }

                if (nxtsize + 10240 + 32 <= MAXSIZE) {
                    s->insert(count, textLine);
                } else {
                    auto start_compact = high_resolution_clock::now();
                    sstable ss(s);
                    s->reset();
                    std::string url  = ss.getFilename();
                    std::string path = "./data/level-0";
                    if (!utils::dirExists(path)) {
                        utils::mkdir(path.data());
                        totalLevel = 0;
                    }
                    addsstable(ss, 0);
                    ss.putFile(url.data());
                    compaction();
                    auto end_compact = high_resolution_clock::now();
                    total_compact_time_ms += duration_cast<milliseconds>(end_compact - start_compact).count();
                    ++compact_count;

                    s->insert(count, textLine);
                }
                auto end_memInsert = high_resolution_clock::now();
                total_memInsert_time_us += duration_cast<microseconds>(end_memInsert - start_memInsert).count();
                //std::cout << "finished: "<< count << std::endl;

                ++count;
                
            } catch (const std::exception &ex) {
                std::cerr << "Error at line " << lineIndex << ": " << ex.what() << std::endl;
            }
        }
        ++lineIndex;
    }

    auto end_total    = high_resolution_clock::now();
    auto total_time_s = duration_cast<seconds>(end_total - start_total).count();

    // 输出汇总结果
    std::cout << "\n====== Performance Summary ======\n";
    std::cout << "Total entries loaded: " << count << "\n";
    std::cout << "Total time taken: " << total_time_s << " s\n\n";

    std::cout << "[Parse Embedding] Total: " << total_parse_time_us / 1000.0
              << " ms, Average: " << (count ? total_parse_time_us / count : 0) << " us\n";

    std::cout << "[Insert to Route] Total: " << total_insertNode_time_us / 1000.0
              << " ms, Average: " << (count ? total_insertNode_time_us / count : 0) << " us\n";

    std::cout << "[Insert to Memtable] Total: " << total_memInsert_time_us / 1000.0
              << " ms, Average: " << (count ? total_memInsert_time_us / count : 0) << " us\n";

    std::cout << "[Compaction] Total: " << total_compact_time_ms << " ms, Times triggered: " << compact_count
              << ", Avg per compaction: " << (compact_count ? (total_compact_time_ms * 1000) / compact_count : 0)
              << " us\n";

    std::cout << "=================================\n";
}

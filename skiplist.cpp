//
// Created by zzm on 25-3-18.
//

#include "skiplist.h"

double skiplist::my_rand() {
    return (double)rand() / RAND_MAX;
}

int skiplist::randLevel() {
    int level = 1;
    while (my_rand() < p && level < MAX_LEVEL) {
        level++;
    }
    return level;
}

void skiplist::insert(uint64_t key, const std::string &str) {
    slnode *x = head;
    slnode *update[MAX_LEVEL];

    // **手动初始化 update 数组**
    for (int i = 0; i < MAX_LEVEL; ++i) {
        update[i] = nullptr;
    }

    // **寻找插入位置，并填充 update 数组**
    for (int level = curMaxL - 1; level >= 0; --level) {
        while (x->nxt[level] && x->nxt[level]->key < key) {
            x = x->nxt[level];
        }
        update[level] = x;  // **存储当前层的前驱节点**
    }

    // **检查 key 是否已存在**
    x = x->nxt[0];
    if (x && x->key == key) {
        x->val = str; // **更新已存在 key 的值**
        return;
    }

    // **生成新节点的层数**
    int newLevel = randLevel();
    newLevel = std::min(newLevel, MAX_LEVEL);  // 确保不超过 MAX_LEVEL

    // **如果新层数大于当前最大层，初始化 update**
    if (newLevel > curMaxL) {
        for (int i = curMaxL; i < newLevel; ++i) {
            update[i] = head;  // **所有新层的前驱节点设为 head**
        }
        curMaxL = newLevel;  // **更新当前最大层数**
    }

    // **创建新节点**
    auto *newNode = new slnode(key, str, NORMAL);

    // **插入新节点**
    for (int i = 0; i < newLevel; ++i) {
        newNode->nxt[i] = update[i]->nxt[i];  // **链接后继节点**
        update[i]->nxt[i] = newNode;  // **前驱节点指向新节点**
    }

    // **更新跳表的总字节数**
    bytes += sizeof(slnode) + str.size();
}

std::string skiplist::search(uint64_t key) {
    slnode *x = head;
    for (int i = curMaxL - 1; i >= 0; --i) {
        while (x->nxt[i] && x->nxt[i]->key < key) {
            x = x->nxt[i];
        }
    }
    x = x->nxt[0];
    if (x && x->key == key) {
        return x->val;
    }
    return "";
}

bool skiplist::del(uint64_t key, uint32_t len) {
    slnode *update[MAX_LEVEL];
    slnode *x = head;

    for (int i = curMaxL - 1; i >= 0; --i) {
        while (x->nxt[i] && x->nxt[i]->key < key) {
            x = x->nxt[i];
        }
        update[i] = x;
    }
    x = x->nxt[0];

    if (!x || x->key != key) {
        return false;
    }

    for (int i = 0; i < curMaxL; ++i) {
        if (update[i]->nxt[i] != x) break;
        update[i]->nxt[i] = x->nxt[i];
    }

    delete x;
    bytes -= len;

    while (curMaxL > 1 && head->nxt[curMaxL - 1] == tail) {
        curMaxL--;
    }
    return true;
}

void skiplist::scan(uint64_t key1, uint64_t key2,
                    std::vector<std::pair<uint64_t, std::string>> &list) {
    slnode *x = head;
    for (int i = curMaxL - 1; i >= 0; --i) {
        while (x->nxt[i] && x->nxt[i]->key < key1) {
            x = x->nxt[i];
        }
    }
    x = x->nxt[0];

    while (x && x->key <= key2) {
        if (x->type == NORMAL) {
            list.emplace_back(x->key, x->val);
        }
        x = x->nxt[0];
    }
}

slnode *skiplist::lowerBound(uint64_t key) {
    slnode *x = head;
    for (int i = curMaxL - 1; i >= 0; --i) {
        while (x->nxt[i] && x->nxt[i]->key < key) {
            x = x->nxt[i];
        }
    }
    return x->nxt[0];
}

void skiplist::reset() {
    slnode *x = head->nxt[0];
    while (x != tail) {
        slnode *next = x->nxt[0];
        delete x;
        x = next;
    }
    for (int i = 0; i < MAX_LEVEL; ++i) {
        head->nxt[i] = tail;
    }
    curMaxL = 1;
    bytes = 0;
}

uint32_t skiplist::getBytes() {
    return this->bytes;
}

# Project Stage1 LSM-KV: KVStore using Log-structured Merge Tree

## Overview
This project implements a key-value store based on the Log-Structured Merge-Tree (LSM-Tree) architecture. The system consists of memory components using SkipList and disk components using hierarchical SSTables, with Bloom Filters for efficient key existence checks.

## Project Structure
```
.
├── CMakeLists.txt            // CMake configuration
├── MurmurHash3.h             // MurmurHash3 implementation
├── README.md                 // Project documentation
├── bloom.cpp                 // Bloom Filter implementation
├── bloom.h                   // Bloom Filter header
├── correctness.cc            // Correctness test (do not modify)
├── kvstore.cc                // KVStore core implementation
├── kvstore.h                 // KVStore header
├── kvstore_api.h             // KVStore API interface (do not modify)
├── persistence.cc            // Persistence test (do not modify)
├── skiplist.cpp              // SkipList implementation
├── skiplist.h                // SkipList header
├── sstable.cpp               // SSTable disk operations
├── sstable.h                 // SSTable data structure
├── sstablehead.cpp           // SSTable header management
├── sstablehead.h             // SSTable header definition
├── test.h                    // Testing base class (do not modify)
└── utils.h                   // Cross-platform file utilities
```

## Key Components

### Core Modules
• **SkipList (skiplist.h/cpp)**  
  In-memory ordered data structure storing recent writes with O(log n) time complexity for insert/search operations.

• **SSTable (sstable.h/cpp)**  
  Disk-based storage structure containing:
  • Header section (timestamp, entry count, key range)
  • Bloom Filter section for fast key existence checks
  • Key-Value entries sorted by key

• **SSTable Header (sstablehead.h/cpp)**  
  Manages SSTable metadata including:
  • Timestamp generation
  • Key range tracking (min/max keys)
  • Entry count management

• **Bloom Filter (bloom.h/cpp)**  
  Probabilistic data structure using MurmurHash3 for efficient membership queries, reducing unnecessary disk reads.

### Interface
• **KVStoreAPI (kvstore_api.h)**  

core APIs:
  ```cpp
  class KVStoreAPI {
    virtual void put(uint64_t key, const std::string &s) = 0;
    virtual std::string get(uint64_t key) = 0;
    virtual bool del(uint64_t key) = 0;
    };

  ```

### Storage Hierarchy
1. **Memory Tier**  
   • Active SkipList for write operations
   • Immutable SkipLists ready for compaction

2. **Disk Tier**  
   • Leveled SSTable organization
   • Compaction process merges and reorganizes SSTables

## Build & Test

### Build Instructions
```bash
cmake -B build             # Initial configuration
make -C build              # Compile project
```

### Run Tests
```bash
./build/correctness        # Validate functional correctness
./build/persistence        # Verify data durability
```

Test execution may take **several minutes** to complete due to comprehensive stress testing. Ensure all test files are restored to original versions before final submission.

## Implementation Notes

1. Please read all the documents we provide carefully to make sure you understand all the details of implementing lsm-tree.
2. You can continue to implement it based on the sstable.cpp file we implemented, or you can implement it completely by yourself. If you choose to implement it completely by yourself, please make sure you implement it according to the conventions of kvstore_api.h.
3. You should implement code in `kvstore.cc` and another file(*go and find it*), you can search `TODO here` to help you locate the position.
4. To reduce your workload, **you only have three todos**, but be sure to implement them carefully to ensure that you understand the lsm tree.

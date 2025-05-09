cmake_minimum_required(VERSION 3.10)
project(LSM_TREE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# 强制使用 Release 模式
set(CMAKE_BUILD_TYPE Release)

# 设置 Release 模式下的优化等级为 -O1
set(CMAKE_C_FLAGS_RELEASE "-O1 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O1 -DNDEBUG")

add_subdirectory(third_party/llama.cpp)

add_subdirectory(embedding)

add_subdirectory(test)

add_executable(correctness correctness.cc kvstore_api.h kvstore.h
        kvstore.cc skiplist.cpp skiplist.h sstable.cpp sstable.h
        bloom.cpp bloom.h MurmurHash3.h utils.h test.h
        sstablehead.cpp sstablehead.h)

add_executable(persistence persistence.cc kvstore_api.h kvstore.h kvstore.cc
        skiplist.cpp skiplist.h sstable.cpp sstable.h
        bloom.cpp bloom.h MurmurHash3.h utils.h test.h
        sstablehead.cpp sstablehead.h)

add_executable(timeCompare timeCompare.cc kvstore_api.h kvstore.h kvstore.cc
        skiplist.cpp skiplist.h sstable.cpp sstable.h
        bloom.cpp bloom.h MurmurHash3.h utils.h test.h
        sstablehead.cpp sstablehead.h)

add_executable(VectorPersistent1 Vector_Persistent_Test_Phase1.cpp kvstore_api.h kvstore.h kvstore.cc
        skiplist.cpp skiplist.h sstable.cpp sstable.h
        bloom.cpp bloom.h MurmurHash3.h utils.h test.h
        sstablehead.cpp sstablehead.h)

add_executable(VectorPersistent2 Vector_Persistent_Test_Phase2.cpp kvstore_api.h kvstore.h kvstore.cc
        skiplist.cpp skiplist.h sstable.cpp sstable.h
        bloom.cpp bloom.h MurmurHash3.h utils.h test.h
        sstablehead.cpp sstablehead.h)

add_executable(HNSWDeleteTest HNSW_Delete_Test.cpp kvstore_api.h kvstore.h kvstore.cc
        skiplist.cpp skiplist.h sstable.cpp sstable.h
        bloom.cpp bloom.h MurmurHash3.h utils.h test.h
        sstablehead.cpp sstablehead.h)

add_executable(HNSWPersistent1 HNSW_Persistent_Test_Phase1.cpp kvstore_api.h kvstore.h kvstore.cc
        skiplist.cpp skiplist.h sstable.cpp sstable.h
        bloom.cpp bloom.h MurmurHash3.h utils.h test.h
        sstablehead.cpp sstablehead.h)

add_executable(HNSWPersistent2 HNSW_Persistent_Test_Phase2.cpp kvstore_api.h kvstore.h kvstore.cc
        skiplist.cpp skiplist.h sstable.cpp sstable.h
        bloom.cpp bloom.h MurmurHash3.h utils.h test.h
        sstablehead.cpp sstablehead.h)
# 链接 embedding 模块
target_link_libraries(correctness PRIVATE embedding)
target_link_libraries(persistence PRIVATE embedding)
target_link_libraries(timeCompare PRIVATE embedding)
target_link_libraries(VectorPersistent1 PRIVATE embedding)
target_link_libraries(VectorPersistent2 PRIVATE embedding)
target_link_libraries(HNSWDeleteTest PRIVATE embedding)
target_link_libraries(HNSWPersistent1 PRIVATE embedding)
target_link_libraries(HNSWPersistent2 PRIVATE embedding)

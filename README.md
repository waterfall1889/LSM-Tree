# LSM-Tree Project
这是一个基于LSM-Tree的向量数据库项目。LSM-TREE 是一种高效的存储结构。分为内存部分和磁盘存储的部分。LSM-Tree适合写入任务密集的场景，就效率而言PUT操作效率最高，GET操作效率较高，DEL操作效率最低。整体分为内存部分和磁盘部分的存储。
内存部分通常使用红黑树或者跳表存储，当存储达到一定规模后，利用sstable中的接口把跳表转化成sstable文件存储进入磁盘。磁盘部分利用sstable文件存储，sstable本身不可以直接修改。SSTable 内部存储的键值对（Key-Value pairs）是按照 Key 严格排序的，这是可以进行高效多路归并排序的基础。 LSM 树通常将 SSTable 组织成多个层级（Level）。较新的数据在较浅的层级（如 L0），随着数据的合并和老化，会逐渐移动到更深的层级（L1, L2, ...）。深层的 SSTable 通常更大，覆盖的 Key 范围也更广。SSTable Head 特指一个 SSTable 文件开头部分的元数据区域，或者更广义地说，是指访问一个 SSTable 所需的最关键元信息。可以通过一个独立的结构存储对应的head信息，管理对应的sstable。

-----
## PUT操作
PUT操作会先尝试插入内存的跳表。如果因为插入会造成跳表大小溢出，则进行合并compaction操作。Compaction从level-0向底层合并。
## GET操作
GET操作先查找当前跳表的元素，再查找磁盘的文件。
## DEL操作
DEL操作实际上用了tombstone的思想，先查找对应的数据是否存在，然后选择插入对应key的DEL marker，在合并的时候去除掉旧的记录即可。
## Compaction
合并操作从level-0开始进行，第k层最多的文件数量为个。第0层超出限制后，全部参与合并；其余层只选择合并超出数量的文件中时间戳最小、最旧的记录。在下一层中找到与这些文件存在区间重叠的文件进行合并，利用多路归并排序并去除重复键值中时间戳较小的，得到一个堆，构建跳表依次插入，每次插满一个跳表进行一次转化，转化成sstable，存储到下一层。然后依次向下合并，直到完成。
## 字符串向量化
使用embedding模型，对于集中的插入阶段，可以先用vector临时存储，等到compaction或者需要搜索时集中转化。因为重复加载模型会极大影响时间性能。
## 向量持久化
向量的持久化归并为一个文件（对应VectorMap），先储存维数再依次存储key和向量。
## HNSW的节点插入
HNSW的主要参数有M（插入时默认连接数），M_max（最大连接数），m_L（层数），efConstruction（维护的最近节点数量）。M，M_max，efConstruction和正确率成正相关，和时间性能呈负相关。插入时先随机生成层数，然后从顶层入口开始向下找到距离该节点最近的入口，直到该节点最高层的上一层的最高层。然后下面的层依次利用类似于BFS找出连接上比较近的efConstruction个节点，选其中的最近的M个建立连接。
## HNSW的搜索
从最顶层开始查找下一层入口。实际为了避免局部最优可以找距离目标点最近的前三个入口点随机选择其中之一。到最低层时类似于BFS思想，找出连接上比较近的efConstruction个节点，选其中的最近的k个作为结果。
## HNSW的删除和覆盖插入
删除时在对应节点上标记已经删除，但是节点本身保留。覆盖插入把原来节点标记为删除，然后重新插入即可，这样可以减少维护时间。
## HNSW的持久化
持久化先储存global head。然后接着分节点存储信息，包括邻接表、点的数据。

---
有关该实验项目，先后进行了五次迭代，第一次迭代完善了基本的Compaction、PUT、DELETE、GET操作，第二次迭代完善了使用embedding模型进行向量化、语义近似性查找的过程，第三次迭代完成了HNSW算法有关数据结构的构建，第四次迭代完成了HNSW算法的持久化存储，第五次迭代完成了有关并行化的优化。具体的测试报告可以在目录下的report子目录中找到。

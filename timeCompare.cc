#include "test.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <fstream>
#include <iostream>
#include <string>
#include <cassert>
#include <chrono>

std::vector<std::string> read_file(std::string filename) {
	// read file from ./data/trimmed_text
	// every line is a string
	// skip all the lines that are empty
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr<<"Failed to open file: "<<filename<<std::endl;
        return {};
    }
    std::string line;
    std::vector<std::string> temp;
    while (std::getline(file, line)) {
        //std::cout<<line<<std::endl;
        // check if all characters in the line cannot be seen
        // 检查是不是全是不可见字符
        bool exist_alpha = false;
        for (auto c : line) {
            if (isalpha(c)) {
                exist_alpha = true;
                break;
            }
        }
        if (!exist_alpha) {
            continue;
        }
        if (line.empty())
            continue;
        if(line.size() < 30) {
            continue;
        }
        // std::cout<<line<<std::endl;
        temp.push_back(line);
    }
    file.close();
    return temp;
}

class timeTest : public Test {
private:
public:
    timeTest(const std::string &dir, bool v = true) : Test(dir, v) {}

    void start_test(void *args = NULL) override {
        auto start = std::chrono::high_resolution_clock::now();
        std::cout << "Time Test" << std::endl;
        
        store.reset();
        std::cout << "[Time Compare Test]" << std::endl;
        auto trimmed_text = read_file("./data/trimmed_text.txt");
        uint64_t max = 80;
        max = std::min(max, (uint64_t)trimmed_text.size());
        
        
        auto end_prepare = std::chrono::high_resolution_clock::now();
        auto duration1 = std::chrono::duration_cast<std::chrono::milliseconds>(end_prepare - start);
        std::cout << "End preparation!"<<std::endl;
        std::cout << "Prepare time cost:"<< duration1.count()<<"ms"<<std::endl;
        
        
        for (int i = 0; i < max; ++i) {
            store.put(i, trimmed_text[i]);//put
        }
        
        
        auto end_put = std::chrono::high_resolution_clock::now();
        auto duration2 = std::chrono::duration_cast<std::chrono::milliseconds>(end_put - end_prepare);
        std::cout << "End put "<< max << " elements!"<<std::endl;
        std::cout << "Put time cost:"<< duration2.count()<<"ms"<<std::endl;
        
        
        auto test_text = read_file("./data/test_text.txt");
        max = std::min(max, (uint64_t)test_text.size());

        std::vector<std::string> ans;
        std::ifstream file("./data/test_text_ans.txt");
        if (!file.is_open()) {
            std::cerr << "Failed to open the file" << std::endl;
            return;
        }
        std::string line;
        while (std::getline(file, line)) {
            ans.push_back(line);
        }
        
        auto end_read = std::chrono::high_resolution_clock::now();
        auto duration3 = std::chrono::duration_cast<std::chrono::milliseconds>(end_read - end_put);
        std::cout << "End read!"<<std::endl;
        std::cout << "Read time cost:"<< duration3.count()<<"ms"<<std::endl;
        
        file.close();
        
        int idx = 0;
        
        for (int i = 0; i < max; ++i) {
            std::cout << "Route:"<<(i+1)<<std::endl;
            auto res = store.search_knn(test_text[i], 5);
            for(auto j : res) {
                idx++;
            }
        }
        
        
        auto End = std::chrono::high_resolution_clock::now();
        auto duration4 = std::chrono::duration_cast<std::chrono::milliseconds>(End - end_read);
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(End - start);
        std::cout << "End search "<< idx <<" elements!"<<std::endl;
        std::cout << "Search time cost:"<< duration4.count()<<"ms"<<std::endl;
        std::cout << "End time test!"<<std::endl;
        std::cout << "Total time cost:"<< duration.count()<<"ms"<<std::endl;
    }
    
};

int main(int argc, char *argv[]) {
    bool verbose = (argc == 2 && std::string(argv[1]) == "-v");

    std::cout << "Usage: " << argv[0] << " [-v]" << std::endl;
    std::cout << "  -v: print extra info for failed tests [currently ";
    std::cout << (verbose ? "ON" : "OFF") << "]" << std::endl;
    std::cout << std::endl;
    std::cout.flush();

    timeTest test("./data", verbose);

    test.start_test();

    return 0;
}

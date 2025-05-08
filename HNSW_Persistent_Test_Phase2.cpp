#include "kvstore.h"
#include <fstream>
#include <iostream>
#include <vector>

std::vector<std::string> load_text(std::string filename) {
  std::ifstream file(filename);
  std::string line;
  std::vector<std::string> text;
  while (std::getline(file, line)) {
    text.push_back(line);
  }
  return text;
}

bool check_result(std::vector<std::pair<std::uint64_t, std::string>> result,
                  std::string text) {
  for (int i = 0; i < result.size(); i++) {
    if (result[i].second == text) {
      return true;
    }
  }
  return false;
}



int main() {
  KVStore store("data/");

  store.load_hnsw_index_from_disk("hnsw_data/");

  int pass = 0;
  int total = 128;
  int phase[4] = {0, 32, 64, 96};
  //store.searchRoute.printInfo();

  std::vector<std::string> text = load_text("data/trimmed_text.txt");

  //  delete test
  pass = 0;
  for(int i = phase[1]; i < phase[2]; ++i) {
    std::vector<std::pair<std::uint64_t, std::string>> result = store.search_knn_hnsw(text[i], 3);
    for(int k = 0; k < 3; ++k) {
      if(result[k].first == i) {
        std::cout << "Delete Test Error: value[" << i << "] is not deleted" << std::endl;
        std::cerr << "Test failed." << std::endl;
        return 0;
      }
    }
    pass++;
  }

  //  reinsert test
  pass = 0;
  for(int i = phase[2]; i < phase[3]; ++i) {
    std::vector<std::pair<std::uint64_t, std::string>> result = store.search_knn_hnsw(text[i], 3);
    if(result.size() != 3) {
      std::cout << "Reinsert Test Error: result.size() != 3" << std::endl;
      continue;
    }
    if(!check_result(result, text[i])) {
      std::cout << "Reinsert Test Error: value[" << i << "] is not inserted" << std::endl;
      continue;
    }
    pass++;
  }
  std::cout << "Passes: "<< pass <<std::endl;
  if((double)pass / 32.0 < 0.85) {
    std::cout << "Reinsert Test Error: accept rate is too low" << std::endl;
    std::cerr << "Test failed." << std::endl;
    //return 0;
  }

  //  replace test
  pass = 0;
  for(int i = phase[0]; i < phase[1]; ++i) {
    int j = i + phase[3];
    std::vector<std::pair<std::uint64_t, std::string>> result_i = store.search_knn_hnsw(text[i], 3);
    if(result_i[0].first == i) {
      std::cout << "Replace Test Error: value[" << i << "] is not deleted" << std::endl;
      continue;
    }
    std::vector<std::pair<std::uint64_t, std::string>> result_j = store.search_knn_hnsw(text[j], 3);
    if(result_j.size() != 3) {
      std::cout << "Replace Test Error: result_j.size() != 3" << std::endl;
      continue;
    }
    if(!check_result(result_j, text[j])) {
      std::cout << "Replace Test Error: value[" << j << "] is not inserted" << std::endl;
      continue;
    }
    pass++;
  }
  std::cout << "Passes: "<< pass << std::endl;
  if((double)pass / 32.0 < 0.85) {
    std::cout << "Replace Test Error: accept rate is too low" << std::endl;
    std::cerr << "Test failed." << std::endl;
    return 0;
  }

  std::cout << "Test passed" << std::endl;
  return 0;
}
#include "kvstore.h"
#include <fstream>
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

int main() {
  KVStore store("data/");
  store.reset();

  std::vector<std::string> text = load_text("data/trimmed_text.txt");
  int total = 128;
  for (int i = 0; i < total; i++) {
    store.put(i, text[i]);
  }

  // TODO: uncomment this line when you have implemented the function
  store.save_hnsw_index_to_disk("hnsw_data/");

  return 0;
}

/*#pragma once

#include <algorithm>
#include <ctime>
#include <numeric>
#include <string>
#include <vector>

#include "common.h"
#include "llama.h"
#include "log.h"

std::string join(const std::vector<std::string>& vec,
                 const std::string& delimiter);

std::vector<std::vector<float>> embedding(const std::string& prompt);

std::vector<float> embedding_single(const std::string& prompt);

std::vector<std::vector<float>> embedding_batch(const std::string& prompts);
*/
#pragma once

#include <atomic>
#include <numeric>
#include <string>
#include <vector>

#include "common.h"
#include "llama.h"
#include "log.h"

class LLMEmbedding;

const std::string MODEL = "./model/nomic-embed-text-v1.5.Q8_0.gguf";
const int ngl = 99;
const int CONTEXT_SIZE = 2048;
const int BATCH_SIZE = 2048;
const int ROPE_SCALING_YARN = 1;
const float ROPE_FREQ_SCALE = 0.75;
const bool PERFORMANCE_METRICS = true;
const bool LOG_DISABLE = true;

std::string join(const std::vector<std::string> &vec,
                 const std::string &delimiter) {
  if (vec.empty())
    return "";
  return std::accumulate(
      vec.begin() + 1, vec.end(), vec[0],
      [&delimiter](const std::string &a, const std::string &b) {
        return a + delimiter + b;
      });
}

class LLMEmbedding {
  // llama.cpp-specific types
  llama_context *_ctx;
  llama_model *_model;
  common_params _params;
  const struct llama_vocab *_vocab;
  std::atomic<bool> _initialized;

public:
  explicit LLMEmbedding(const std::string &modelPath);

  static LLMEmbedding &getInstance() {
    static LLMEmbedding instance(MODEL);
    return instance;
  }

  int embedding(const std::string &prompt, std::vector<float> &embeddings,
                int &n_embd, int &n_prompts);

  std::vector<float> embedding(const std::string &prompt);

  ~LLMEmbedding();

private:
  void initParams();

  void loadModel(const std::string &modelPath);

  static void batchDecode(llama_context *ctx, llama_batch &batch, float *output,
                          int n_seq, int n_embd, int embd_norm);
  static void batchAddSeq(llama_batch &batch,
                          const std::vector<int32_t> &tokens,
                          llama_seq_id seq_id);
  static std::vector<std::string>
  splitLines(const std::string &s, const std::string &separator = "\n");
};

void LLMEmbedding::initParams() {
  _params = common_params();

  _params.model = MODEL;

  _params.n_batch = BATCH_SIZE;

  _params.n_ctx = CONTEXT_SIZE;

  _params.rope_scaling_type = LLAMA_ROPE_SCALING_TYPE_YARN;

  _params.rope_freq_scale = ROPE_FREQ_SCALE;

  _params.embedding = true;
  // For non-causal models, batch size must be equal to ubatch size
  _params.n_ubatch = _params.n_batch;

  _params.no_perf = !PERFORMANCE_METRICS;

  _params.n_gpu_layers = ngl;
}

LLMEmbedding::LLMEmbedding(const std::string &modelPath) {
  if (_initialized) {
    throw std::runtime_error("LLMEmbedding already initialized");
  }
  if (LOG_DISABLE) {
    common_log_pause(common_log_main());
  }
  loadModel(modelPath);
  _initialized = true;
}

void LLMEmbedding::loadModel(const std::string &model_path) {
  initParams();
  // create an instance of llama_model
  llama_model_params model_params = llama_model_default_params();

  model_params.n_gpu_layers = _params.n_gpu_layers;

  _model = llama_model_load_from_file(model_path.data(), model_params);

  if (!_model) {
    throw std::runtime_error("load_model() failed");
  }

  // create an instance of llama_context
  llama_context_params ctx_params = llama_context_default_params();
  ctx_params.n_ctx =
      _params.n_ctx; // take context size from the model GGUF file
  ctx_params.no_perf = _params.no_perf; // disable performance metrics
  ctx_params.n_batch = _params.n_batch;
  ctx_params.embeddings = _params.embedding;
  ctx_params.rope_scaling_type = _params.rope_scaling_type;
  ctx_params.rope_freq_scale = _params.rope_freq_scale;
  ctx_params.n_ubatch = _params.n_ubatch;

  _ctx = llama_init_from_model(_model, ctx_params);

  if (!_ctx) {
    throw std::runtime_error("llama_new_context_with_model() returned null");
  }

  _vocab = llama_model_get_vocab(_model);
}

LLMEmbedding::~LLMEmbedding() {
  if (PERFORMANCE_METRICS) {
    llama_perf_context_print(_ctx);
  }
  llama_free(_ctx);
  llama_model_free(_model);
}

int LLMEmbedding::embedding(const std::string &prompt,
                            std::vector<float> &embeddings, int &n_embd,
                            int &n_prompts) {
  std::vector<std::string> prompts = splitLines(prompt, _params.embd_sep);

  // max batch size
  const uint64_t n_batch = _params.n_batch;
  GGML_ASSERT(_params.n_batch >= _params.n_ctx);

  // tokenize the prompts and trim
  std::vector<std::vector<int32_t>> inputs;
  for (const auto &prompt : prompts) {
    auto inp = common_tokenize(_ctx, prompt, true, true);
    if (inp.size() > n_batch) {
      LOG_ERR("%s: number of tokens in input line (%lld) exceeds batch size "
              "(%lld), increase batch size and re-run\n",
              __func__, (long long int)inp.size(), (long long int)n_batch);
      return -1;
    }
    inputs.push_back(inp);
  }

  // check if the last token is SEP
  // it should be automatically added by the tokenizer when
  // 'tokenizer.ggml.add_eos_token' is set to 'true'
  for (auto &inp : inputs) {
    if (inp.empty() || inp.back() != llama_vocab_sep(_vocab)) {
      LOG_WRN("%s: last token in the prompt is not SEP\n", __func__);
      LOG_WRN(
          "%s: 'tokenizer.ggml.add_eos_token' should be set to 'true' in the "
          "GGUF header\n",
          __func__);
    }
  }

  // initialize batch
  n_prompts = prompts.size();
  struct llama_batch batch = llama_batch_init(n_batch, 0, 1);

  // count number of embeddings
  int n_embd_count = 0;
  if (_params.pooling_type == LLAMA_POOLING_TYPE_NONE) {
    for (int k = 0; k < n_prompts; k++) {
      n_embd_count += inputs[k].size();
    }
  } else {
    n_embd_count = n_prompts;
  }

  // allocate output
  n_embd = llama_model_n_embd(_model);
  embeddings.resize(n_embd_count * n_embd, 0);
  float *emb = embeddings.data();

  // break into batches
  int e = 0; // number of embeddings already stored
  int s = 0; // number of prompts in current batch
  for (int k = 0; k < n_prompts; k++) {
    // clamp to n_batch tokens
    auto &inp = inputs[k];

    const uint64_t n_toks = inp.size();

    // encode if at capacity
    if (batch.n_tokens + n_toks > n_batch) {
      float *out = emb + e * n_embd;
      LLMEmbedding::batchDecode(_ctx, batch, out, s, n_embd,
                                _params.embd_normalize);
      e += _params.pooling_type == LLAMA_POOLING_TYPE_NONE ? batch.n_tokens : s;
      s = 0;
      common_batch_clear(batch);
    }

    // add to batch
    LLMEmbedding::batchAddSeq(batch, inp, s);
    s += 1;
  }

  // final batch
  float *out = emb + e * n_embd;
  LLMEmbedding::batchDecode(_ctx, batch, out, s, n_embd,
                            _params.embd_normalize);
  return 0;
}

std::vector<std::string>
LLMEmbedding::splitLines(const std::string &s, const std::string &separator) {
  std::vector<std::string> lines;
  size_t start = 0;
  size_t end = s.find(separator);

  while (end != std::string::npos) {
    lines.push_back(s.substr(start, end - start));
    start = end + separator.length();
    end = s.find(separator, start);
  }

  lines.push_back(s.substr(start)); // Add the last part
  return lines;
}

void LLMEmbedding::batchAddSeq(llama_batch &batch,
                               const std::vector<int32_t> &tokens,
                               llama_seq_id seq_id) {
  size_t n_tokens = tokens.size();
  for (size_t i = 0; i < n_tokens; i++) {
    common_batch_add(batch, tokens[i], i, {seq_id}, true);
  }
}

void LLMEmbedding::batchDecode(llama_context *ctx, llama_batch &batch,
                               float *output, int n_seq, int n_embd,
                               int embd_norm) {
  const enum llama_pooling_type pooling_type = llama_pooling_type(ctx);
  const struct llama_model *model = llama_get_model(ctx);

  // clear previous kv_cache values (irrelevant for embeddings)
  llama_kv_self_clear(ctx);

  // run model
  LOG_INF("%s: n_tokens = %d, n_seq = %d\n", __func__, batch.n_tokens, n_seq);
  if (llama_model_has_encoder(model) && !llama_model_has_decoder(model)) {
    // encoder-only model
    if (llama_encode(ctx, batch) < 0) {
      LOG_ERR("%s : failed to encode\n", __func__);
    }
  } else if (!llama_model_has_encoder(model) &&
             llama_model_has_decoder(model)) {
    // decoder-only model
    if (llama_decode(ctx, batch) < 0) {
      LOG_ERR("%s : failed to decode\n", __func__);
    }
  }

  for (int i = 0; i < batch.n_tokens; i++) {
    if (!batch.logits[i]) {
      continue;
    }

    const float *embd = nullptr;
    int embd_pos = 0;

    if (pooling_type == LLAMA_POOLING_TYPE_NONE) {
      // try to get token embeddings
      embd = llama_get_embeddings_ith(ctx, i);
      embd_pos = i;
      GGML_ASSERT(embd != NULL && "failed to get token embeddings");
    } else {
      // try to get sequence embeddings - supported only when pooling_type is
      // not NONE
      embd = llama_get_embeddings_seq(ctx, batch.seq_id[i][0]);
      embd_pos = batch.seq_id[i][0];
      GGML_ASSERT(embd != NULL && "failed to get sequence embeddings");
    }

    float *out = output + embd_pos * n_embd;
    common_embd_normalize(embd, out, n_embd, embd_norm);
  }
}

int embedding_utils(const std::string &prompt, std::vector<float> &embeddings,
                    int &n_embd, int &n_prompts) {
  common_log_pause(common_log_main());

  return LLMEmbedding::getInstance().embedding(prompt, embeddings, n_embd,
                                               n_prompts);
}

std::vector<std::vector<float>> embedding(const std::string &prompt) {
  int n_embd = 0;
  int n_prompts = 0;
  std::vector<float> embeddings;
  int ret = embedding_utils(prompt, embeddings, n_embd, n_prompts);
  if (ret != 0) {
    LOG_ERR("%s: failed to embed prompt\n", __func__);
    return std::vector<std::vector<float>>();
  }
  std::vector<std::vector<float>> out_embeddings;
  out_embeddings.resize(n_prompts, std::vector<float>(n_embd));
  for (int i = 0; i < n_prompts; i++) {
    out_embeddings[i] = std::vector<float>(
        embeddings.begin() + i * n_embd, embeddings.begin() + (i + 1) * n_embd);
  }
  return out_embeddings;
}

std::vector<float> embedding_single(const std::string &prompt) {
  // if '\n' exists in prompt, return empty vector
  if (prompt.find('\n') != std::string::npos) {
    return std::vector<float>();
  }
  auto embeddings = embedding(prompt);
  if (embeddings.empty()) {
    return std::vector<float>();
  }
  return embeddings[0];
}

std::vector<std::vector<float>> embedding_batch(const std::string &prompts) {
  return embedding(prompts);
}


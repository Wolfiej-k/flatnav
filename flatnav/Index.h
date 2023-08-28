#pragma once
#include <algorithm>
#include <cassert>
#include <cereal/access.hpp>
#include <cereal/archives/binary.hpp>
#include <cereal/cereal.hpp>
#include <cereal/types/memory.hpp>
#include <cereal/types/optional.hpp>
#include <cstring>
#include <flatnav/DistanceInterface.h>
#include <flatnav/util/ExplicitSet.h>
#include <flatnav/util/reordering.h>
#include <flatnav/util/verifysimd.h>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <quantization/BaseProductQuantization.h>
#include <queue>
#include <utility>
#include <vector>

using flatnav::quantization::ProductQuantizer;

namespace flatnav {

// dist_t: A distance function implementing DistanceInterface.
// label_t: A fixed-width data type for the label (meta-data) of each point.
template <typename dist_t, typename label_t> class Index {
public:
  typedef std::pair<float, label_t> dist_label_t;

  /**
   * @brief Construct a new Index object for approximate near neighbor search
   *
   * @param dist                A distance metric for the specific index
   * distance. Options include l2(euclidean) and inner product.
   * @param dataset_size        The maximum number of vectors that can be
   * inserted in the index.
   * @param max_edges_per_node  The maximum number of links per node.
   */
  Index(std::shared_ptr<DistanceInterface<dist_t>> dist, int dataset_size,
        int max_edges_per_node, ProductQuantizer<dist_t> *pq = nullptr)
      : _M(max_edges_per_node), _max_node_count(dataset_size),
        _cur_num_nodes(0), _distance(dist), _visited_nodes(dataset_size + 1),
        _product_quantizer(pq) {

    if (_product_quantizer && !_product_quantizer->isTrained()) {
      throw std::runtime_error("Product quantizer must be trained before use.");
    }

    _data_size_bytes = _distance->dataSize();

    if (_product_quantizer) {
      // For now, this is expected to be 8 (1 byte for each of the 8
      // subvectors)
      _data_size_bytes = _product_quantizer->getCodeSize();
    }
    _node_size_bytes =
        _data_size_bytes + (sizeof(node_id_t) * _M) + sizeof(label_t);

    size_t index_memory_size = _node_size_bytes * _max_node_count;

    _index_memory = new char[index_memory_size];
    _transformed_query = new char[_data_size_bytes];
  }

  // Constructor for serialization with cereal. Do not use outside of
  // this class.
  Index() = default;

  ~Index() {
    delete[] _index_memory;
    delete[] _transformed_query;
  }

  bool add(void *data, label_t &label, int ef_construction,
           int num_initializations = 100) {
    // initialization must happen before alloc due to a bug where
    // initializeSearch chooses new_node_id as the initialization
    // since new_node_id has distance 0 (but no links). The search is
    // skipped because the "optimal" node seems to have been found.
    node_id_t new_node_id;
    node_id_t entry_node = initializeSearch(data, num_initializations);
    // make space for the new node
    if (!allocateNode(data, label, new_node_id)) {
      return false;
    }
    // search graph for neighbors of new node, connect to them
    if (new_node_id > 0) {
      PriorityQueue neighbors = beamSearch(data, entry_node, ef_construction);
      selectNeighbors(neighbors, _M);
      connectNeighbors(neighbors, new_node_id);
    } else {
      return false;
    }
    return true;
  }

  /***
   * @brief Search the index for the k nearest neighbors of the query.
   * @param query The query vector.
   * @param K The number of nearest neighbors to return.
   * @param ef_search The search beam width.
   * @param num_initializations The number of random initializations to use.
   */
  std::vector<dist_label_t> search(const void *query, const int K,
                                   int ef_search,
                                   int num_initializations = 100) {

    // We use a pre-allocated buffer for the transformed query, for speed
    // reasons, but it would also be acceptable to manage this buffer
    // dynamically (e.g. in the multi-threaded setting).
    // _distance->transformData(_transformed_query, query);

    node_id_t entry_node = initializeSearch(query, num_initializations);
    PriorityQueue neighbors = beamSearch(/* query = */ query,
                                         /* entry_node = */ entry_node,
                                         /* buffer_size = */ ef_search);
    std::vector<dist_label_t> results;

    // std::cout << "[info] neighbors size = " << neighbors.size() << std::endl;

    while (neighbors.size() > K) {
      neighbors.pop();
    }
    while (neighbors.size() > 0) {
      results.emplace_back(neighbors.top().first,
                           *getNodeLabel(neighbors.top().second));
      // std::cout << "[results-size]: " << results.size() << std::endl;
      neighbors.pop();
    }
    // Print out the value in the results vector
    for (int i = 0; i < results.size(); i++) {
      std::cout << "[results]: (distance=" << results[i].first
                << ", label=" << results[i].second << ")" << std::endl;
    }

    std::cout << "[DEBUG]" << std::endl;
    std::sort(results.begin(), results.end(),
              [](const dist_label_t &left, const dist_label_t &right) {
                return left.first < right.first;
              });
    return results;
  }

  void reorder_gorder(const int window_size = 5) {
    std::vector<std::vector<node_id_t>> outdegree_table(_cur_num_nodes);
    for (node_id_t node = 0; node < _cur_num_nodes; node++) {
      node_id_t *links = getNodeLinks(node);
      for (int i = 0; i < _M; i++) {
        if (links[i] != node) {
          outdegree_table[node].push_back(links[i]);
        }
      }
    }
    std::vector<node_id_t> P = g_order<node_id_t>(outdegree_table, window_size);

    relabel(P);
  }

  void reorder_rcm() {
    // TODO: Remove code duplication for outdegree_table.
    std::vector<std::vector<node_id_t>> outdegree_table(_cur_num_nodes);
    for (node_id_t node = 0; node < _cur_num_nodes; node++) {
      node_id_t *links = getNodeLinks(node);
      for (int i = 0; i < _M; i++) {
        if (links[i] != node) {
          outdegree_table[node].push_back(links[i]);
        }
      }
    }
    std::vector<node_id_t> P = rcm_order<node_id_t>(outdegree_table);
    relabel(P);
  }

  static std::unique_ptr<Index<dist_t, label_t>>
  loadIndex(const std::string &filename) {
    std::ifstream stream(filename, std::ios::binary);

    if (!stream.is_open()) {
      throw std::runtime_error("Unable to open file for reading: " + filename);
    }

    cereal::BinaryInputArchive archive(stream);
    std::unique_ptr<Index<dist_t, label_t>> index =
        std::make_unique<Index<dist_t, label_t>>();

    size_t data_dimension;

    // 1. Deserialize metadata
    archive(index->_M, index->_data_size_bytes, index->_node_size_bytes,
            index->_max_node_count, index->_cur_num_nodes, data_dimension,
            index->_visited_nodes);

    // 2. Deserialize product quantizer
    // bool has_product_quantizer;
    // archive(has_product_quantizer);
    // if (has_product_quantizer) {
    //   index->_product_quantizer = new ProductQuantizer<dist_t>();
    //   archive(*index->_product_quantizer);
    // } else {
    //   index->_product_quantizer = nullptr;
    // }

    index->_product_quantizer = nullptr;

    index->_distance = std::make_shared<dist_t>(data_dimension);
    // 3. Allocate memory using deserialized metadata
    index->_index_memory =
        new char[index->_node_size_bytes * index->_max_node_count];
    index->_transformed_query = new char[index->_data_size_bytes];

    // 4. Deserialize content into allocated memory
    archive(
        cereal::binary_data(index->_index_memory,
                            index->_node_size_bytes * index->_max_node_count));
    archive(cereal::binary_data(index->_transformed_query,
                                index->_data_size_bytes));

    return index;
  }

  void saveIndex(const std::string &filename) {
    std::ofstream stream(filename, std::ios::binary);

    if (!stream.is_open()) {
      throw std::runtime_error("Unable to open file for writing: " + filename);
    }

    cereal::BinaryOutputArchive archive(stream);
    archive(*this);
  }

  inline size_t maxEdgesPerNode() const { return _M; }
  inline size_t dataSizeBytes() const { return _data_size_bytes; }

  inline size_t nodeSizeBytes() const { return _node_size_bytes; }

  inline size_t maxNodeCount() const { return _max_node_count; }

  inline char *indexMemory() const { return _index_memory; }
  inline size_t currentNumNodes() const { return _cur_num_nodes; }

  void printIndexParams() const {
    std::cout << "\nIndex Parameters" << std::endl;
    std::cout << "-----------------------------" << std::endl;
    std::cout << "max_edges_per_node (M): " << _M << std::endl;
    std::cout << "data_size_bytes: " << _data_size_bytes << std::endl;
    std::cout << "node_size_bytes: " << _node_size_bytes << std::endl;
    std::cout << "max_node_count: " << _max_node_count << std::endl;
    std::cout << "cur_num_nodes: " << _cur_num_nodes << std::endl;
    std::cout << "visited_nodes size: " << _visited_nodes.size() << std::endl;
    std::cout << "distance dimension: " << _distance->dimension() << std::endl;
    bool has_product_quantizer = _product_quantizer != nullptr;
    std::cout << "product quantizer: " << has_product_quantizer << std::endl;

    if (_product_quantizer) {
      _product_quantizer->printQuantizerParams();
    }
  }

private:
  // internal node numbering scheme
  typedef unsigned int node_id_t;
  typedef std::pair<float, node_id_t> dist_node_t;

  typedef ExplicitSet VisitedSet;

  typedef std::priority_queue<dist_node_t, std::vector<dist_node_t>>
      PriorityQueue;

  // Large (several GB), pre-allocated block of memory.
  char *_index_memory;
  char *_transformed_query;

  size_t _M;
  // size of one data point (does not support variable-size data, strings)
  size_t _data_size_bytes;
  // Node consists of: ([data] [M links] [data label]). This layout was chosen
  // after benchmarking - it's slightly more cache-efficient than others.
  size_t _node_size_bytes;
  size_t _max_node_count; // Determines size of internal pre-allocated memory
  size_t _cur_num_nodes;

  std::shared_ptr<DistanceInterface<dist_t>> _distance;

  // Remembers which nodes we've visited, to avoid re-computing distances.
  // Might be a caching problem in beamSearch - needs to be profiled.
  VisitedSet _visited_nodes;

  ProductQuantizer<dist_t> *_product_quantizer;

  friend class cereal::access;

  template <typename Archive> void serialize(Archive &archive) {
    archive(_M, _data_size_bytes, _node_size_bytes, _max_node_count,
            _cur_num_nodes, _distance->dimension(), _visited_nodes);

    // Handle serializing the product quantizer.
    // if constexpr (!Archive::is_loading::value) {
    //   // We are serializing
    //   bool has_product_quantizer = _product_quantizer != nullptr;
    //   archive(has_product_quantizer);
    //   if (has_product_quantizer) {
    //     archive(*_product_quantizer);
    //   }
    // } else {
    //   // We are deserializing.
    //   bool has_product_quantizer;
    //   archive(has_product_quantizer);
    //   if (has_product_quantizer) {
    //     _product_quantizer = new ProductQuantizer<dist_t>();
    //     archive(*_product_quantizer);
    //   } else {
    //     _product_quantizer = nullptr;
    //   }
    // }

    // Serialize the allocated memory for the index & query.
    archive(
        cereal::binary_data(_index_memory, _node_size_bytes * _max_node_count));
    archive(cereal::binary_data(_transformed_query, _data_size_bytes));
  }

  char *getNodeData(const node_id_t &n) const {
    char *location = _index_memory + (n * _node_size_bytes);
    return location;
  }

  node_id_t *getNodeLinks(const node_id_t &n) const {
    char *location = _index_memory + (n * _node_size_bytes) + _data_size_bytes;
    return reinterpret_cast<node_id_t *>(location);
  }

  label_t *getNodeLabel(const node_id_t &n) const {
    char *location = _index_memory + (n * _node_size_bytes) + _data_size_bytes +
                     (_M * sizeof(node_id_t));
    return reinterpret_cast<label_t *>(location);
  }

  // TODO: Add optional argument here for quantized data vector.
  bool allocateNode(void *data, label_t &label, node_id_t &new_node_id) {
    if (_cur_num_nodes >= _max_node_count) {
      return false;
    }
    new_node_id = _cur_num_nodes;

    if (_product_quantizer) {
      // Quantize the data vector and write the quantized vector into the
      // index at the correct location.
      uint32_t code_size = _product_quantizer->getCodeSize();
      uint8_t *code = new uint8_t[code_size]();

      _product_quantizer->computePQCode(/* vector = */ (float *)data,
                                        /* code = */ code);

      std::memcpy(getNodeData(_cur_num_nodes), code, _data_size_bytes);
      delete[] code;
    } else {
      // Transforms and writes data into the index at the correct location.
      _distance->transformData(/* destination = */ getNodeData(_cur_num_nodes),
                               /* src = */ data);
    }
    *(getNodeLabel(_cur_num_nodes)) = label;

    node_id_t *links = getNodeLinks(_cur_num_nodes);
    for (uint32_t i = 0; i < _M; i++) {
      links[i] = _cur_num_nodes;
    }

    _cur_num_nodes++;
    return true;
  }

  inline void swapNodes(node_id_t a, node_id_t b, void *temp_data,
                        node_id_t *temp_links, label_t *temp_label) {

    // stash b in temp
    std::memcpy(temp_data, getNodeData(b), _data_size_bytes);
    std::memcpy(temp_links, getNodeLinks(b), _M * sizeof(node_id_t));
    std::memcpy(temp_label, getNodeLabel(b), sizeof(label_t));

    // place node at a in b
    std::memcpy(getNodeData(b), getNodeData(a), _data_size_bytes);
    std::memcpy(getNodeLinks(b), getNodeLinks(a), _M * sizeof(node_id_t));
    std::memcpy(getNodeLabel(b), getNodeLabel(a), sizeof(label_t));

    // put node b in a
    std::memcpy(getNodeData(a), temp_data, _data_size_bytes);
    std::memcpy(getNodeLinks(a), temp_links, _M * sizeof(node_id_t));
    std::memcpy(getNodeLabel(a), temp_label, sizeof(label_t));

    return;
  }

  /**
   * @brief Performs beam search for the nearest neighbors of the query.
   * @TODO: Add `entry_node_dist` argument to this function since we expect to
   * have computed that a priori.
   *
   * @param query
   * @param entry_node
   * @param buffer_size
   * @return PriorityQueue
   */
  PriorityQueue beamSearch(const void *query, const node_id_t entry_node,
                           const int buffer_size) {

    // The query pointer should contain transformed data.
    // returns an iterable list of node_id_t's, sorted by distance (ascending)
    PriorityQueue neighbors;  // W in the HNSW paper
    PriorityQueue candidates; // C in the HNSW paper

    _visited_nodes.clear();
    float dist = std::numeric_limits<float>::max();

    if (_product_quantizer) {
      // Print out the computed code we get from calling getNodeData(entry_node)
      uint8_t* code = reinterpret_cast<uint8_t *>(getNodeData(entry_node));
      for (int i = 0; i < _product_quantizer->getCodeSize(); i++) {
        std::cout << "[info] code[" << i << "] = " << (int)code[i] << std::endl;
      }

      dist = _product_quantizer->getDistance(
          /* query_vector = */ (float *)query,
          /* code = */ reinterpret_cast<uint8_t *>(getNodeData(entry_node)));
    } else {
      dist = _distance->distance(query, getNodeData(entry_node));
    }
    

    // Print out the distance 
    // std::cout << "[info] beam-search-distance = " << dist << std::endl;

    float max_dist = dist;

    candidates.emplace(-dist, entry_node);
    neighbors.emplace(dist, entry_node);
    _visited_nodes.insert(entry_node);

    while (!candidates.empty()) {
      // get nearest element from candidates
      dist_node_t d_node = candidates.top();
      if ((-d_node.first) > max_dist) {
        break;
      }
      candidates.pop();
      node_id_t *d_node_links = getNodeLinks(d_node.second);
      for (int i = 0; i < _M; i++) {
        if (!_visited_nodes[d_node_links[i]]) {
          // If we haven't visited the node yet.
          _visited_nodes.insert(d_node_links[i]);

          if (_product_quantizer) {
            dist = _product_quantizer->getDistance(
                /* query_vector = */ (float *)query,
                /* code = */
                reinterpret_cast<uint8_t *>(getNodeData(d_node_links[i])));
          } else {
            dist = _distance->distance(query, getNodeData(d_node_links[i]));
          }

          // dist = _distance->distance(query, getNodeData(d_node_links[i]));

          // Include the node in the buffer if buffer isn't full or
          // if the node is closer than a node already in the buffer.
          if (neighbors.size() < buffer_size || dist < max_dist) {
            candidates.emplace(-dist, d_node_links[i]);
            neighbors.emplace(dist, d_node_links[i]);
            if (neighbors.size() > buffer_size) {
              neighbors.pop();
            }
            if (!neighbors.empty()) {
              max_dist = neighbors.top().first;
            }
          }
        }
      }
    }
    return neighbors;
  }

  void selectNeighbors(PriorityQueue &neighbors, const int M) {
    // selects neighbors from the PriorityQueue, according to the HNSW
    // heuristic
    if (neighbors.size() < M) {
      return;
    }

    PriorityQueue candidates;
    std::vector<dist_node_t> saved_candidates;
    saved_candidates.reserve(M);

    while (neighbors.size() > 0) {
      candidates.emplace(-neighbors.top().first, neighbors.top().second);
      neighbors.pop();
    }

    while (candidates.size() > 0) {
      if (saved_candidates.size() >= M) {
        break;
      }
      dist_node_t current_pair = candidates.top();
      candidates.pop();

      bool should_keep_candidate = true;
      for (const dist_node_t &second_pair : saved_candidates) {
        float cur_dist = std::numeric_limits<float>::max();

        if (_product_quantizer) {
          cur_dist = _product_quantizer->getSymmetricDistance(
              /* code1 = */
              reinterpret_cast<uint8_t *>(getNodeData(second_pair.second)),
              /* code2 = */
              reinterpret_cast<uint8_t *>(getNodeData(current_pair.second)));
        } else {
          cur_dist =
              _distance->distance(/* x = */ getNodeData(second_pair.second),
                                  /* y = */ getNodeData(current_pair.second));
        }
        if (cur_dist < (-current_pair.first)) {
          should_keep_candidate = false;
          break;
        }
      }
      if (should_keep_candidate) {
        // We could do neighbors.emplace except we have to iterate
        // through saved_candidates, and std::priority_queue doesn't
        // support iteration (there is no technical reason why not).
        saved_candidates.push_back(current_pair);
      }
    }
    // TODO: implement my own priority queue, get rid of vector
    // saved_candidates, add directly to neighborqueue earlier.
    for (const dist_node_t &current_pair : saved_candidates) {
      neighbors.emplace(-current_pair.first, current_pair.second);
    }
  }

  void connectNeighbors(PriorityQueue &neighbors, node_id_t new_node_id) {
    // connects neighbors according to the HSNW heuristic
    node_id_t *new_node_links = getNodeLinks(new_node_id);
    int i = 0; // iterates through links for "new_node_id"

    while (neighbors.size() > 0) {
      node_id_t neighbor_node_id = neighbors.top().second;
      // add link to the current new node
      new_node_links[i] = neighbor_node_id;
      // now do the back-connections (a little tricky)
      node_id_t *neighbor_node_links = getNodeLinks(neighbor_node_id);
      bool is_inserted = false;
      for (int j = 0; j < _M; j++) {
        if (neighbor_node_links[j] == neighbor_node_id) {
          // If there is a self-loop, replace the self-loop with
          // the desired link.
          neighbor_node_links[j] = new_node_id;
          is_inserted = true;
          break;
        }
      }
      if (!is_inserted) {
        // now, we may to replace one of the links. This will disconnect
        // the old neighbor and create a directed edge, so we have to be
        // very careful. To ensure we respect the pruning heuristic, we
        // construct a candidate set including the old links AND our new
        // one, then prune this candidate set to get the new neighbors.

        float max_dist = std::numeric_limits<float>::max();
        if (_product_quantizer) {
          max_dist = _product_quantizer->getSymmetricDistance(
              /* code1 = */
              reinterpret_cast<uint8_t *>(getNodeData(neighbor_node_id)),
              /* code2 = */
              reinterpret_cast<uint8_t *>(getNodeData(new_node_id)));
        } else {
          max_dist = _distance->distance(getNodeData(neighbor_node_id),
                                         getNodeData(new_node_id));
        }

        PriorityQueue candidates;
        candidates.emplace(max_dist, new_node_id);
        for (int j = 0; j < _M; j++) {
          if (neighbor_node_links[j] != neighbor_node_id) {
            if (_product_quantizer) {
              candidates.emplace(_product_quantizer->getSymmetricDistance(
                                     /* code1 = */
                                     reinterpret_cast<uint8_t *>(
                                         getNodeData(neighbor_node_id)),
                                     /* code2 = */
                                     reinterpret_cast<uint8_t *>(
                                         getNodeData(neighbor_node_links[j]))),
                                 neighbor_node_links[j]);
            } else {
              candidates.emplace(
                  _distance->distance(
                      /* x = */ getNodeData(neighbor_node_id),
                      /* y = */ getNodeData(neighbor_node_links[j])),
                  neighbor_node_links[j]);
            }
          }
        }
        selectNeighbors(candidates, _M);
        // connect the pruned set of candidates, including self-loops:
        int j = 0;
        while (candidates.size() > 0) { // candidates
          neighbor_node_links[j] = candidates.top().second;
          candidates.pop();
          j++;
        }
        while (j < _M) { // self-loops (unused links)
          neighbor_node_links[j] = neighbor_node_id;
          j++;
        }
      }
      // loop increments:
      i++;
      if (i >= _M) {
        i = _M;
      }
      neighbors.pop();
    }
  }

  /**
   * @brief Selects a node to use as the entry point for a new node.
   * This proceeds in a greedy fashion, by selecting the node with
   * the smallest distance to the query.
   *
   * @param query
   * @param num_initializations
   * @return node_id_t
   */
  inline node_id_t initializeSearch(const void *query,
                                    int num_initializations) {
    // select entry_node from a set of random entry point options
    assert(num_initializations != 0);

    int step_size = _cur_num_nodes / num_initializations;
    if (step_size <= 0) {
      step_size = 1;
    }

    float min_dist = std::numeric_limits<float>::max();
    node_id_t entry_node = 0;

    /**
     * @brief If we use a product quantizer, this is what will happen:
     * 1. When comparing the query to a database vector (during insertion or
     * search in HNSW), you don't directly compute a distance using their
     * original vectors or even use the PQ code of the query. Instead, for each
     * segment of the database vector, look up the distance between the query's
     * segment and the database vector's corresponding centroid using the
     * pre-computed distance table for that segment. Sum these distances across
     * all segments to get the total asymmetric distance between the query and
     * the database vector.
     *
     */
    for (node_id_t node = 0; node < _cur_num_nodes; node += step_size) {
      float dist = std::numeric_limits<float>::max();

      if (_product_quantizer) {
        char *pq_code = getNodeData(node);
        auto code_size = _product_quantizer->getCodeSize();

        dist = _product_quantizer->getDistance(
            /* query_vector = */ (float *)query,
            /* code = */ reinterpret_cast<uint8_t *>(pq_code));

      } else {
        dist = _distance->distance(query, getNodeData(node));
      }
      if (dist < min_dist) {
        min_dist = dist;
        entry_node = node;
      }
    }
    return entry_node;
  }

  void relabel(const std::vector<node_id_t> &P) {
    // 1. Rewire all of the node connections
    for (node_id_t n = 0; n < _cur_num_nodes; n++) {
      node_id_t *links = getNodeLinks(n);
      for (int m = 0; m < _M; m++) {
        links[m] = P[links[m]];
      }
    }

    // 2. Physically re-layout the nodes (in place)
    char *temp_data = new char[_data_size_bytes];
    node_id_t *temp_links = new node_id_t[_M];
    label_t *temp_label = new label_t;

    // In this context, is_visited stores which nodes have been relocated
    // (it would be equivalent to name this variable "is_relocated").
    _visited_nodes.clear();

    for (node_id_t n = 0; n < _cur_num_nodes; n++) {
      if (!_visited_nodes[n]) {

        node_id_t src = n;
        node_id_t dest = P[src];

        // swap node at src with node at dest
        swapNodes(src, dest, temp_data, temp_links, temp_label);

        // mark src as having been relocated
        _visited_nodes.insert(src);

        // recursively relocate the node from "dest"
        while (!_visited_nodes[dest]) {
          // mark node as having been relocated
          _visited_nodes.insert(dest);
          // the value of src remains the same. However, dest needs
          // to change because the node located at src was previously
          // located at dest, and must be relocated to P[dest].
          dest = P[dest];

          // swap node at src with node at dest
          swapNodes(src, dest, temp_data, temp_links, temp_label);
        }
      }
    }

    delete[] temp_data;
    delete[] temp_links;
    delete temp_label;
  }
};

} // namespace flatnav
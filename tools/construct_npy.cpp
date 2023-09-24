#include "cnpy.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <flatnav/DistanceInterface.h>
#include <flatnav/Index.h>
#include <flatnav/distances/InnerProductDistance.h>
#include <flatnav/distances/SquaredL2Distance.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <omp.h>
#include <optional>
#include <quantization/ProductQuantization.h>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using flatnav::DistanceInterface;
using flatnav::Index;
using flatnav::InnerProductDistance;
using flatnav::SquaredL2Distance;
using flatnav::quantization::ProductQuantizer;

template <typename dist_t>
void runUnquantized(float *data, flatnav::METRIC_TYPE metric_type, int N, int M,
                    int dim, int ef_construction,
                    const std::string &save_file) {

  auto distance = std::make_shared<dist_t>(dim);
  auto index = new Index<dist_t, int>(
      /* dist = */ std::move(distance), /* dataset_size = */ N,
      /* max_edges = */ M);

  auto start = std::chrono::high_resolution_clock::now();

  for (int label = 0; label < N; label++) {
    float *element = data + (dim * label);
    index->add(/* data = */ (void *)element, /* label = */ label,
               /* ef_construction */ ef_construction);
    if (label % 10000 == 0)
      std::clog << "." << std::flush;
  }
  std::clog << std::endl;

  auto stop = std::chrono::high_resolution_clock ::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
  std::clog << "Build time: " << (float)duration.count() << " milliseconds"
            << std::endl;

  std::clog << "Saving index to: " << save_file << std::endl;
  index->saveIndex(/* filename = */ save_file);

  delete index;
}

void run(float *data, flatnav::METRIC_TYPE metric_type, int N, int M, int dim,
         int ef_construction, const std::string &save_file,
         bool quantize = false) {

  if (quantize) {
    auto quantizer = std::make_shared<ProductQuantizer>(
        /* dim = */ dim, /* M = */ 5, /* nbits = */ 8,
        /* metric_type = */ metric_type);

    auto start = std::chrono::high_resolution_clock::now();
    quantizer->train(/* vectors = */ data, /* num_vectors = */ N);
    auto stop = std::chrono::high_resolution_clock::now();
    auto duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
    std::clog << "Quantization time: " << (float)duration.count()
              << " milliseconds" << std::endl;

    auto index = new Index<ProductQuantizer, int>(
        /* dist = */ std::move(quantizer), /* dataset_size = */ N,
        /* max_edges = */ M);

    start = std::chrono::high_resolution_clock::now();

    for (int label = 0; label < N; label++) {
      float *element = data + (dim * label);
      index->add(/* data = */ (void *)element, /* label = */ label,
                 /* ef_construction */ ef_construction);
      if (label % 10000 == 0)
        std::clog << "." << std::flush;
    }
    std::clog << std::endl;

    stop = std::chrono::high_resolution_clock ::now();
    duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
    std::clog << "Build time: " << (float)duration.count() << " milliseconds"
              << std::endl;

    std::clog << "Saving index to: " << save_file << std::endl;
    index->saveIndex(/* filename = */ save_file);

    delete index;
  } else {
    if (metric_type == flatnav::METRIC_TYPE::EUCLIDEAN) {
      runUnquantized<SquaredL2Distance>(data, metric_type, N, M, dim,
                                        ef_construction, save_file);
    } else if (metric_type == flatnav::METRIC_TYPE::INNER_PRODUCT) {
      runUnquantized<InnerProductDistance>(data, metric_type, N, M, dim,
                                           ef_construction, save_file);
    }
  }
}

int main(int argc, char **argv) {

  if (argc < 7) {
    std::clog << "Usage: " << std::endl;
    std::clog << "construct <quantize> <metric> <data> <M> <ef_construction> "
                 "<outfile>"
              << std::endl;
    std::clog << "\t <quantize> int, 0 for no quantization, 1 for quantization"
              << std::endl;
    std::clog << "\t <metric> int, 0 for L2, 1 for inner product (angular)"
              << std::endl;
    std::clog << "\t <data> npy file from ann-benchmarks" << std::endl;
    std::clog << "\t <M>: int " << std::endl;
    std::clog << "\t <ef_construction>: int " << std::endl;
    std::clog << "\t <outfile>: where to stash the index" << std::endl;

    return -1;
  }

  bool quantize = std::stoi(argv[1]) ? true : false;
  int metric_id = std::stoi(argv[2]);
  cnpy::NpyArray datafile = cnpy::npy_load(argv[3]);
  int M = std::stoi(argv[4]);
  int ef_construction = std::stoi(argv[5]);

  if ((datafile.shape.size() != 2)) {
    return -1;
  }

  int dim = datafile.shape[1];
  int N = datafile.shape[0];

  std::clog << "Loading " << dim << "-dimensional dataset with N = " << N
            << std::endl;
  float *data = datafile.data<float>();
  flatnav::METRIC_TYPE metric_type = metric_id == 0
                                         ? flatnav::METRIC_TYPE::EUCLIDEAN
                                         : flatnav::METRIC_TYPE::INNER_PRODUCT;

  run(/* data = */ data,
      /* metric_type = */ metric_type,
      /* N = */ N, /* M = */ M, /* dim = */ dim,
      /* ef_construction = */ ef_construction, /* save_file = */ argv[6],
      /* quantize = */ quantize);

  return 0;
}
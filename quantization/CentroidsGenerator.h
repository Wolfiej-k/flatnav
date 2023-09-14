#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <memory>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace flatnav::quantization {

class CentroidsGenerator {
public:
  CentroidsGenerator(uint32_t dim, uint32_t num_centroids,
                     uint32_t num_iterations = 5,
                     uint32_t max_points_per_centroid = 256,
                     bool normalized = true, bool verbose = false,
                     const std::string &initialization_type = "default")
      : _dim(dim), _num_centroids(num_centroids),
        _clustering_iterations(num_iterations),
        _max_points_per_centroid(max_points_per_centroid),
        _normalized(normalized), _verbose(verbose),
        _centroids_initialized(false), _seed(3333),
        _initialization_type(initialization_type) {}

  void initializeCentroids(const float *data, uint64_t n) {
    // TODO: Move hypercube initialization from the ProductQuantizer class
    // to here.
    auto initialization_type = _initialization_type;
    std::transform(initialization_type.begin(), initialization_type.end(),
                   initialization_type.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (initialization_type == "default") {
      randomInitialize(data, n);
    } else if (initialization_type == "kmeans++") {
      kmeansPlusPlusInitialize(data, n);
    } else {
      throw std::invalid_argument(
          "Invalid centroids initialization initialization type: " +
          initialization_type);
    }
    _centroids_initialized = true;
  }

  /**
   * @brief Run k-means clustering algorithm to compute D-dimensional
   * centroids given n D-dimensional vectors.

   * The algorithm proceeds as follows:
   * - Select k datapoints as the initial centroids (we use a random
   initialization for now).
   *   We can also use another strategy such as kmeans++ initialization due to
   Arthur and
   *   Vassilvitskii.
   https://theory.stanford.edu/~sergei/papers/kMeansPP-soda.pdf
   *
   * - Assign each data point to its nearest centroid based on l2 distances.
   * - Update each centroid to be the mean of its assigned datapoints
   * - Repeat steps 2 & 3 until we reach _num_iterations
   *
   * @param vectors       the input datapoints
   * @param vec_weights   weight associated with each datapoint: NULL or size n
   * @param n             The number of datapoints
   */
  void generateCentroids(const float *vectors, const float *vec_weights,
                         uint64_t n) {
    if (n < _num_centroids) {
      throw std::runtime_error(
          "Invalid configuration. The number of centroids: " +
          std::to_string(_num_centroids) +
          " is bigger than the number of data points: " + std::to_string(n));
    }

    // Initialize the centroids by randomly sampling k centroids among the n
    // data points
    if (!_centroids_initialized) {
      _centroids.resize(_num_centroids * _dim);
      initializeCentroids(/* data = */ vectors, /* n = */ n);
    }

    // Temporary array to store assigned centroids for each vector
    std::vector<uint32_t> assignment(n);

    // K-means loop
    for (uint32_t iteration = 0; iteration < _clustering_iterations;
         iteration++) {
// Step 1. Find the minimizing centroid based on l2 distance
#pragma omp parallel for
      for (uint64_t vec_index = 0; vec_index < n; vec_index++) {
        float min_distance = std::numeric_limits<float>::max();

        for (uint32_t c_index = 0; c_index < _num_centroids; c_index++) {
          float distance = 0.0;

          for (uint32_t dim_index = 0; dim_index < _dim; dim_index++) {
            auto temp = vectors[vec_index * _dim + dim_index] -
                        _centroids[c_index * _dim + dim_index];

            distance += temp * temp;
          }
          if (distance < min_distance) {
            assignment[vec_index] = c_index;
            min_distance = distance;
          }
        }
      }

      // Step 2: Update each centroid to be the mean of the assigned points
      std::vector<float> sums(_num_centroids * _dim, 0.0);
      std::vector<uint64_t> counts(_num_centroids, 0);

#pragma omp parallel for
      for (uint64_t vec_index = 0; vec_index < n; vec_index++) {
        for (uint32_t dim_index = 0; dim_index < _dim; dim_index++) {
#pragma omp atomic
          sums[assignment[vec_index] * _dim + dim_index] +=
              vectors[vec_index * _dim + dim_index];
        }
#pragma omp atomic
        counts[assignment[vec_index]]++;
      }
#pragma omp parallel for
      for (uint32_t c_index = 0; c_index < _num_centroids; c_index++) {
        for (uint32_t dim_index = 0; dim_index < _dim; dim_index++) {
          _centroids[c_index * _dim + dim_index] =
              counts[c_index]
                  ? sums[c_index * _dim + dim_index] / counts[c_index]
                  : 0;
        }
      }
    }
  }

  // NOTE: This is non-const because we need to resize the dimension of the
  // centroids from outside the class in some cases.
  inline std::vector<float> &centroids() { return _centroids; }

private:
  /**
   * @brief Initialize the centroids by randomly sampling k centroids among the
   * n data points
   * @param data  The input data points
   * @param n     The number of data points
   */
  void randomInitialize(const float *data, uint64_t n) {
    if (_centroids_initialized) {
      return;
    }
    std::vector<uint64_t> indices(n);

    std::iota(indices.begin(), indices.end(), 0);
    std::mt19937 generator(_seed);
    std::vector<uint64_t> sample_indices(_num_centroids);
    std::sample(indices.begin(), indices.end(), sample_indices.begin(),
                _num_centroids, generator);

    for (uint32_t i = 0; i < _num_centroids; i++) {
      auto sample_index = sample_indices[i];

      for (uint32_t dim_index = 0; dim_index < _dim; dim_index++) {
        _centroids[(i * _dim) + dim_index] =
            data[(sample_index * _dim) + dim_index];
      }
    }
  }

  /**
   * @brief Initialize the centroids using the kmeans++ algorithm
   * @param data  The input data points
   * @param n     The number of data points
   */
  void kmeansPlusPlusInitialize(const float *data, uint64_t n) {
    if (_centroids_initialized) {
      return;
    }
    std::mt19937 generator(_seed);
    std::uniform_int_distribution<uint64_t> distribution(0, n - 1);

    // Step 1. Select the first centroid at random
    uint64_t first_centroid_index = distribution(generator);
    for (uint32_t dim_index = 0; dim_index < _dim; dim_index++) {
      _centroids[dim_index] = data[first_centroid_index * _dim + dim_index];
    }

    std::vector<double> min_squared_distances(
        n, std::numeric_limits<double>::max());

    // Step 2. For k-1 remaining centroids
    for (uint32_t cent_idx = 1; cent_idx < _num_centroids; cent_idx++) {
      // Compute squared distances from the points to the nearest centroid
      double sum = 0.0;

      for (uint64_t i = 0; i < n; i++) {
        double min_distance = std::numeric_limits<double>::max();

        for (uint64_t c = 0; c < cent_idx; c++) {
          double distance = 0.0;
          for (uint32_t dim_index = 0; dim_index < _dim; dim_index++) {
            auto diff =
                _centroids[c * _dim + dim_index] - data[c * _dim + dim_index];
            distance += diff * diff;
          }
          if (distance < min_distance) {
            min_distance = distance;
          }
        }
        min_squared_distances[i] = min_distance;
        sum += min_distance;
      }

      // Choose the next centroid based on weighted probability
      std::uniform_real_distribution<double> distribution(0.0, sum);
      double threshold = distribution(generator);
      sum = 0.0;
      uint64_t next_centroid_index = 0;
      for (; next_centroid_index < n; next_centroid_index++) {
        sum += min_squared_distances[next_centroid_index];
        if (sum >= threshold) {
          break;
        }
      }

      // Add selected centroid the the centroids array
      for (uint32_t dim_index = 0; dim_index < _dim; dim_index++) {
        _centroids[cent_idx * _dim + dim_index] =
            data[next_centroid_index * _dim + dim_index];
      }
    }
  }

  uint32_t _dim;

  // Number of cluster centroids
  uint32_t _num_centroids;
  // Centroids. This will be an array of k * _dim floats
  // where k is the number of centroids
  std::vector<float> _centroids;

  // Number of clustering iterations
  uint32_t _clustering_iterations;
  // normalize centroids if set to true

  // limit the dataset size. If the number of datapoints
  // exceeds k * _max_points_per_centroid, we use subsampling
  uint32_t _max_points_per_centroid;
  bool _normalized;
  bool _verbose;
  bool _centroids_initialized;

  // seed for random number generator;
  int _seed;

  std::string _initialization_type;
};

} // namespace flatnav::quantization
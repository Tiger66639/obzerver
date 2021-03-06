#ifndef SELF_SIMILARITY_H
#define SELF_SIMILARITY_H

#include "opencv2/core/core.hpp"
#include "opencv2/features2d/features2d.hpp"
#include "opencv2/video/background_segm.hpp"

#include "obzerver/common_types.hpp"
#include "obzerver/circular_buffer.hpp"
#include "obzerver/benchmarker.hpp"

namespace obz
{

class SelfSimilarity {

private:
  bool debug_mode;  
  cv::Mat sim_matrix;
  std::uint64_t last_update_time;
  // For median
  std::vector<std::size_t> widths;
  std::vector<std::size_t> heights;

  StepBenchmarker& ticker;

public:
  SelfSimilarity(const std::size_t hist_len,
                 const std::uint64_t current_time,
                 const bool debug_mode = false);

  static float CalcFramesSimilarity(const cv::Mat &m1,
                                    const cv::Mat &m2,
                                    cv::Mat &buff,
                                    const unsigned int index,
                                    bool debug_mode);

  void Update(mseq_t& sequence,
              std::uint64_t current_time,
              const std::string &debug_folder = std::string());
  void Reset();

  std::size_t GetLastUpdateTime() const {return last_update_time;}
  const cv::Mat& GetSimMatrix() const;
  cv::Mat GetSimMatrixRendered() const;
  void WriteToDisk(
      const obz::mseq_t& sequence,
      const std::string &path,
      const std::string &prefix = std::string("seq")) const;
};

}  // namespace obz
#endif

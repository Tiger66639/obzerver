#include "glog/logging.h"

#include "obzerver/object_tracker.hpp"
#include "obzerver/utility.hpp"

#include "opencv2/core/core.hpp"

cv::Ptr<smc_shared_param_t> shared_data;

double ParticleObservationUpdate(long t, const particle_state_t &X)
{
  int NN = 1; // Prevent div/0
  double corr_weight = 0.0;
//  cv::Rect bb = ClampRect(
//        cv::Rect(X.bb.tl().x - 9, X.bb.tl().y - 9, 19, 19),
//        cv::Rect(
//          shared_data->crop, shared_data->crop,
//          shared_data->obs_diff.cols - shared_data->crop, shared_data->obs_diff.rows - shared_data->crop
//          )
//        );
  for (int i = -9; i < 10; i+=2) {
    for (int j = -9; j < 10; j+=2) {
      int xx = (int) round(X.bb.tl().x) - i;
      int yy = (int) round(X.bb.tl().y) - j;
      if (xx < shared_data->crop ||
          yy < shared_data->crop ||
          xx > (shared_data->obs_diff.cols - shared_data->crop) ||
          yy > (shared_data->obs_diff.rows - shared_data->crop))
      {
        continue;
      }
      NN++;
      corr_weight += shared_data->obs_diff.ptr<uchar>(yy)[xx];
    }
  }
//  double corr_weight = cv::mean(shared_data->obs_diff(bb))[0];
  corr_weight /= NN;
  return fabs(corr_weight) > 1e-12 ? log(corr_weight) : -12.0;
}

smc::particle<particle_state_t> ParticleInitialize(smc::rng *rng)
{
  particle_state_t p;
  p.bb.x = rng->Uniform(shared_data->crop, shared_data->obs_diff.cols - shared_data->crop);
  p.bb.y = rng->Uniform(shared_data->crop, shared_data->obs_diff.rows - shared_data->crop);
  p.recent_random_move = true;
  //return smc::particle<particle_state_t>(p, -log(shared_data->num_particles)); // log(1/1000)
  return smc::particle<particle_state_t>(p, ParticleObservationUpdate(0, p)); // log(1/1000)
}

void ParticleMove(long t, smc::particle<particle_state_t> &X, smc::rng *rng)
{
  particle_state_t* cv_to = X.GetValuePointer();
  if (rng->Uniform(0.0, 1.0) > shared_data->prob_random_move) {
    cv_to->recent_random_move = false;
    cv_to->bb.x += rng->Normal(0, shared_data->mm_displacement_stddev);
    cv_to->bb.y += rng->Normal(0, shared_data->mm_displacement_stddev);
  } else {
    cv_to->recent_random_move = true;
    cv_to->bb.x = rng->Uniform(shared_data->crop, shared_data->obs_diff.cols - shared_data->crop);
    cv_to->bb.y = rng->Uniform(shared_data->crop, shared_data->obs_diff.rows - shared_data->crop);
  }
  X.AddToLogWeight(ParticleObservationUpdate(t, *cv_to));

}

// We are sharing img_diff and img_sof by reference (no copy)
ObjectTracker::ObjectTracker(const std::size_t num_particles,
                             const std::size_t hist_len,
                             const unsigned short int crop,
                             const double prob_random_move,
                             const double mm_displacement_noise_stddev)
  :
  num_particles(num_particles),
  hist_len(hist_len),
  crop(crop),
  prob_random_move(prob_random_move),
  mm_displacement_noise_stddev(mm_displacement_noise_stddev),
  sampler(num_particles, SMC_HISTORY_NONE),
  moveset(ParticleInitialize, ParticleMove),
  status(TRACKING_STATUS_LOST),
  tracking_counter(0),
  num_clusters(0),
  clustering_err_threshold(100),
  object_hist(hist_len),
  ticker(StepBenchmarker::GetInstance())
{
  shared_data = new smc_shared_param_t();
  shared_data->crop = crop;
  shared_data->num_particles = num_particles;
  shared_data->prob_random_move = prob_random_move;
  shared_data->mm_displacement_stddev = mm_displacement_noise_stddev;

  sampler.SetResampleParams(SMC_RESAMPLE_SYSTEMATIC, 0.5);
  sampler.SetMoveSet(moveset);
  sampler.Initialise();

  LOG(INFO) << "Initialized object tracker with " << num_particles << " particles.";
}

ObjectTracker::~ObjectTracker() {
  LOG(INFO) << "Object tracker destroyed.";
}

bool ObjectTracker::Update(const cv::Mat &img_diff, const cv::Mat &img_sof)
{
  CV_Assert(img_diff.type() == CV_8UC1);
  //CV_Assert(img_sof.type() == CV_8UC1);

  // This is only shallow copy O(1)
  shared_data->obs_diff = img_diff;
  shared_data->obs_sof = img_sof;

  sampler.Iterate();
  ticker.tick("  [OT] Particle Filter");

  // to use this method smctc/sampler.hh needs to be pathced to
  // return the pParticles pointer
  // particle<Space>* GetParticlesPtr() {return pParticles; }
  // No copying, just a wrapper, do not mess with this matrix.
  // Use it only as a readonly
  // bb.x bb.y bb.w bb.h logweight
//  cv::Mat particles(sampler.GetNumber(),
//               sizeof(smc::particle<particle_state_t>) / sizeof(double),
//               CV_64FC1, sampler.GetParticlesPtr());
//  cv::Mat particles_pose;
//  particles.colRange(0, 2).convertTo(particles_pose, CV_32F);


  //std::vector<cv::Point2f> pts(sampler.GetNumber());
  std::vector<cv::Point2f> pts;
  for (long i = 0; i < sampler.GetNumber(); i++) {
    const particle_state_t p = sampler.GetParticleValue(i);
    if (false == p.recent_random_move) {
      //pts[i] = p.bb.tl();
      pts.push_back(p.bb.tl());
    }
  }

  LOG(INFO) << "Number of particles to consider: " << pts.size();

  if (pts.size() < 0.5 * sampler.GetNumber()) {
    LOG(WARNING) << "Number of non-random partilces are not enough for clustering ...";
    return false;
  }
  cv::Mat particles_pose(pts, false);

  ticker.tick("  [OT] Particles -> Mat");

  unsigned short int k = 0;
  double err = 1e12;
  for (k = 1; k < 5 && err > clustering_err_threshold; k++) {
      err = cv::kmeans(particles_pose, // Only on positions
                       k,
                       labels,
                       cv::TermCriteria(CV_TERMCRIT_ITER|CV_TERMCRIT_EPS, 100, 0.01)
                       , 20, cv::KMEANS_PP_CENTERS, centers);
      err = sqrt(err / (double) particles_pose.rows);
      LOG(INFO) << "Tried with: " << k << " Err:" << err;
  }
  if (err <= clustering_err_threshold) {
    num_clusters = k - 1;
    LOG(INFO) << "Particles: "<< particles_pose.rows << " Number of clusers in particles: " << num_clusters << " Centers" << centers << " err: " << err;
  } else {
    num_clusters = 0;
    LOG(WARNING) << "Clustering failed. Particles are very sparse.";
  }

  ticker.tick("  [OT] Clustering");

  if (status == TRACKING_STATUS_LOST) {
    if (num_clusters != 1) {
      LOG(INFO) << "Waiting for first dense cluser ...";
    } else {
      // All pts are condensed enough to form a bounding box
      object_hist.push_front(GenerateBoundingBox(pts, 1.0, img_diff.cols, img_diff.rows));
      status = TRACKING_STATUS_TRACKING;
      tracking_counter = 15;
    }
  } else if (status == TRACKING_STATUS_TRACKING) {
    if (tracking_counter == 0) {
      LOG(INFO) << "Lost Track";
      status = TRACKING_STATUS_LOST;
      object_hist.clear();
    } else {
      double min_dist = 1e12;
      cv::Rect bb;
      std::vector<cv::Point2f> cluster;
      int min_dist_cluster = 0;
      for (unsigned int i = 0; i < num_clusters; i++) {
          double dist = pow(centers.at<float>(i, 0) - rectCenter(object_hist.latest().bb).x, 2);
          dist += pow(centers.at<float>(i, 1) - rectCenter(object_hist.latest().bb).y, 2);
          LOG(INFO) << "Distance frome " << rectCenter(object_hist.latest().bb) << " to " << centers.at<float>(i, 1) << " , " << centers.at<float>(i, 0) << " is " << sqrt(dist) << std::endl;
          if (dist < min_dist) {
              min_dist = dist;
              min_dist_cluster = i;
          }
      }
      LOG(INFO) << "Chose cluster #" << min_dist_cluster << std::endl;
      if (num_clusters == 0) {
        LOG(INFO) << "No cluster at all. Skipping.";
        bb = GetBoundingBox();
        tracking_counter--;
      }
      if  (min_dist < 200) {
        for (unsigned int i = 0; i < pts.size(); i++) {
            if (labels.at<int>(i) == min_dist_cluster) {
                cluster.push_back(pts.at(i));
            }
        }
        LOG(INFO) << "Subset size: " << cluster.size() << std::endl;
        bb = GenerateBoundingBox(cluster, 1.0, img_diff.cols, img_diff.rows);
        tracking_counter = 15;
      } else {
        LOG(INFO) << "The closest cluster is far from current object being tracked, skipping";
        bb = GetBoundingBox();
        tracking_counter--;
      }
      cv::Rect lp_bb = GetBoundingBox();
      lp_bb.x = 0.5 * (lp_bb.x + bb.x);
      lp_bb.y = 0.5 * (lp_bb.y + bb.y);
      lp_bb.width = 0.5 * (lp_bb.width + bb.width);
      lp_bb.height = 0.5 * (lp_bb.height + bb.height);
      object_hist.push_front(TObject(lp_bb));
    }
  }

  LOG(INFO) << "Tracking Status: " << status;
  return true;
}

cv::Rect ObjectTracker::GenerateBoundingBox(const std::vector<cv::Point2f>& pts,
                                            const float alpha,
                                            const int boundary_width,
                                            const int boundary_height)
{
  cv::Scalar _c, _s;
  cv::meanStdDev(pts, _c, _s);
  float _e = alpha * std::max(_s[0], _s[1]);
  cv::Rect bb(int(_c[0] - _e), int(_c[1] - _e), 2.0 * _e, 2.0 * _e);
  return ClampRect(bb, boundary_width, boundary_height);
}

cv::Rect ObjectTracker::GetBoundingBox(unsigned int t) const {
  return object_hist.at(t).bb;
}

/* Debug */

void ObjectTracker::DrawParticles(cv::Mat &img)
{
  for (long i = 0; i < sampler.GetNumber(); i++) {
    if (!sampler.GetParticleValue(i).recent_random_move)
      cv::circle(img, sampler.GetParticleValue(i).bb.tl(), std::max(0.0, sampler.GetParticleWeight(i)), cv::Scalar(0, 0, 0), -1);
  }

  for (int i = 0; i < num_clusters; i++) {
    cv::circle(img, centers.at<cv::Point2f>(i), 10, cv::Scalar(255, 255, 255));
  }

  if (object_hist.size()) {
    cv::rectangle(img, GetBoundingBox(), cv::Scalar(127, 127, 127), 5);
  }
}
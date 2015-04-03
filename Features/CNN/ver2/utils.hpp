#ifndef UTILS_HPP
#define UTILS_HPP

#include <opencv2/opencv.hpp>
#include <boost/filesystem.hpp>
#include "caffe/caffe.hpp"
#include "Config.hpp"

using namespace std;
using namespace caffe;
using namespace cv;
namespace fs = boost::filesystem;


template<typename Dtype>
void computeFeatures(Net<Dtype>& caffe_test_net,
    const vector<Mat>& imgs,
    const string& LAYER,
    int BATCH_SIZE,
    vector<vector<Dtype>>& output,
    bool verbose = true) {
  int nImgs = imgs.size();
  int nBatches = ceil(nImgs * 1.0f / BATCH_SIZE);
  for (int batch = 0; batch < nBatches; batch++) {
    int actBatchSize = min(nImgs - batch * BATCH_SIZE, BATCH_SIZE);
    vector<Mat> imgs_b;
    if (actBatchSize >= BATCH_SIZE) {
      imgs_b.insert(imgs_b.end(), imgs.begin() + batch * BATCH_SIZE, 
          imgs.begin() + (batch + 1) * BATCH_SIZE);
    } else {
      imgs_b.insert(imgs_b.end(), imgs.begin() + batch * BATCH_SIZE, imgs.end());
      for (int j = actBatchSize; j < BATCH_SIZE; j++)
        imgs_b.push_back(imgs[0]);
    }
    vector<int> dvl(BATCH_SIZE, 0);
//    boost::dynamic_pointer_cast<caffe::MemoryDataLayer<Dtype>>(
//        caffe_test_net.layers()[0])->AddMatVector(imgs_b, dvl);
    boost::shared_ptr<MemoryDataLayer<float>> md_layer =
          boost::dynamic_pointer_cast<MemoryDataLayer<float>>(caffe_test_net.layers()[0]);
    md_layer->AddMatVector(imgs_b, dvl);
    Dtype loss = 0.0f;
    caffe_test_net.ForwardPrefilled(&loss);
    const boost::shared_ptr<Blob<Dtype>> feat = caffe_test_net.blob_by_name(LAYER);
    for (int i = 0; i < actBatchSize; i++) {
      Dtype* feat_data = feat->mutable_cpu_data() + feat->offset(i);
      output.push_back(vector<Dtype>(feat_data, feat_data + feat->count() / feat->shape()[0]));
    }
    if (verbose) {
      LOG(INFO) << "Batch " << batch << "/" << nBatches << " (" << actBatchSize << " images) done";
    }
  }
}

/**
 * Function to return list of images in a directory (searched recursively).
 * The output paths are w.r.t. the path imgsDir
 */
void genImgsList(const fs::path& imgsDir, vector<fs::path>& list) {
  if(!fs::exists(imgsDir) || !fs::is_directory(imgsDir)) return;
  vector<string> imgsExts = {".jpg", ".png", ".jpeg", ".JPEG", ".PNG", ".JPG"};

  fs::recursive_directory_iterator it(imgsDir);
  fs::recursive_directory_iterator endit;
  while(it != endit) {
    if(fs::is_regular_file(*it) && 
        find(imgsExts.begin(), imgsExts.end(), 
          it->path().extension()) != imgsExts.end())
      // write out paths but clip out the initial relative path from current dir 
      list.push_back(fs::path(it->path().relative_path().string().
            substr(imgsDir.relative_path().string().length())));
    ++it;
  }
  LOG(INFO) << "Found " << list.size() << " image file(s) in " << imgsDir;
}

/**
 * Read bbox from file in selsearch format (y1 x1 y2 x2)
 */
template<typename Dtype>
void readBBoxesSelSearch(const fs::path& fpath, vector<Rect>& output) {
  Dtype x1, x2, y1, y2;
  output.clear();
  ifstream ifs(fpath.string());
  if (!ifs.is_open()) {
    LOG(ERROR) << "Unable to open file " << fpath.string();
    return;
  }
  string line;
  while (getline(ifs, line)) {
    replace(line.begin(), line.end(), ',', ' ');
    istringstream iss(line);
    iss >> y1 >> x1 >> y2 >> x2;
    output.push_back(Rect(x1 - 1, y1 - 1, x2 - x1, y2 - y1)); 
  }
  ifs.close();
}

/**
 * Read any list of Dtype element separated by a whitespace delimitter.
 */
template<typename Dtype>
void readList(const fs::path& fpath, vector<Dtype>& output) {
  output.clear();
  Dtype el;
  ifstream ifs(fpath.string());
  while (ifs >> el) {
    output.push_back(el);
  }
  ifs.close();
}

template<typename Dtype>
void l2NormalizeFeatures(vector<vector<Dtype>>& feats) {
  #pragma omp parallel for
  for (int i = 0; i < feats.size(); i++) {
    Dtype l2norm = 0;
    for (auto el = feats[i].begin(); el != feats[i].end(); el++) {
      l2norm += (*el) * (*el);
    }
    l2norm = sqrt(l2norm);
    for (int j = 0; j < feats[i].size(); j++) {
      feats[i][j] = feats[i][j] / l2norm;
    }
  } 
}

void genSlidingWindows(const Size& I_size, vector<Rect>& bboxes) {
  bboxes.clear();
  int sliding_sz_x = max((int) (SLIDINGWIN_WINDOW_RATIO * I_size.width),
      SLIDINGWIN_MIN_SZ_X);
  int sliding_sz_y = max((int) (SLIDINGWIN_WINDOW_RATIO * I_size.height),
      SLIDINGWIN_MIN_SZ_Y);
  sliding_sz_x = sliding_sz_y = min(sliding_sz_x, sliding_sz_y);
  int sliding_stride_x = max((int) (SLIDINGWIN_STRIDE_RATIO * sliding_sz_x),
      SLIDINGWIN_MIN_STRIDE);
  int sliding_stride_y = max((int) (SLIDINGWIN_STRIDE_RATIO * sliding_sz_y),
      SLIDINGWIN_MIN_STRIDE);
  sliding_stride_x = sliding_stride_y = min(sliding_stride_x, sliding_stride_y);
  for (int x = 0; x < I_size.width - sliding_sz_x; x += sliding_stride_x) {
    for (int y = 0; y < I_size.height - sliding_sz_y; y += sliding_stride_y) {
      bboxes.push_back(Rect(x, y, sliding_sz_x, sliding_sz_y));
    }
  }
}

void pruneBboxesWithSeg(const Size& I_size, 
    const fs::path& segpath, vector<Rect>& bboxes, Mat& S) {
  // TODO (rg): speed up by using integral images
  vector<Rect> res;
  S = imread(segpath.string().c_str(), CV_LOAD_IMAGE_GRAYSCALE);
  // resize to the same size as I
  resize(S, S, I_size);
  for (int i = 0; i < bboxes.size(); i++) {
    int in = cv::sum(S(bboxes[i]))[0]; 
    int tot = bboxes[i].width * bboxes[i].height;
    if (in * 1.0f / tot < PERC_FGOVERLAP_FOR_BG) { // bg patch
      res.push_back(bboxes[i]);
    }
  }
  bboxes = res;
}

void DEBUG_storeWindows(const vector<Mat>& Is, fs::path fpath, 
    const Mat& I, const Mat& S) {
  fs::create_directories(fpath);
  imwrite(fpath.string() + "/main.jpg", I);
  imwrite(fpath.string() + "/seg.jpg", S);
  for (int i = 0; i < Is.size(); i++) {
    imwrite(fpath.string() + "/" + to_string((long long)i) + ".jpg", Is[i]);
  }
}

void poolFeatures(vector<vector<float>>& feats, const string& pooltype) {
  if (feats.size() == 0) return;
  vector<float> res(feats[0].size(), 0.0f);
  if (pooltype.compare("avg") == 0) {
    for (int i = 0; i < feats.size(); i++) {
      for (int j = 0; j < feats[i].size(); j++) {
        res[j] += feats[i][j];
      }
    }
    for (int j = 0; j < res.size(); j++) {
      res[j] /= feats.size();
    }
  } else {
    LOG(ERROR) << "Pool type " << pooltype << " not implemented yet!";
  }
  feats.clear();
  feats.push_back(res);
}

#endif


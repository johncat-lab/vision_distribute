#include "calibration_core.h"
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/calib3d.hpp>
#include <opencv2/features2d.hpp>
#include <iostream>
#include <iomanip>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

#include "object_info.h"

void ensureDir(const std::string& dir) {
#ifdef _WIN32
    _mkdir(dir.c_str());
#else
    mkdir(dir.c_str(), 0755);
#endif
}

cv::Mat frameToBGR(const uint8_t* data, int width, int height, int channels, uint32_t pixel_type) {
    cv::Mat result;
    if (channels == 3) {
        cv::Mat src(height, width, CV_8UC3, const_cast<uint8_t*>(data));
        if (pixel_type == 0x02180015) {
            result = src.clone();
        } else {
            cv::cvtColor(src, result, cv::COLOR_RGB2BGR);
        }
    } else {
        cv::Mat src(height, width, CV_8UC1, const_cast<uint8_t*>(data));
        cv::cvtColor(src, result, cv::COLOR_GRAY2BGR);
    }
    return result;
}

CalibConfig loadCalibConfig(const std::string& config_path) {
    CalibConfig cfg;
    cv::FileStorage fs(config_path, cv::FileStorage::READ);
    if (!fs.isOpened()) {
        std::cerr << "[标定] 无法打开配置文件: " << config_path << std::endl;
        return cfg;
    }
    if (!fs["camera_index"].empty()) cfg.camera_index = (int)fs["camera_index"];
    if (!fs["calib_path"].empty()) cfg.calib_path = (std::string)fs["calib_path"];
    if (!fs["save_dir"].empty()) cfg.save_dir = (std::string)fs["save_dir"];
    if (!fs["output_path"].empty()) cfg.output_path = (std::string)fs["output_path"];
    if (!fs["min_circle_area"].empty()) cfg.min_circle_area = (double)fs["min_circle_area"];
    if (!fs["max_circle_area"].empty()) cfg.max_circle_area = (double)fs["max_circle_area"];
    if (!fs["min_circularity"].empty()) cfg.min_circularity = (double)fs["min_circularity"];
    if (!fs["reproj_error_threshold"].empty()) cfg.reproj_error_threshold = (double)fs["reproj_error_threshold"];
    if (!fs["min_poses"].empty()) cfg.min_poses = (int)fs["min_poses"];
    if (!fs["min_angle_range"].empty()) cfg.min_angle_range = (double)fs["min_angle_range"];
    if (!fs["tcp_output_path"].empty()) cfg.tcp_output_path = (std::string)fs["tcp_output_path"];
    {
        cv::FileNode n = fs["roi_y_center"];
        if (!n.empty() && n.isInt() && (int)n > 0) cfg.roi_y_center = (int)n;
    }
    {
        cv::FileNode n = fs["roi_y_margin"];
        if (!n.empty() && n.isInt() && (int)n > 0) cfg.roi_y_margin = (int)n;
    }
    fs.release();
    return cfg;
}

bool saveCalibConfig(const CalibConfig& cfg, const std::string& config_path) {
    cv::FileStorage fs(config_path, cv::FileStorage::WRITE);
    if (!fs.isOpened()) {
        std::cerr << "[标定] 无法保存配置文件: " << config_path << std::endl;
        return false;
    }
    fs << "camera_index" << cfg.camera_index;
    fs << "calib_path" << cfg.calib_path;
    fs << "save_dir" << cfg.save_dir;
    fs << "output_path" << cfg.output_path;
    fs << "min_circle_area" << cfg.min_circle_area;
    fs << "max_circle_area" << cfg.max_circle_area;
    fs << "min_circularity" << cfg.min_circularity;
    fs << "reproj_error_threshold" << cfg.reproj_error_threshold;
    fs << "min_poses" << cfg.min_poses;
    fs << "min_angle_range" << cfg.min_angle_range;
    fs << "tcp_output_path" << cfg.tcp_output_path;
    if (cfg.roi_y_center >= 0) fs << "roi_y_center" << cfg.roi_y_center;
    if (cfg.roi_y_margin >= 0) fs << "roi_y_margin" << cfg.roi_y_margin;
    fs.release();
    return true;
}

std::vector<cv::Point2d> loadRobotCoords(const std::string& filepath) {
    std::vector<cv::Point2d> robot_coords;
    std::ifstream ifs(filepath);
    if (!ifs.is_open()) {
        std::cerr << "[标定] 无法打开机器人坐标文件: " << filepath << std::endl;
        return robot_coords;
    }
    double x, y;
    while (ifs >> x >> y) {
        robot_coords.push_back(cv::Point2d(x, y));
    }
    ifs.close();
    return robot_coords;
}

std::vector<FlangePose> loadFlangePoses(const std::string& filepath) {
    std::vector<FlangePose> poses;
    std::ifstream ifs(filepath);
    if (!ifs.is_open()) {
        std::cerr << "[TCP标定] 无法打开位姿文件: " << filepath << std::endl;
        return poses;
    }
    std::string line;
    while (std::getline(ifs, line)) {
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        if (line[start] == '#' || line[start] == '/') continue;

        std::istringstream iss(line);
        FlangePose pose;
        if (iss >> pose.x >> pose.y >> pose.theta) {
            poses.push_back(pose);
        }
    }
    ifs.close();
    return poses;
}

// ============================================================
// detectCircles — 多阈值扫描 + 标定板mask定位 + 3x3网格匹配
// 参考 detect_single_circle_center.py 的多阈值扫描策略
// ============================================================
std::vector<CircleInfo> detectCircles(const cv::Mat& image, const CalibConfig& cfg) {
    try {
    cv::Mat gray;
    if (image.channels() == 3) {
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = image.clone();
    }

    int h = gray.rows, w = gray.cols;
    double img_area = static_cast<double>(h) * w;

    double auto_min_a = std::max(cfg.min_circle_area, img_area * 0.000035);
    double auto_max_a = std::min(cfg.max_circle_area, img_area * 0.003);

    // ---- 1. 定位标定板 mask (参考 largest_bright_mask) ----
    cv::Mat board_mask;
    {
        cv::Mat blurred;
        cv::GaussianBlur(gray, blurred, cv::Size(9, 9), 0);
        cv::Mat bright;
        cv::threshold(blurred, bright, 0, 255, cv::THRESH_BINARY + cv::THRESH_OTSU);

        cv::Mat labels, stats, centroids;
        int n_labels = cv::connectedComponentsWithStats(bright, labels, stats, centroids, 8);

        double min_area = 0.01 * img_area;
        int best_label = -1;
        double best_area = 0;
        for (int i = 1; i < n_labels; ++i) {
            double area = stats.at<int>(i, cv::CC_STAT_AREA);
            if (area >= min_area && area > best_area) {
                best_area = area;
                best_label = i;
            }
        }

        if (best_label < 0) {
            board_mask = cv::Mat::ones(h, w, CV_8UC1) * 255;
        } else {
            board_mask = cv::Mat::zeros(h, w, CV_8UC1);
            board_mask.setTo(255, labels == best_label);

            cv::Mat close_kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(31, 31));
            cv::morphologyEx(board_mask, board_mask, cv::MORPH_CLOSE, close_kernel, cv::Point(-1, -1), 2);

            int erode_size = std::max(11, static_cast<int>(std::min(h, w) * 0.006) | 1);
            cv::Mat erode_kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(erode_size, erode_size));
            cv::erode(board_mask, board_mask, erode_kernel, cv::Point(-1, -1), 1);
        }
    }

    // ---- 2. 多阈值扫描检测暗色圆点 (参考 find_circle_candidates) ----
    struct RawCandidate {
        double cx, cy, radius, area, circularity;
        int threshold;
    };
    std::vector<RawCandidate> raw_candidates;

    cv::Mat open_kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(5, 5));

    for (int threshold = 45; threshold <= 250; threshold += 5) {
        cv::Mat dark = cv::Mat::zeros(h, w, CV_8UC1);
        for (int y = 0; y < h; ++y) {
            const uint8_t* gray_row = gray.ptr<uint8_t>(y);
            const uint8_t* mask_row = board_mask.ptr<uint8_t>(y);
            uint8_t* dark_row = dark.ptr<uint8_t>(y);
            for (int x = 0; x < w; ++x) {
                if (gray_row[x] < threshold && mask_row[x] > 0) {
                    dark_row[x] = 255;
                }
            }
        }

        cv::morphologyEx(dark, dark, cv::MORPH_OPEN, open_kernel, cv::Point(-1, -1), 1);

        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(dark, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_NONE);

        for (const auto& contour : contours) {
            double area = cv::contourArea(contour);
            if (area < auto_min_a || area > auto_max_a) continue;

            cv::Rect br = cv::boundingRect(contour);
            if (br.width < 12 || br.height < 12) continue;

            double aspect = static_cast<double>(br.width) / br.height;
            if (aspect < 0.65 || aspect > 1.45) continue;

            double perimeter = cv::arcLength(contour, true);
            if (perimeter <= 0) continue;
            double circ = 4.0 * CV_PI * area / (perimeter * perimeter);
            if (circ < 0.55) continue;

            double cx, cy, radius;
            if (contour.size() >= 5) {
                cv::RotatedRect el = cv::fitEllipse(contour);
                cx = el.center.x;
                cy = el.center.y;
                radius = 0.25 * (el.size.width + el.size.height);
            } else {
                cv::Moments m = cv::moments(contour);
                if (std::abs(m.m00) < 1e-9) continue;
                cx = m.m10 / m.m00;
                cy = m.m01 / m.m00;
                radius = std::sqrt(area / CV_PI);
            }

            raw_candidates.push_back({cx, cy, radius, area, circ, threshold});
        }
    }

    if (raw_candidates.empty()) return {};

    // ---- 3. 去重合并 (参考 merge_duplicate_candidates) ----
    std::sort(raw_candidates.begin(), raw_candidates.end(),
              [](const RawCandidate& a, const RawCandidate& b) {
                  if (a.circularity != b.circularity) return a.circularity > b.circularity;
                  return a.threshold < b.threshold;
              });

    std::vector<RawCandidate> merged_raw;
    for (auto& cand : raw_candidates) {
        bool dup = false;
        for (auto& kept : merged_raw) {
            double dist = std::sqrt(std::pow(cand.cx - kept.cx, 2) + std::pow(cand.cy - kept.cy, 2));
            if (dist <= std::max(cand.radius, kept.radius) * 0.35) {
                if (cand.circularity > kept.circularity) kept = cand;
                dup = true;
                break;
            }
        }
        if (!dup) merged_raw.push_back(cand);
    }

    std::vector<CircleInfo> candidates;
    candidates.reserve(merged_raw.size());
    for (auto& rc : merged_raw) {
        CircleInfo ci;
        ci.center = cv::Point2d(rc.cx, rc.cy);
        ci.radius = rc.radius;
        ci.area = rc.area;
        ci.circularity = rc.circularity;
        ci.index = -1;
        candidates.push_back(ci);
    }

    // ---- 4. 面积一致性过滤 ----
    if (candidates.size() > 9) {
        std::vector<double> areas;
        for (const auto& c : candidates) areas.push_back(c.area);
        std::sort(areas.begin(), areas.end());

        double best_lo = 0, best_hi = 0, best_range = 1e10;
        for (size_t lo = 0; lo < areas.size(); ++lo) {
            for (size_t hi = lo + 4; hi < areas.size(); ++hi) {
                double range = areas[hi] / areas[lo];
                if (range > 2.5) break;
                if (range < best_range) { best_range = range; best_lo = areas[lo]; best_hi = areas[hi]; }
            }
        }

        if (best_lo > 0) {
            std::vector<CircleInfo> flt;
            for (const auto& c : candidates)
                if (c.area >= best_lo * 0.9 && c.area <= best_hi * 1.1)
                    flt.push_back(c);
            if (flt.size() >= 5) candidates = flt;
        }
    }

    // ---- 5. 3x3 网格模式匹配 ----
    if (candidates.size() > 9) {
        size_t n = candidates.size();

        double ox = 1e10, oy = 1e10;
        for (const auto& ci : candidates) {
            if (ci.center.x < ox) ox = ci.center.x;
            if (ci.center.y < oy) oy = ci.center.y;
        }

        struct GridHyp { std::vector<size_t> idxs; double score; };
        std::vector<GridHyp> hyps;

        for (size_t a = 0; a < n && hyps.size() < 300; ++a) {
            for (size_t b = a + 1; b < n && hyps.size() < 300; ++b) {
                double dx = std::abs(candidates[a].center.x - candidates[b].center.x);
                double dy = std::abs(candidates[a].center.y - candidates[b].center.y);
                if (dx < 5 && dy < 5) continue;
                if (dx < 5) dx = dy * 0.8;
                if (dy < 5) dy = dx * 0.8;

                double dxs[] = {dx, dx * 0.5};
                double dys[] = {dy, dy * 0.5};
                for (double gx : dxs) {
                    for (double gy : dys) {
                        if (gx < 5 || gy < 5 || gx > 2000 || gy > 2000) continue;

                        std::vector<cv::Point2d> gps;
                        for (int r = 0; r < 3; ++r)
                            for (int c = 0; c < 3; ++c)
                                gps.push_back(cv::Point2d(ox + c * gx, oy + r * gy));

                        std::vector<size_t> matched;
                        double tot_d = 0;
                        double tol = std::max(gx, gy) * 0.40;

                        for (const auto& gp : gps) {
                            double bd = 1e10;
                            size_t bk = 0;
                            for (size_t k = 0; k < n; ++k) {
                                bool used = false;
                                for (auto m : matched) if (m == k) { used = true; break; }
                                if (used) continue;
                                double d = std::sqrt(std::pow(candidates[k].center.x - gp.x, 2) +
                                                     std::pow(candidates[k].center.y - gp.y, 2));
                                if (d < bd) { bd = d; bk = k; }
                            }
                            if (bd < tol) { matched.push_back(bk); tot_d += bd; }
                        }

                        if (matched.size() >= 8) {
                            GridHyp gh; gh.idxs = matched; gh.score = -tot_d;
                            hyps.push_back(gh);
                        }
                    }
                }
            }
        }

        if (!hyps.empty()) {
            std::sort(hyps.begin(), hyps.end(),
                      [](const GridHyp& a, const GridHyp& b) { return a.score > b.score; });

            std::vector<size_t> best;
            for (size_t hi = 0; hi < std::min(hyps.size(), size_t(3)); ++hi) {
                for (auto idx : hyps[hi].idxs) {
                    bool has = false;
                    for (auto bi : best) if (bi == idx) { has = true; break; }
                    if (!has) best.push_back(idx);
                }
                if (best.size() >= 9) break;
            }

            std::vector<CircleInfo> matched;
            for (auto idx : best)
                if (idx < n && matched.size() < 9) matched.push_back(candidates[idx]);
            if (matched.size() >= 8) candidates = matched;
        }
    }

    // ---- 6. 亚像素圆心精化 ----
    for (auto& ci : candidates) {
        cv::Point2d refined = refineCircleCenter(gray, ci.center, ci.radius,
                                                   std::vector<cv::Point>());
        ci.center = refined;
    }

    // ---- 7. 3x3 网格排序 ----
    if (candidates.empty()) return candidates;

    std::sort(candidates.begin(), candidates.end(),
              [](const CircleInfo& a, const CircleInfo& b) {
                  return a.center.y < b.center.y;
              });

    double row_gap = 10.0;
    {
        std::vector<double> diffs;
        for (size_t i = 1; i < candidates.size(); ++i)
            diffs.push_back(candidates[i].center.y - candidates[i - 1].center.y);

        if (diffs.size() >= 3) {
            std::sort(diffs.begin(), diffs.end());
            double max_gap = 0, gap_mid = 0;
            for (size_t i = 1; i < diffs.size(); ++i) {
                double g = diffs[i] - diffs[i - 1];
                if (g > max_gap) { max_gap = g; gap_mid = (diffs[i] + diffs[i - 1]) * 0.5; }
            }
            row_gap = (max_gap > 0) ? gap_mid : diffs[diffs.size() / 2] * 2.0;
        } else if (!diffs.empty()) {
            row_gap = diffs[diffs.size() / 2] * 2.0;
        }
        if (row_gap < 1.0) row_gap = 1.0;
    }

    std::vector<std::vector<CircleInfo>> rows;
    std::vector<CircleInfo> cur_row;
    if (!candidates.empty()) cur_row.push_back(candidates[0]);
    for (size_t i = 1; i < candidates.size(); ++i) {
        if (std::abs(candidates[i].center.y - cur_row.back().center.y) > row_gap) {
            rows.push_back(cur_row);
            cur_row.clear();
        }
        cur_row.push_back(candidates[i]);
    }
    rows.push_back(cur_row);

    for (auto& row : rows) {
        std::sort(row.begin(), row.end(),
                  [](const CircleInfo& a, const CircleInfo& b) {
                      return a.center.x < b.center.x;
                  });
    }

    std::vector<CircleInfo> sorted;
    int idx = 0;
    for (auto& row : rows) {
        for (auto& ci : row) {
            ci.index = idx++;
            sorted.push_back(ci);
        }
    }

    return sorted;
    } catch (const cv::Exception& e) {
        std::cerr << "detectCircles OpenCV exception: " << e.what() << std::endl;
        return {};
    } catch (const std::exception& e) {
        std::cerr << "detectCircles exception: " << e.what() << std::endl;
        return {};
    } catch (...) {
        std::cerr << "detectCircles unknown exception" << std::endl;
        return {};
    }
}

// ============================================================
// 4点标定 — 参考 detect_four_circle_centers.py
// ============================================================

static cv::Rect expandROI(int x, int y, int roi_w, int roi_h, int img_h, int img_w, double scale = 0.08) {
    int margin = static_cast<int>(std::round(std::max(roi_w, roi_h) * scale));
    int x0 = std::max(0, x - margin);
    int y0 = std::max(0, y - margin);
    int x1 = std::min(img_w, x + roi_w + margin);
    int y1 = std::min(img_h, y + roi_h + margin);
    return cv::Rect(x0, y0, x1 - x0, y1 - y0);
}

static std::vector<CircleInfo> findDotCandidatesInROI(const cv::Mat& gray, const cv::Mat& board_mask, const cv::Rect& roi) {
    int h = gray.rows, w = gray.cols;
    double img_area = static_cast<double>(h) * w;

    cv::Mat search_mask = board_mask.clone();
    if (cv::countNonZero(search_mask) == 0) {
        search_mask = cv::Mat::ones(h, w, CV_8UC1) * 255;
    }

    double auto_min_a = std::max(150.0, img_area * 0.000025);
    double auto_max_a = img_area * 0.004;

    struct RawCand { double cx, cy, radius, area, circularity; int threshold; };
    std::vector<RawCand> raw_candidates;

    cv::Mat open_kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(5, 5));

    for (int threshold = 45; threshold <= 250; threshold += 5) {
        cv::Mat dark = cv::Mat::zeros(h, w, CV_8UC1);
        cv::Mat bright = cv::Mat::zeros(h, w, CV_8UC1);
        int bright_thresh = 255 - threshold;
        for (int y = 0; y < h; ++y) {
            const uint8_t* gray_row = gray.ptr<uint8_t>(y);
            const uint8_t* mask_row = search_mask.ptr<uint8_t>(y);
            uint8_t* dark_row = dark.ptr<uint8_t>(y);
            uint8_t* bright_row = bright.ptr<uint8_t>(y);
            for (int x = 0; x < w; ++x) {
                if (mask_row[x] > 0) {
                    if (gray_row[x] < threshold) dark_row[x] = 255;
                    if (gray_row[x] > bright_thresh) bright_row[x] = 255;
                }
            }
        }

        cv::morphologyEx(dark, dark, cv::MORPH_OPEN, open_kernel, cv::Point(-1, -1), 1);
        cv::morphologyEx(bright, bright, cv::MORPH_OPEN, open_kernel, cv::Point(-1, -1), 1);

        std::vector<std::vector<cv::Point>> dark_contours;
        cv::findContours(dark, dark_contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_NONE);
        std::vector<std::vector<cv::Point>> bright_contours;
        cv::findContours(bright, bright_contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_NONE);

        auto process_contours = [&](const std::vector<std::vector<cv::Point>>& contours, int thresh) {
            for (const auto& contour : contours) {
                double area = cv::contourArea(contour);
                if (area < auto_min_a || area > auto_max_a) continue;

                cv::Rect br = cv::boundingRect(contour);
                if (br.width < 12 || br.height < 12) continue;

                double aspect = static_cast<double>(br.width) / br.height;
                if (aspect < 0.65 || aspect > 1.45) continue;

                double perimeter = cv::arcLength(contour, true);
                if (perimeter <= 0) continue;
                double circ = 4.0 * CV_PI * area / (perimeter * perimeter);
                if (circ < 0.55) continue;

                double cx, cy, radius;
                if (contour.size() >= 5) {
                    cv::RotatedRect el = cv::fitEllipse(contour);
                    cx = el.center.x;
                    cy = el.center.y;
                    radius = 0.25 * (el.size.width + el.size.height);
                } else {
                    cv::Moments m = cv::moments(contour);
                    if (std::abs(m.m00) < 1e-9) continue;
                    cx = m.m10 / m.m00;
                    cy = m.m01 / m.m00;
                    radius = std::sqrt(area / CV_PI);
                }

                raw_candidates.push_back({cx, cy, radius, area, circ, thresh});
            }
        };

        process_contours(dark_contours, threshold);
        process_contours(bright_contours, threshold);
    }

    if (raw_candidates.empty()) return {};

    std::sort(raw_candidates.begin(), raw_candidates.end(),
              [](const RawCand& a, const RawCand& b) {
                  if (a.circularity != b.circularity) return a.circularity > b.circularity;
                  return a.threshold < b.threshold;
              });

    std::vector<RawCand> merged_raw;
    for (auto& cand : raw_candidates) {
        bool dup = false;
        for (auto& kept : merged_raw) {
            double dist = std::sqrt(std::pow(cand.cx - kept.cx, 2) + std::pow(cand.cy - kept.cy, 2));
            if (dist <= std::max(cand.radius, kept.radius) * 0.35) {
                if (cand.circularity > kept.circularity) kept = cand;
                dup = true;
                break;
            }
        }
        if (!dup) merged_raw.push_back(cand);
    }

    std::vector<CircleInfo> candidates;
    candidates.reserve(merged_raw.size());
    for (auto& rc : merged_raw) {
        CircleInfo ci;
        ci.center = cv::Point2d(rc.cx, rc.cy);
        ci.radius = rc.radius;
        ci.area = rc.area;
        ci.circularity = rc.circularity;
        ci.index = -1;
        candidates.push_back(ci);
    }
    return candidates;
}

cv::Rect findCalibrationBoardROI(const cv::Mat& gray, int roi_y_center, int roi_y_margin) {
    int h = gray.rows, w = gray.cols;
    double img_area = static_cast<double>(h) * w;
    int min_side = std::min(h, w);

    int y_lo = 0, y_hi = h;
    if (roi_y_center >= 0 && roi_y_margin > 0) {
        y_lo = std::max(0, roi_y_center - roi_y_margin);
        y_hi = std::min(h, roi_y_center + roi_y_margin);
    }

    struct ROIEntry { cv::Rect roi; double score; int threshold; int candidate_count; };
    std::vector<ROIEntry> valid_rois;

    int thresholds[] = {235, 220, 200, 180};

    for (int threshold : thresholds) {
        cv::Mat blurred;
        cv::GaussianBlur(gray, blurred, cv::Size(7, 7), 0);
        cv::Mat bright;
        cv::threshold(blurred, bright, threshold, 255, cv::THRESH_BINARY);

        cv::Mat close_kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(15, 15));
        cv::morphologyEx(bright, bright, cv::MORPH_CLOSE, close_kernel, cv::Point(-1, -1), 1);

        cv::Mat labels, stats, centroids;
        int n_labels = cv::connectedComponentsWithStats(bright, labels, stats, centroids, 8);

        for (int i = 1; i < n_labels; ++i) {
            int rx = stats.at<int>(i, cv::CC_STAT_LEFT);
            int ry = stats.at<int>(i, cv::CC_STAT_TOP);
            int rw = stats.at<int>(i, cv::CC_STAT_WIDTH);
            int rh = stats.at<int>(i, cv::CC_STAT_HEIGHT);
            double area = stats.at<int>(i, cv::CC_STAT_AREA);

            if (area < img_area * 0.002) continue;
            if (std::max(rw, rh) > min_side * 0.45) continue;
            if (std::min(rw, rh) < min_side * 0.06) continue;

            double aspect = static_cast<double>(rw) / rh;
            if (aspect < 0.55 || aspect > 1.85) continue;

            double fill_ratio = area / static_cast<double>(rw * rh);
            if (fill_ratio < 0.35) continue;

            int roi_cy = ry + rh / 2;
            if (roi_cy < y_lo || roi_cy > y_hi) continue;

            cv::Rect roi = expandROI(rx, ry, rw, rh, h, w);

            try {
                cv::Mat roi_mask = cv::Mat::zeros(h, w, CV_8UC1);
                roi_mask(roi).setTo(255);

                cv::Mat board_bright;
                cv::threshold(gray, board_bright, 150, 255, cv::THRESH_BINARY);
                cv::Mat close_k2 = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(31, 31));
                cv::morphologyEx(board_bright, board_bright, cv::MORPH_CLOSE, close_k2, cv::Point(-1, -1), 2);

                cv::Mat combined_mask;
                cv::bitwise_and(roi_mask, board_bright, combined_mask);

                if (cv::countNonZero(combined_mask) < h * w * 0.001) {
                    combined_mask = roi_mask;
                }

                auto candidates = findDotCandidatesInROI(gray, combined_mask, roi);
                auto ordered = chooseFourPoints(candidates);

                double mean_circularity = 0;
                double mean_radius = 0;
                for (const auto& ci : ordered) {
                    mean_circularity += ci.circularity;
                    mean_radius += ci.radius;
                }
                mean_circularity /= ordered.size();
                mean_radius /= ordered.size();

                double radius_cv = 0;
                for (const auto& ci : ordered) {
                    radius_cv += std::pow(ci.radius - mean_radius, 2);
                }
                radius_cv = std::sqrt(radius_cv / ordered.size()) / std::max(mean_radius, 1.0);

                double fit_error = rectangleFitError(ordered);

                double spacing = 0;
                {
                    std::vector<double> dists;
                    dists.push_back(std::sqrt(std::pow(ordered[0].center.x - ordered[1].center.x, 2) +
                                              std::pow(ordered[0].center.y - ordered[1].center.y, 2)));
                    dists.push_back(std::sqrt(std::pow(ordered[2].center.x - ordered[3].center.x, 2) +
                                              std::pow(ordered[2].center.y - ordered[3].center.y, 2)));
                    dists.push_back(std::sqrt(std::pow(ordered[0].center.x - ordered[2].center.x, 2) +
                                              std::pow(ordered[0].center.y - ordered[2].center.y, 2)));
                    dists.push_back(std::sqrt(std::pow(ordered[1].center.x - ordered[3].center.x, 2) +
                                              std::pow(ordered[1].center.y - ordered[3].center.y, 2)));
                    std::sort(dists.begin(), dists.end());
                    spacing = (dists[1] + dists[2]) / 2.0;
                }

                double normalized_fit = fit_error / std::max(spacing, 1.0);
                double score = normalized_fit * 6.0 + radius_cv * 20.0 - mean_circularity * 2.0
                             - std::min(mean_radius / 50.0, 1.0);

                valid_rois.push_back({roi, score, threshold, static_cast<int>(candidates.size())});
            } catch (...) {
                continue;
            }
        }
    }

    if (valid_rois.empty()) {
        return cv::Rect(0, 0, w, h);
    }

    std::sort(valid_rois.begin(), valid_rois.end(),
              [](const ROIEntry& a, const ROIEntry& b) { return a.score < b.score; });

    return valid_rois[0].roi;
}

double rectangleFitError(const std::vector<CircleInfo>& four_points) {
    if (four_points.size() < 4) return 1e10;

    const auto& tl = four_points[0];
    const auto& tr = four_points[1];
    const auto& bl = four_points[2];
    const auto& br = four_points[3];

    double top_width = std::sqrt(std::pow(tl.center.x - tr.center.x, 2) +
                                  std::pow(tl.center.y - tr.center.y, 2));
    double bottom_width = std::sqrt(std::pow(bl.center.x - br.center.x, 2) +
                                     std::pow(bl.center.y - br.center.y, 2));
    double left_height = std::sqrt(std::pow(tl.center.x - bl.center.x, 2) +
                                    std::pow(tl.center.y - bl.center.y, 2));
    double right_height = std::sqrt(std::pow(tr.center.x - br.center.x, 2) +
                                     std::pow(tr.center.y - br.center.y, 2));
    double diag1 = std::sqrt(std::pow(tl.center.x - br.center.x, 2) +
                              std::pow(tl.center.y - br.center.y, 2));
    double diag2 = std::sqrt(std::pow(tr.center.x - bl.center.x, 2) +
                              std::pow(tr.center.y - bl.center.y, 2));

    return std::abs(top_width - bottom_width) + std::abs(left_height - right_height) + std::abs(diag1 - diag2);
}

std::vector<CircleInfo> chooseFourPoints(std::vector<CircleInfo>& candidates) {
    if (candidates.size() < 4) {
        throw std::runtime_error("Not enough candidates to choose 4 points");
    }

    const int max_candidates = 18;
    if (static_cast<int>(candidates.size()) > max_candidates) {
        double median_area = 0;
        {
            std::vector<double> areas;
            for (const auto& c : candidates) areas.push_back(c.area);
            std::sort(areas.begin(), areas.end());
            median_area = areas[areas.size() / 2];
        }

        std::sort(candidates.begin(), candidates.end(),
                  [&median_area](const CircleInfo& a, const CircleInfo& b) {
                      double da = std::abs(a.area - median_area) / std::max(median_area, 1.0);
                      double db = std::abs(b.area - median_area) / std::max(median_area, 1.0);
                      if (da != db) return da < db;
                      if (a.circularity != b.circularity) return a.circularity > b.circularity;
                      return false;
                  });
        candidates.resize(max_candidates);
    }

    size_t n = candidates.size();
    double best_score = std::numeric_limits<double>::max();
    std::vector<size_t> best_combo;

    for (size_t i0 = 0; i0 < n; ++i0) {
        for (size_t i1 = i0 + 1; i1 < n; ++i1) {
            for (size_t i2 = i1 + 1; i2 < n; ++i2) {
                for (size_t i3 = i2 + 1; i3 < n; ++i3) {
                    std::vector<CircleInfo> combo = {
                        candidates[i0], candidates[i1], candidates[i2], candidates[i3]
                    };

                    std::sort(combo.begin(), combo.end(),
                              [](const CircleInfo& a, const CircleInfo& b) {
                                  return a.center.y < b.center.y;
                              });

                    std::vector<CircleInfo> top(combo.begin(), combo.begin() + 2);
                    std::vector<CircleInfo> bottom(combo.begin() + 2, combo.end());

                    std::sort(top.begin(), top.end(),
                              [](const CircleInfo& a, const CircleInfo& b) {
                                  return a.center.x < b.center.x;
                              });
                    std::sort(bottom.begin(), bottom.end(),
                              [](const CircleInfo& a, const CircleInfo& b) {
                                  return a.center.x < b.center.x;
                              });

                    std::vector<CircleInfo> ordered = {top[0], top[1], bottom[0], bottom[1]};

                    double mean_circularity = 0;
                    for (const auto& ci : ordered) mean_circularity += ci.circularity;
                    mean_circularity /= 4.0;

                    double fit_error = rectangleFitError(ordered);

                    double score = fit_error - mean_circularity * 80.0;

                    if (score < best_score) {
                        best_score = score;
                        best_combo = {i0, i1, i2, i3};
                    }
                }
            }
        }
    }

    if (best_combo.empty()) {
        throw std::runtime_error("Found candidates, but none form a 2x2 target");
    }

    std::vector<CircleInfo> result = {
        candidates[best_combo[0]], candidates[best_combo[1]],
        candidates[best_combo[2]], candidates[best_combo[3]]
    };

    std::sort(result.begin(), result.end(),
              [](const CircleInfo& a, const CircleInfo& b) {
                  return a.center.y < b.center.y;
              });

    std::vector<CircleInfo> top(result.begin(), result.begin() + 2);
    std::vector<CircleInfo> bottom(result.begin() + 2, result.end());

    std::sort(top.begin(), top.end(),
              [](const CircleInfo& a, const CircleInfo& b) {
                  return a.center.x < b.center.x;
              });
    std::sort(bottom.begin(), bottom.end(),
              [](const CircleInfo& a, const CircleInfo& b) {
                  return a.center.x < b.center.x;
              });

    result = {top[0], top[1], bottom[0], bottom[1]};

    result[0].index = 0;
    result[1].index = 1;
    result[2].index = 2;
    result[3].index = 3;

    return result;
}

std::vector<CircleInfo> refineFourCirclesByTemplate(const cv::Mat& gray,
                                                     const cv::Mat& search_mask,
                                                     const std::vector<CircleInfo>& rough_circles,
                                                     double match_threshold) {
    std::vector<CircleInfo> result;
    if (rough_circles.size() < 4 || gray.empty()) return result;

    int best_idx = 0;
    double best_circ = rough_circles[0].circularity;
    for (size_t i = 1; i < rough_circles.size(); ++i) {
        if (rough_circles[i].circularity > best_circ) {
            best_circ = rough_circles[i].circularity;
            best_idx = static_cast<int>(i);
        }
    }

    const auto& ref_circle = rough_circles[best_idx];
    double radius = ref_circle.radius;
    int half_size = static_cast<int>(radius * 1.3);
    int cx = cvRound(ref_circle.center.x);
    int cy = cvRound(ref_circle.center.y);

    int x0 = std::max(0, cx - half_size);
    int y0 = std::max(0, cy - half_size);
    int x1 = std::min(gray.cols - 1, cx + half_size);
    int y1 = std::min(gray.rows - 1, cy + half_size);

    if (x1 <= x0 || y1 <= y0) return result;

    cv::Rect tmpl_rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    cv::Mat tmpl = gray(tmpl_rect).clone();

    cv::Mat tmpl_mask = cv::Mat::zeros(tmpl.rows, tmpl.cols, CV_8UC1);
    cv::Point mask_center(cx - x0, cy - y0);
    int mask_radius = static_cast<int>(radius + 2);
    cv::circle(tmpl_mask, mask_center, mask_radius, cv::Scalar(255), -1);

    cv::Scalar fg_mean = cv::mean(tmpl, tmpl_mask);
    tmpl.setTo(fg_mean, ~tmpl_mask);

    if (tmpl.cols > gray.cols || tmpl.rows > gray.rows) return result;

    cv::Mat match_result;
    cv::matchTemplate(gray, tmpl, match_result, cv::TM_CCOEFF_NORMED);

    if (!search_mask.empty() && search_mask.size() == gray.size()) {
        cv::Mat mask_result;
        cv::matchTemplate(search_mask, tmpl_mask, mask_result, cv::TM_CCORR);
        double mask_thresh = (tmpl_mask.rows * tmpl_mask.cols) * 255.0 * 0.3;
        for (int y = 0; y < match_result.rows; ++y) {
            for (int x = 0; x < match_result.cols; ++x) {
                if (mask_result.at<float>(y, x) < static_cast<float>(mask_thresh)) {
                    match_result.at<float>(y, x) = 0.0f;
                }
            }
        }
    }

    double min_dist = radius * 2.5;
    std::vector<CircleInfo> matched;

    while (matched.size() < 10) {
        double max_val;
        cv::Point max_loc;
        cv::minMaxLoc(match_result, nullptr, &max_val, nullptr, &max_loc);

        if (max_val < match_threshold) break;

        double sub_x = max_loc.x;
        double sub_y = max_loc.y;

        if (max_loc.x > 0 && max_loc.x < match_result.cols - 1) {
            float left  = match_result.at<float>(max_loc.y, max_loc.x - 1);
            float mid   = match_result.at<float>(max_loc.y, max_loc.x);
            float right = match_result.at<float>(max_loc.y, max_loc.x + 1);
            double denom = 2.0 * (2.0 * mid - left - right);
            if (std::abs(denom) > 1e-10) {
                double offset = (left - right) / denom;
                offset = std::max(-0.5, std::min(0.5, offset));
                sub_x = max_loc.x + offset;
            }
        }

        if (max_loc.y > 0 && max_loc.y < match_result.rows - 1) {
            float top   = match_result.at<float>(max_loc.y - 1, max_loc.x);
            float mid   = match_result.at<float>(max_loc.y, max_loc.x);
            float bot   = match_result.at<float>(max_loc.y + 1, max_loc.x);
            double denom = 2.0 * (2.0 * mid - top - bot);
            if (std::abs(denom) > 1e-10) {
                double offset = (top - bot) / denom;
                offset = std::max(-0.5, std::min(0.5, offset));
                sub_y = max_loc.y + offset;
            }
        }

        cv::Point2d center(sub_x + tmpl.cols / 2.0,
                           sub_y + tmpl.rows / 2.0);

        CircleInfo ci;
        ci.center = center;
        ci.radius = radius;
        ci.area = CV_PI * radius * radius;
        ci.circularity = max_val;
        ci.index = -1;
        matched.push_back(ci);

        int suppress_x0 = std::max(0, max_loc.x - static_cast<int>(min_dist));
        int suppress_y0 = std::max(0, max_loc.y - static_cast<int>(min_dist));
        int suppress_x1 = std::min(match_result.cols - 1, max_loc.x + static_cast<int>(min_dist));
        int suppress_y1 = std::min(match_result.rows - 1, max_loc.y + static_cast<int>(min_dist));

        for (int sy = suppress_y0; sy <= suppress_y1; ++sy) {
            for (int sx = suppress_x0; sx <= suppress_x1; ++sx) {
                match_result.at<float>(sy, sx) = 0.0f;
            }
        }
    }

    if (matched.size() < 4) return result;

    try {
        result = chooseFourPoints(matched);
    } catch (...) {
        std::sort(matched.begin(), matched.end(),
                  [](const CircleInfo& a, const CircleInfo& b) {
                      return a.circularity > b.circularity;
                  });
        if (matched.size() >= 4) {
            result.assign(matched.begin(), matched.begin() + 4);
        }
    }

    std::cerr << "[模板精化] 粗定位→模板匹配: 找到 " << matched.size()
              << " 个匹配, 选择 " << result.size() << " 个" << std::endl;

    return result;
}

std::vector<CircleInfo> detectFourCircles(const cv::Mat& image, const CalibConfig& cfg) {
    try {
    cv::Mat gray;
    if (image.channels() == 3) {
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = image.clone();
    }

    int h = gray.rows, w = gray.cols;

    cv::Rect roi = findCalibrationBoardROI(gray, cfg.roi_y_center, cfg.roi_y_margin);

    cv::Mat search_mask;
    {
        cv::Mat board_bright;
        cv::threshold(gray, board_bright, 150, 255, cv::THRESH_BINARY);
        cv::Mat close_kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(31, 31));
        cv::morphologyEx(board_bright, board_bright, cv::MORPH_CLOSE, close_kernel, cv::Point(-1, -1), 2);

        search_mask = cv::Mat::zeros(h, w, CV_8UC1);
        search_mask(roi).setTo(255);
        cv::bitwise_and(search_mask, board_bright, search_mask);

        if (cfg.roi_y_center >= 0 && cfg.roi_y_margin > 0) {
            int y_lo = std::max(0, cfg.roi_y_center - cfg.roi_y_margin);
            int y_hi = std::min(h, cfg.roi_y_center + cfg.roi_y_margin);
            cv::Mat y_mask = cv::Mat::zeros(h, w, CV_8UC1);
            y_mask(cv::Rect(0, y_lo, w, y_hi - y_lo)).setTo(255);
            cv::bitwise_and(search_mask, y_mask, search_mask);
        }

        int white_count = cv::countNonZero(search_mask);
        if (white_count < h * w * 0.001) {
            search_mask = cv::Mat::ones(h, w, CV_8UC1) * 255;
        }
    }

    auto candidates = findDotCandidatesInROI(gray, search_mask, roi);

    auto result = chooseFourPoints(candidates);

    auto refined_by_template = refineFourCirclesByTemplate(gray, search_mask, result, 0.5);
    if (refined_by_template.size() == 4) {
        for (size_t i = 0; i < result.size(); ++i) {
            double best_dist = 1e9;
            cv::Point2d best_center = result[i].center;
            for (size_t j = 0; j < refined_by_template.size(); ++j) {
                double d = std::sqrt(std::pow(result[i].center.x - refined_by_template[j].center.x, 2) +
                                     std::pow(result[i].center.y - refined_by_template[j].center.y, 2));
                if (d < best_dist) {
                    best_dist = d;
                    best_center = refined_by_template[j].center;
                }
            }
            if (best_dist < result[i].radius * 0.5) {
                result[i].center = best_center;
            }
        }
    }

    for (auto& ci : result) {
        cv::Point2d refined = refineCircleCenter(gray, ci.center, ci.radius,
                                                   std::vector<cv::Point>());
        ci.center = refined;
    }

    return result;
    } catch (const cv::Exception& e) {
        std::cerr << "detectFourCircles OpenCV exception: " << e.what() << std::endl;
        return {};
    } catch (const std::exception& e) {
        std::cerr << "detectFourCircles exception: " << e.what() << std::endl;
        return {};
    } catch (...) {
        std::cerr << "detectFourCircles unknown exception" << std::endl;
        return {};
    }
}

cv::Point2d refineCrossCenterByHough(const cv::Mat& gray,
                                       const cv::Point2d& rough_center,
                                       double radius) {
    int roi_half = std::max(8, static_cast<int>(radius * 0.8));
    int cx = cvRound(rough_center.x);
    int cy = cvRound(rough_center.y);

    int x0 = std::max(0, cx - roi_half);
    int y0 = std::max(0, cy - roi_half);
    int x1 = std::min(gray.cols - 1, cx + roi_half);
    int y1 = std::min(gray.rows - 1, cy + roi_half);

    if (x1 <= x0 || y1 <= y0) return rough_center;

    cv::Rect roi_rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    cv::Mat roi = gray(roi_rect);

    cv::Mat blurred;
    cv::GaussianBlur(roi, blurred, cv::Size(5, 5), 1.0);

    double otsu_thresh = 0;
    cv::threshold(blurred, cv::Mat(), otsu_thresh, 255, cv::THRESH_OTSU);
    if (otsu_thresh < 10) otsu_thresh = 50;

    cv::Mat edges;
    cv::Canny(blurred, edges, otsu_thresh * 0.4, otsu_thresh * 0.8);

    cv::Mat circle_mask = cv::Mat::zeros(roi.rows, roi.cols, CV_8UC1);
    cv::Point mask_center(cx - x0, cy - y0);
    int mask_radius = static_cast<int>(radius * 0.75);
    cv::circle(circle_mask, mask_center, mask_radius, cv::Scalar(255), -1);
    cv::bitwise_and(edges, circle_mask, edges);

    std::vector<cv::Vec4i> lines;
    int min_line_len = std::max(6, static_cast<int>(radius * 0.25));
    cv::HoughLinesP(edges, lines, 1, CV_PI / 180.0, 8, min_line_len, 5);

    if (lines.size() < 2) return rough_center;

    struct LineInfo {
        double angle;
        double a, b, c;
        double length;
    };

    std::vector<LineInfo> line_infos;
    line_infos.reserve(lines.size());

    for (const auto& l : lines) {
        double dx = l[2] - l[0];
        double dy = l[3] - l[1];
        double len = std::sqrt(dx * dx + dy * dy);
        if (len < 1.0) continue;

        double angle = std::atan2(dy, dx);
        if (angle < 0) angle += CV_PI;

        double a = -dy / len;
        double b = dx / len;
        double c = -(a * l[0] + b * l[1]);

        double dist_to_center = std::abs(a * mask_center.x + b * mask_center.y + c);
        if (dist_to_center > radius * 0.5) continue;

        line_infos.push_back({angle, a, b, c, len});
    }

    if (line_infos.size() < 2) return rough_center;

    std::vector<int> cluster_id(line_infos.size(), 0);

    double best_split_angle = 0;
    int best_split_score = -1;

    for (double ref_angle = 0; ref_angle < CV_PI; ref_angle += CV_PI / 36.0) {
        int count_a = 0, count_b = 0;
        double total_len_a = 0, total_len_b = 0;

        for (const auto& li : line_infos) {
            double diff = std::abs(li.angle - ref_angle);
            if (diff > CV_PI / 2.0) diff = CV_PI - diff;

            if (diff < CV_PI / 6.0) {
                count_a++;
                total_len_a += li.length;
            } else {
                count_b++;
                total_len_b += li.length;
            }
        }

        if (count_a >= 1 && count_b >= 1) {
            int score = count_a + count_b + static_cast<int>((total_len_a + total_len_b) / 10.0);
            if (score > best_split_score) {
                best_split_score = score;
                best_split_angle = ref_angle;
            }
        }
    }

    if (best_split_score < 0) return rough_center;

    struct ClusterAccum {
        double sum_a = 0, sum_b = 0, sum_c = 0;
        double total_weight = 0;
    };
    ClusterAccum cluster_a, cluster_b;

    for (size_t i = 0; i < line_infos.size(); ++i) {
        const auto& li = line_infos[i];
        double diff = std::abs(li.angle - best_split_angle);
        if (diff > CV_PI / 2.0) diff = CV_PI - diff;

        double w = li.length;
        if (diff < CV_PI / 6.0) {
            if (li.a < 0) { cluster_a.sum_a -= li.a * w; cluster_a.sum_b -= li.b * w; cluster_a.sum_c -= li.c * w; }
            else { cluster_a.sum_a += li.a * w; cluster_a.sum_b += li.b * w; cluster_a.sum_c += li.c * w; }
            cluster_a.total_weight += w;
        } else {
            if (li.b < 0) { cluster_b.sum_a -= li.a * w; cluster_b.sum_b -= li.b * w; cluster_b.sum_c -= li.c * w; }
            else { cluster_b.sum_a += li.a * w; cluster_b.sum_b += li.b * w; cluster_b.sum_c += li.c * w; }
            cluster_b.total_weight += w;
        }
    }

    if (cluster_a.total_weight < 1.0 || cluster_b.total_weight < 1.0) return rough_center;

    double a1 = cluster_a.sum_a / cluster_a.total_weight;
    double b1 = cluster_a.sum_b / cluster_a.total_weight;
    double c1 = cluster_a.sum_c / cluster_a.total_weight;
    double norm1 = std::sqrt(a1 * a1 + b1 * b1);
    if (norm1 < 1e-10) return rough_center;
    a1 /= norm1; b1 /= norm1; c1 /= norm1;

    double a2 = cluster_b.sum_a / cluster_b.total_weight;
    double b2 = cluster_b.sum_b / cluster_b.total_weight;
    double c2 = cluster_b.sum_c / cluster_b.total_weight;
    double norm2 = std::sqrt(a2 * a2 + b2 * b2);
    if (norm2 < 1e-10) return rough_center;
    a2 /= norm2; b2 /= norm2; c2 /= norm2;

    double det = a1 * b2 - a2 * b1;
    if (std::abs(det) < 1e-10) return rough_center;

    double local_x = (b1 * c2 - b2 * c1) / det;
    double local_y = (a2 * c1 - a1 * c2) / det;

    double global_x = local_x + x0;
    double global_y = local_y + y0;

    double shift = std::sqrt(std::pow(global_x - rough_center.x, 2) +
                              std::pow(global_y - rough_center.y, 2));
    if (shift > radius * 0.3) return rough_center;

    return cv::Point2d(global_x, global_y);
}

cv::Point2d refineCrossCenterByBrightCentroid(const cv::Mat& gray,
                                                const cv::Point2d& rough_center,
                                                double radius) {
    int roi_half = std::max(8, static_cast<int>(radius * 0.8));
    int cx = cvRound(rough_center.x);
    int cy = cvRound(rough_center.y);

    int x0 = std::max(0, cx - roi_half);
    int y0 = std::max(0, cy - roi_half);
    int x1 = std::min(gray.cols - 1, cx + roi_half);
    int y1 = std::min(gray.rows - 1, cy + roi_half);

    if (x1 <= x0 || y1 <= y0) return rough_center;

    cv::Rect roi_rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    cv::Mat roi = gray(roi_rect);

    cv::Mat circle_mask = cv::Mat::zeros(roi.rows, roi.cols, CV_8UC1);
    cv::Point mask_center(cx - x0, cy - y0);
    int mask_radius = static_cast<int>(radius * 0.75);
    cv::circle(circle_mask, mask_center, mask_radius, cv::Scalar(255), -1);

    cv::Mat blurred;
    cv::GaussianBlur(roi, blurred, cv::Size(3, 3), 0.5);

    double min_val, max_val;
    cv::minMaxLoc(blurred, &min_val, &max_val, nullptr, nullptr, circle_mask);
    double bright_thresh = min_val + (max_val - min_val) * 0.6;

    cv::Mat bright_mask = cv::Mat::zeros(roi.rows, roi.cols, CV_8UC1);
    for (int y = 0; y < blurred.rows; ++y) {
        const uint8_t* blur_row = blurred.ptr<uint8_t>(y);
        const uint8_t* mask_row = circle_mask.ptr<uint8_t>(y);
        uint8_t* bright_row = bright_mask.ptr<uint8_t>(y);
        for (int x = 0; x < blurred.cols; ++x) {
            if (mask_row[x] > 0 && blur_row[x] > bright_thresh) {
                bright_row[x] = 255;
            }
        }
    }

    cv::Mat open_kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3));
    cv::morphologyEx(bright_mask, bright_mask, cv::MORPH_OPEN, open_kernel);

    int bright_count = cv::countNonZero(bright_mask);
    if (bright_count < 4) return rough_center;

    double total_area = CV_PI * mask_radius * mask_radius;
    double bright_ratio = bright_count / total_area;
    if (bright_ratio > 0.6 || bright_ratio < 0.01) return rough_center;

    double sum_w = 0, sum_wx = 0, sum_wy = 0;
    for (int y = 0; y < blurred.rows; ++y) {
        const uint8_t* blur_row = blurred.ptr<uint8_t>(y);
        const uint8_t* bright_row = bright_mask.ptr<uint8_t>(y);
        for (int x = 0; x < blurred.cols; ++x) {
            if (bright_row[x] > 0) {
                double val = blur_row[x];
                double weight = val * val;
                sum_w += weight;
                sum_wx += weight * (x0 + x);
                sum_wy += weight * (y0 + y);
            }
        }
    }

    if (sum_w < 1e-10) return rough_center;

    cv::Point2d bright_center(sum_wx / sum_w, sum_wy / sum_w);
    double shift = std::sqrt(std::pow(bright_center.x - rough_center.x, 2) +
                              std::pow(bright_center.y - rough_center.y, 2));
    if (shift > radius * 0.3) return rough_center;

    return bright_center;
}

cv::Point2d refineCircleCenter(const cv::Mat& gray, const cv::Point2d& rough_center, double radius, const std::vector<cv::Point>& contour, double* out_fitted_radius) {
    cv::Point2d center = rough_center;
    double fitted_radius = 0;

    if (contour.size() >= 10) {
        std::vector<cv::Point> pts = contour;
        if (pts.size() > 300) {
            size_t step = pts.size() / 300;
            std::vector<cv::Point> sampled;
            for (size_t i = 0; i < pts.size(); i += step) {
                sampled.push_back(pts[i]);
            }
            pts = sampled;
        }

        cv::Mat A(pts.size(), 3, CV_64F);
        cv::Mat b_mat(pts.size(), 1, CV_64F);
        for (size_t i = 0; i < pts.size(); ++i) {
            double xi = pts[i].x;
            double yi = pts[i].y;
            A.at<double>(i, 0) = 2.0 * xi;
            A.at<double>(i, 1) = 2.0 * yi;
            A.at<double>(i, 2) = 1.0;
            b_mat.at<double>(i, 0) = xi * xi + yi * yi;
        }
        cv::Mat x;
        if (cv::solve(A, b_mat, x, cv::DECOMP_SVD)) {
            double fit_cx = x.at<double>(0, 0);
            double fit_cy = x.at<double>(1, 0);
            double fit_c = x.at<double>(2, 0);
            fitted_radius = std::sqrt(fit_cx * fit_cx + fit_cy * fit_cy + fit_c);
            double dist = std::sqrt(std::pow(fit_cx - rough_center.x, 2) + std::pow(fit_cy - rough_center.y, 2));
            if (dist < radius * 0.5) {
                center = cv::Point2d(fit_cx, fit_cy);
            }
        }
    }

    if (out_fitted_radius && fitted_radius > 0) {
        *out_fitted_radius = fitted_radius;
    }

    int r = std::max(3, static_cast<int>(radius * 0.6));
    int cx = cvRound(center.x);
    int cy = cvRound(center.y);

    int x0 = std::max(0, cx - r);
    int y0 = std::max(0, cy - r);
    int x1 = std::min(gray.cols - 1, cx + r);
    int y1 = std::min(gray.rows - 1, cy + r);

    if (x1 <= x0 || y1 <= y0)
        return center;

    cv::Rect roi_rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    cv::Mat roi = gray(roi_rect);

    cv::Mat roi_blur;
    cv::GaussianBlur(roi, roi_blur, cv::Size(3, 3), 0.5);

    double min_val, max_val;
    cv::minMaxLoc(roi_blur, &min_val, &max_val);

    double sum_w = 0;
    double sum_wx = 0;
    double sum_wy = 0;

    {
        double threshold_val = (min_val + max_val) * 0.5;
        for (int y = 0; y < roi_blur.rows; ++y) {
            for (int x = 0; x < roi_blur.cols; ++x) {
                double val = roi_blur.at<uint8_t>(y, x);
                if (val > threshold_val) continue;
                double weight = (threshold_val - val);
                weight = weight * weight;
                sum_w += weight;
                sum_wx += weight * (x0 + x);
                sum_wy += weight * (y0 + y);
            }
        }
    }

    if (sum_w > 0) {
        cv::Point2d intensity_center(sum_wx / sum_w, sum_wy / sum_w);
        double shift = std::sqrt(std::pow(intensity_center.x - center.x, 2) +
                                 std::pow(intensity_center.y - center.y, 2));
        if (shift < radius * 0.5) {
            center = intensity_center;
        }
    }

    {
        double threshold_val = (min_val + max_val) * 0.5;
        double sum_w2 = 0, sum_wx2 = 0, sum_wy2 = 0;
        for (int y = 0; y < roi_blur.rows; ++y) {
            for (int x = 0; x < roi_blur.cols; ++x) {
                double val = roi_blur.at<uint8_t>(y, x);
                if (val < threshold_val) continue;
                double weight = (val - threshold_val);
                weight = weight * weight;
                sum_w2 += weight;
                sum_wx2 += weight * (x0 + x);
                sum_wy2 += weight * (y0 + y);
            }
        }
        if (sum_w2 > 0) {
            cv::Point2d bright_center(sum_wx2 / sum_w2, sum_wy2 / sum_w2);
            double shift = std::sqrt(std::pow(bright_center.x - center.x, 2) +
                                     std::pow(bright_center.y - center.y, 2));
            if (shift < radius * 0.5 && sum_w2 > sum_w) {
                center = bright_center;
            }
        }
    }

    // ===== 十字精定位: Hough 直线交点法 (主策略) =====
    {
        cv::Point2d hough_center = refineCrossCenterByHough(gray, center, radius);
        double shift = std::sqrt(std::pow(hough_center.x - center.x, 2) +
                                  std::pow(hough_center.y - center.y, 2));
        if (shift > 0.01 && shift < radius * 0.3) {
            center = hough_center;
            return center;
        }
    }

    // ===== 十字精定位: 白色区域质心法 (回退策略一) =====
    {
        cv::Point2d bright_centroid = refineCrossCenterByBrightCentroid(gray, center, radius);
        double shift = std::sqrt(std::pow(bright_centroid.x - center.x, 2) +
                                  std::pow(bright_centroid.y - center.y, 2));
        if (shift > 0.01 && shift < radius * 0.3) {
            center = bright_centroid;
            return center;
        }
    }

    // ===== 十字精定位: 改进投影法 (回退策略二) =====
    {
        int cross_roi_size = std::max(5, static_cast<int>(radius * 0.6));
        int crx0 = std::max(0, cvRound(center.x) - cross_roi_size);
        int cry0 = std::max(0, cvRound(center.y) - cross_roi_size);
        int crx1 = std::min(gray.cols - 1, cvRound(center.x) + cross_roi_size);
        int cry1 = std::min(gray.rows - 1, cvRound(center.y) + cross_roi_size);

        if (crx1 > crx0 && cry1 > cry0) {
            cv::Rect cross_roi_rect(crx0, cry0, crx1 - crx0 + 1, cry1 - cry0 + 1);
            cv::Mat cross_roi = gray(cross_roi_rect);
            cv::Mat cross_blur;
            cv::GaussianBlur(cross_roi, cross_blur, cv::Size(3, 3), 0);

            cv::Mat cross_bin;
            cv::threshold(cross_blur, cross_bin, 0, 255, cv::THRESH_BINARY + cv::THRESH_OTSU);

            auto subpixelPeak = [](const cv::Mat& proj) -> double {
                double min_val, max_val;
                cv::Point min_loc, max_loc;
                cv::minMaxLoc(proj, &min_val, &max_val, &min_loc, &max_loc);
                int peak = max_loc.y > 0 ? max_loc.y : max_loc.x;
                if (peak <= 0 || peak >= static_cast<int>(proj.total()) - 1) return static_cast<double>(peak);
                double y0 = proj.at<double>(peak - 1);
                double y1 = proj.at<double>(peak);
                double y2 = proj.at<double>(peak + 1);
                double denom = 2.0 * (2 * y1 - y0 - y2);
                if (std::abs(denom) < 1e-10) return static_cast<double>(peak);
                double sub = (y0 - y2) / denom;
                sub = std::max(-0.5, std::min(0.5, sub));
                return peak + sub;
            };

            cv::Mat h_proj = cv::Mat::zeros(cross_bin.rows, 1, CV_64F);
            cv::Mat v_proj = cv::Mat::zeros(1, cross_bin.cols, CV_64F);
            for (int y = 0; y < cross_bin.rows; ++y) {
                double s = 0;
                for (int x = 0; x < cross_bin.cols; ++x) {
                    s += cross_bin.at<uint8_t>(y, x);
                }
                h_proj.at<double>(y, 0) = s;
            }
            for (int x = 0; x < cross_bin.cols; ++x) {
                double s = 0;
                for (int y = 0; y < cross_bin.rows; ++y) {
                    s += cross_bin.at<uint8_t>(y, x);
                }
                v_proj.at<double>(0, x) = s;
            }

            double cross_cx = subpixelPeak(v_proj) + crx0;
            double cross_cy = subpixelPeak(h_proj) + cry0;

            double cross_shift = std::sqrt(std::pow(cross_cx - center.x, 2) +
                                           std::pow(cross_cy - center.y, 2));
            if (cross_shift < radius * 0.3) {
                center = cv::Point2d(cross_cx, cross_cy);
            }
        }
    }

    return center;
}

bool detectSingleCircle(const cv::Mat& image, const cv::Rect& roi,
                        cv::Point2d& out_center, double& out_radius) {
    cv::Rect safe_roi = roi & cv::Rect(0, 0, image.cols, image.rows);
    if (safe_roi.width < 10 || safe_roi.height < 10) return false;

    cv::Mat crop = image(safe_roi).clone();

    cv::Mat gray;
    if (crop.channels() == 3) {
        cv::cvtColor(crop, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = crop.clone();
    }

    cv::Mat blurred;
    cv::GaussianBlur(gray, blurred, cv::Size(5, 5), 1.5);

    cv::Mat binary;
    cv::threshold(blurred, binary, 0, 255, cv::THRESH_BINARY_INV + cv::THRESH_OTSU);

    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3));
    cv::morphologyEx(binary, binary, cv::MORPH_CLOSE, kernel, cv::Point(-1, -1), 1);
    cv::morphologyEx(binary, binary, cv::MORPH_OPEN, kernel, cv::Point(-1, -1), 1);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(binary, contours, cv::RETR_LIST, cv::CHAIN_APPROX_NONE);

    double best_circularity = 0;
    int best_idx = -1;
    double best_area = 0;
    for (size_t i = 0; i < contours.size(); ++i) {
        double area = cv::contourArea(contours[i]);
        if (area < 100) continue;

        double perimeter = cv::arcLength(contours[i], true);
        if (perimeter <= 0) continue;

        double circularity = 4.0 * CV_PI * area / (perimeter * perimeter);
        if (circularity > best_circularity) {
            best_circularity = circularity;
            best_idx = static_cast<int>(i);
            best_area = area;
        }
    }

    if (best_idx < 0 || best_circularity < 0.5) return false;

    cv::Moments m = cv::moments(contours[best_idx]);
    if (m.m00 <= 0) return false;

    cv::Point2d local_center(m.m10 / m.m00, m.m01 / m.m00);
    double radius = std::sqrt(best_area / CV_PI);

    cv::Point2d refined = refineCircleCenter(gray, local_center, radius, contours[best_idx]);

    out_center = cv::Point2d(refined.x + safe_roi.x, refined.y + safe_roi.y);
    out_radius = radius;
    return true;
}

ShapeInfo detectShape(const cv::Mat& image, const cv::Rect& roi) {
    try {
    ShapeInfo info;
    cv::Rect safe_roi = roi & cv::Rect(0, 0, image.cols, image.rows);
    if (safe_roi.width < 10 || safe_roi.height < 10) return info;

    int pad = std::max(10, static_cast<int>(std::min(safe_roi.width, safe_roi.height) * 0.15));
    cv::Rect padded_roi(
        std::max(0, safe_roi.x - pad),
        std::max(0, safe_roi.y - pad),
        0, 0);
    padded_roi.width = std::min(image.cols - padded_roi.x, safe_roi.width + 2 * pad);
    padded_roi.height = std::min(image.rows - padded_roi.y, safe_roi.height + 2 * pad);

    cv::Mat crop = image(padded_roi).clone();

    cv::Mat gray;
    if (crop.channels() == 3) {
        cv::cvtColor(crop, gray, cv::COLOR_BGR2GRAY);
    } else if (crop.channels() == 1) {
        gray = crop.clone();
    } else {
        cv::Mat tmp;
        cv::cvtColor(crop, tmp, cv::COLOR_BGRA2BGR);
        cv::cvtColor(tmp, gray, cv::COLOR_BGR2GRAY);
    }

    cv::Mat blurred;
    cv::GaussianBlur(gray, blurred, cv::Size(5, 5), 0);

    cv::Mat morph_kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3));

    cv::Point2d local_roi_center(
        safe_roi.x - padded_roi.x + safe_roi.width / 2.0,
        safe_roi.y - padded_roi.y + safe_roi.height / 2.0);
    double roi_diag = std::sqrt(static_cast<double>(safe_roi.width) * safe_roi.width +
                                static_cast<double>(safe_roi.height) * safe_roi.height);

    struct ContourCandidate {
        std::vector<cv::Point> contour;
        double score = -1;
        double circularity = 0;
        double area = 0;
    };

    auto findBestInBinary = [&](const cv::Mat& binary) -> ContourCandidate {
        ContourCandidate best;
        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(binary, contours, cv::RETR_LIST, cv::CHAIN_APPROX_NONE);
        for (const auto& cnt : contours) {
            double area = cv::contourArea(cnt);
            if (area < 100) continue;
            cv::Moments m = cv::moments(cnt);
            if (m.m00 <= 0) continue;
            cv::Point2d mc(m.m10 / m.m00, m.m01 / m.m00);
            double dist = std::sqrt(std::pow(mc.x - local_roi_center.x, 2) +
                                    std::pow(mc.y - local_roi_center.y, 2));
            if (dist > roi_diag * 0.6) continue;
            double perimeter = cv::arcLength(cnt, true);
            if (perimeter <= 0) continue;
            double circ = 4.0 * CV_PI * area / (perimeter * perimeter);
            double score = circ * 0.6 + (1.0 - dist / (roi_diag * 0.6)) * 0.4;
            if (score > best.score) {
                best.contour = cnt;
                best.score = score;
                best.circularity = circ;
                best.area = area;
            }
        }
        return best;
    };

    ContourCandidate best_candidate;

    cv::Mat binary_inv, binary_norm;
    cv::threshold(blurred, binary_inv, 0, 255, cv::THRESH_BINARY_INV + cv::THRESH_OTSU);
    cv::threshold(blurred, binary_norm, 0, 255, cv::THRESH_BINARY + cv::THRESH_OTSU);
    cv::morphologyEx(binary_inv, binary_inv, cv::MORPH_CLOSE, morph_kernel, cv::Point(-1, -1), 1);
    cv::morphologyEx(binary_inv, binary_inv, cv::MORPH_OPEN, morph_kernel, cv::Point(-1, -1), 1);
    cv::morphologyEx(binary_norm, binary_norm, cv::MORPH_CLOSE, morph_kernel, cv::Point(-1, -1), 1);
    cv::morphologyEx(binary_norm, binary_norm, cv::MORPH_OPEN, morph_kernel, cv::Point(-1, -1), 1);

    auto c_inv = findBestInBinary(binary_inv);
    auto c_norm = findBestInBinary(binary_norm);
    if (c_inv.score > best_candidate.score) best_candidate = c_inv;
    if (c_norm.score > best_candidate.score) best_candidate = c_norm;

    if (best_candidate.score < 0) {
        cv::Mat adaptive_inv, adaptive_norm;
        cv::adaptiveThreshold(blurred, adaptive_inv, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                              cv::THRESH_BINARY_INV, 31, 3);
        cv::adaptiveThreshold(blurred, adaptive_norm, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                              cv::THRESH_BINARY, 31, 3);
        cv::morphologyEx(adaptive_inv, adaptive_inv, cv::MORPH_CLOSE, morph_kernel, cv::Point(-1, -1), 1);
        cv::morphologyEx(adaptive_inv, adaptive_inv, cv::MORPH_OPEN, morph_kernel, cv::Point(-1, -1), 1);
        cv::morphologyEx(adaptive_norm, adaptive_norm, cv::MORPH_CLOSE, morph_kernel, cv::Point(-1, -1), 1);
        cv::morphologyEx(adaptive_norm, adaptive_norm, cv::MORPH_OPEN, morph_kernel, cv::Point(-1, -1), 1);

        auto c_ainv = findBestInBinary(adaptive_inv);
        auto c_anorm = findBestInBinary(adaptive_norm);
        if (c_ainv.score > best_candidate.score) best_candidate = c_ainv;
        if (c_anorm.score > best_candidate.score) best_candidate = c_anorm;
    }

    if (best_candidate.score < 0) {
        cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(3.0, cv::Size(32, 32));
        cv::Mat enhanced;
        clahe->apply(blurred, enhanced);
        double otsu_thresh = 0;
        cv::threshold(enhanced, cv::Mat(), otsu_thresh, 255, cv::THRESH_OTSU);
        cv::Mat edges;
        cv::Canny(enhanced, edges, 0.4 * otsu_thresh, otsu_thresh);
        cv::morphologyEx(edges, edges, cv::MORPH_CLOSE, morph_kernel, cv::Point(-1, -1), 1);

        std::vector<std::vector<cv::Point>> canny_contours;
        cv::findContours(edges, canny_contours, cv::RETR_LIST, cv::CHAIN_APPROX_NONE);
        for (const auto& cnt : canny_contours) {
            double area = cv::contourArea(cnt);
            if (area < 100) continue;
            cv::Moments m = cv::moments(cnt);
            if (m.m00 <= 0) continue;
            cv::Point2d mc(m.m10 / m.m00, m.m01 / m.m00);
            double dist = std::sqrt(std::pow(mc.x - local_roi_center.x, 2) +
                                    std::pow(mc.y - local_roi_center.y, 2));
            if (dist > roi_diag * 0.6) continue;
            double perimeter = cv::arcLength(cnt, true);
            if (perimeter <= 0) continue;
            double circ = 4.0 * CV_PI * area / (perimeter * perimeter);
            double score = circ * 0.6 + (1.0 - dist / (roi_diag * 0.6)) * 0.4;
            if (score > best_candidate.score) {
                best_candidate.contour = cnt;
                best_candidate.score = score;
                best_candidate.circularity = circ;
                best_candidate.area = area;
            }
        }
    }

    if (best_candidate.score < 0 || best_candidate.contour.empty()) {
        std::cerr << "[detectShape] 未找到合适轮廓, ROI=" << safe_roi << std::endl;
        return info;
    }

    const auto& best_contour = best_candidate.contour;
    double contour_area = best_candidate.area;
    double circularity = best_candidate.circularity;
    info.circularity = circularity;
    info.area = contour_area;

    std::cerr << "[detectShape] best contour: area=" << contour_area
              << " circularity=" << circularity
              << " points=" << best_contour.size() << std::endl;

    bool can_fit_ellipse = (best_contour.size() >= 5);

    if (can_fit_ellipse) {
        cv::RotatedRect ellipse;
        try {
            ellipse = cv::fitEllipse(best_contour);
        } catch (...) {
            can_fit_ellipse = false;
        }
    }

    if (can_fit_ellipse) {
        cv::RotatedRect ellipse = cv::fitEllipse(best_contour);
        double w = ellipse.size.width;
        double h = ellipse.size.height;
        double max_wh = std::max(w, h);
        if (max_wh < 1.0) return info;
        double axis_ratio = std::min(w, h) / max_wh;
        double fit_radius = (w + h) / 4.0;

        std::cerr << "[detectShape] fitEllipse: w=" << w << " h=" << h
                  << " axis_ratio=" << axis_ratio << " fit_radius=" << fit_radius << std::endl;

        if (axis_ratio > 0.7) {
            info.type = ShapeType::CIRCLE;
            info.center = cv::Point2d(ellipse.center.x + padded_roi.x,
                                      ellipse.center.y + padded_roi.y);
            info.radius = fit_radius;

            double fitted_radius = 0;
            cv::Point2d local_center(ellipse.center.x, ellipse.center.y);
            cv::Point2d refined = refineCircleCenter(gray, local_center, fit_radius, best_contour, &fitted_radius);
            double shift = std::sqrt(std::pow(refined.x - local_center.x, 2) +
                                     std::pow(refined.y - local_center.y, 2));
            if (shift < fit_radius * 0.3) {
                info.center = cv::Point2d(refined.x + padded_roi.x, refined.y + padded_roi.y);
            }
            if (fitted_radius > 0 && std::abs(fitted_radius - fit_radius) / fit_radius < 0.2) {
                info.radius = fitted_radius;
            }

            cv::Mat blur_hough;
            cv::GaussianBlur(gray, blur_hough, cv::Size(7, 7), 2);
            std::vector<cv::Vec3f> hc;
            int min_r = std::max(5, static_cast<int>(info.radius * 0.7));
            int max_r = std::min(gray.cols / 2, static_cast<int>(info.radius * 1.3));
            if (max_r > min_r) {
                cv::HoughCircles(blur_hough, hc, cv::HOUGH_GRADIENT, 1,
                                 std::max(20, min_r), 200, 30, min_r, max_r);
            }
            if (!hc.empty()) {
                double best_dist = std::numeric_limits<double>::max();
                double best_hr = 0;
                cv::Point2d local_info_center(info.center.x - padded_roi.x,
                                              info.center.y - padded_roi.y);
                for (const auto& h : hc) {
                    double d = std::sqrt(std::pow(h[0] - local_info_center.x, 2) +
                                         std::pow(h[1] - local_info_center.y, 2));
                    if (d < best_dist) {
                        best_dist = d;
                        best_hr = h[2];
                    }
                }
                if (best_dist < info.radius * 0.5 && best_hr > 0) {
                    info.radius = best_hr;
                }
            }

            std::cerr << std::fixed << std::setprecision(4);
            std::cerr << "[detectShape] CIRCLE: center=(" << info.center.x << "," << info.center.y
                      << ") radius=" << info.radius << std::endl;
        } else {
            std::vector<cv::Point> approx;
            double eps = 0.02 * cv::arcLength(best_contour, true);
            cv::approxPolyDP(best_contour, approx, eps, true);

            if (approx.size() >= 4 && approx.size() <= 8) {
                info.type = ShapeType::RECTANGLE;
            } else {
                info.type = ShapeType::OTHER;
            }

            cv::RotatedRect rrect = cv::minAreaRect(best_contour);
            rrect.center.x += static_cast<float>(padded_roi.x);
            rrect.center.y += static_cast<float>(padded_roi.y);
            info.rrect = rrect;
            info.center = cv::Point2d(rrect.center.x, rrect.center.y);

            std::cerr << "[detectShape] NON-CIRCLE: axis_ratio=" << axis_ratio
                      << " type=" << static_cast<int>(info.type) << std::endl;
        }
    } else {
        if (circularity >= 0.65) {
            info.type = ShapeType::CIRCLE;
            cv::Moments m = cv::moments(best_contour);
            if (m.m00 <= 0) return info;
            info.center = cv::Point2d(m.m10 / m.m00 + padded_roi.x,
                                      m.m01 / m.m00 + padded_roi.y);
            info.radius = std::sqrt(contour_area / CV_PI);
        } else {
            std::vector<cv::Point> approx;
            double eps = 0.02 * cv::arcLength(best_contour, true);
            cv::approxPolyDP(best_contour, approx, eps, true);
            if (approx.size() >= 4 && approx.size() <= 8) {
                info.type = ShapeType::RECTANGLE;
            } else {
                info.type = ShapeType::OTHER;
            }
            cv::RotatedRect rrect = cv::minAreaRect(best_contour);
            rrect.center.x += static_cast<float>(padded_roi.x);
            rrect.center.y += static_cast<float>(padded_roi.y);
            info.rrect = rrect;
            info.center = cv::Point2d(rrect.center.x, rrect.center.y);
        }
    }

    return info;
    } catch (const cv::Exception& e) {
        std::cerr << "detectShape OpenCV exception: " << e.what() << std::endl;
        return ShapeInfo();
    } catch (const std::exception& e) {
        std::cerr << "detectShape exception: " << e.what() << std::endl;
        return ShapeInfo();
    } catch (...) {
        std::cerr << "detectShape unknown exception" << std::endl;
        return ShapeInfo();
    }
}

static double normalizeAngle(double angle) {
    angle = std::fmod(angle, 360.0);
    if (angle < 0) angle += 360.0;
    return angle;
}

cv::Mat cropShapeTemplate(const cv::Mat& image,
                           const ShapeInfo& shape,
                           double padding,
                           cv::Mat* out_mask) {
    if (shape.type == ShapeType::NONE) return cv::Mat();

    if (shape.type == ShapeType::CIRCLE) {
        return cropCircleTemplate(image, shape.center, shape.radius, padding, out_mask);
    }

    cv::RotatedRect rrect = shape.rrect;

    float w = rrect.size.width;
    float h = rrect.size.height;
    double angle_offset = normalizeAngle(static_cast<double>(rrect.angle));

    cv::Point2f src_pts[4];
    rrect.points(src_pts);

    cv::Point2f dst_pts[3];
    dst_pts[0] = cv::Point2f(0, h);
    dst_pts[1] = cv::Point2f(0, 0);
    dst_pts[2] = cv::Point2f(w, 0);

    cv::Point2f src_3[3] = { src_pts[0], src_pts[1], src_pts[2] };
    cv::Mat M = cv::getAffineTransform(src_3, dst_pts);

    cv::Mat tmpl;
    cv::warpAffine(image, tmpl, M, cv::Size(static_cast<int>(w), static_cast<int>(h)),
                   cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));

    if (tmpl.empty() || tmpl.cols < 5 || tmpl.rows < 5) return cv::Mat();

    if (out_mask) {
        cv::Rect bounding = rrect.boundingRect();
        bounding &= cv::Rect(0, 0, image.cols, image.rows);

        if (bounding.width > 0 && bounding.height > 0) {
            cv::Mat grabcut_mask(image.rows, image.cols, CV_8UC1, cv::Scalar(cv::GC_BGD));
            grabcut_mask(bounding).setTo(cv::Scalar(cv::GC_PR_FGD));

            cv::Mat bgd_model, fgd_model;
            try {
                cv::grabCut(image, grabcut_mask, bounding, bgd_model, fgd_model, 5, cv::GC_INIT_WITH_RECT);
                cv::grabCut(image, grabcut_mask, bounding, bgd_model, fgd_model, 3, cv::GC_EVAL);
            } catch (...) {
                cv::Mat fg_mask_raw = ((grabcut_mask == cv::GC_FGD) | (grabcut_mask == cv::GC_PR_FGD));
                fg_mask_raw.convertTo(*out_mask, CV_8U, 255);
                cv::warpAffine(*out_mask, *out_mask, M, cv::Size(static_cast<int>(w), static_cast<int>(h)),
                               cv::INTER_NEAREST, cv::BORDER_CONSTANT, cv::Scalar(0));
                return tmpl;
            }

            cv::Mat fg_mask_raw = ((grabcut_mask == cv::GC_FGD) | (grabcut_mask == cv::GC_PR_FGD));
            cv::Mat fg_mask;
            fg_mask_raw.convertTo(fg_mask, CV_8U, 255);

            cv::warpAffine(fg_mask, *out_mask, M, cv::Size(static_cast<int>(w), static_cast<int>(h)),
                           cv::INTER_NEAREST, cv::BORDER_CONSTANT, cv::Scalar(0));

            double fg_ratio = cv::countNonZero(*out_mask) / (double)(out_mask->rows * out_mask->cols);
            if (fg_ratio < 0.1) {
                *out_mask = cv::Mat::zeros(tmpl.rows, tmpl.cols, CV_8UC1);
                cv::rectangle(*out_mask, cv::Point(2, 2),
                              cv::Point(tmpl.cols - 3, tmpl.rows - 3), cv::Scalar(255), -1);
            }
        } else {
            *out_mask = cv::Mat::zeros(tmpl.rows, tmpl.cols, CV_8UC1);
            cv::rectangle(*out_mask, cv::Point(2, 2),
                          cv::Point(tmpl.cols - 3, tmpl.rows - 3), cv::Scalar(255), -1);
        }
    }

    return tmpl;
}

cv::Mat cropCircleTemplate(const cv::Mat& image,
                           const cv::Point2d& center, double radius,
                           double padding,
                           cv::Mat* out_mask) {
    int half_size = static_cast<int>(radius * padding);
    int cx = cvRound(center.x);
    int cy = cvRound(center.y);

    int x0 = std::max(0, cx - half_size);
    int y0 = std::max(0, cy - half_size);
    int x1 = std::min(image.cols - 1, cx + half_size);
    int y1 = std::min(image.rows - 1, cy + half_size);

    if (x1 <= x0 || y1 <= y0) return cv::Mat();

    cv::Rect crop_rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    cv::Mat tmpl = image(crop_rect).clone();

    if (out_mask) {
        *out_mask = cv::Mat::zeros(tmpl.rows, tmpl.cols, CV_8UC1);
        cv::Point mask_center(cx - x0, cy - y0);
        int mask_radius = static_cast<int>(radius + 2);
        cv::circle(*out_mask, mask_center, mask_radius, cv::Scalar(255), -1);
    }

    return tmpl;
}

std::vector<CircleInfo> detectCirclesByTemplate(const cv::Mat& image,
                                                 const cv::Mat& tmpl,
                                                 const cv::Mat& tmpl_mask,
                                                 double match_threshold,
                                                 double min_dist) {
    std::vector<CircleInfo> circles;

    if (image.empty() || tmpl.empty()) return circles;
    if (tmpl.cols > image.cols || tmpl.rows > image.rows) return circles;

    cv::Mat gray;
    if (image.channels() == 3) {
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = image.clone();
    }

    cv::Mat tmpl_gray;
    if (tmpl.channels() == 3) {
        cv::cvtColor(tmpl, tmpl_gray, cv::COLOR_BGR2GRAY);
    } else {
        tmpl_gray = tmpl.clone();
    }

    if (!tmpl_mask.empty() && tmpl_mask.size() != tmpl_gray.size()) {
        return circles;
    }

    if (!tmpl_mask.empty()) {
        cv::Scalar fg_mean = cv::mean(tmpl_gray, tmpl_mask);
        tmpl_gray.setTo(fg_mean, ~tmpl_mask);
    }

    cv::Mat result;
    cv::matchTemplate(gray, tmpl_gray, result, cv::TM_CCOEFF_NORMED);

    double best_score = 0;
    cv::minMaxLoc(result, nullptr, &best_score, nullptr, nullptr);

    if (best_score < match_threshold && !tmpl_mask.empty()) {
        cv::Mat tmpl_gray_nomask;
        if (tmpl.channels() == 3) {
            cv::cvtColor(tmpl, tmpl_gray_nomask, cv::COLOR_BGR2GRAY);
        } else {
            tmpl_gray_nomask = tmpl.clone();
        }
        cv::matchTemplate(gray, tmpl_gray_nomask, result, cv::TM_CCOEFF_NORMED);
    }

    double tmpl_radius = std::min(tmpl.cols, tmpl.rows) / 2.0;
    if (!tmpl_mask.empty()) {
        cv::Moments m = cv::moments(tmpl_mask, true);
        if (m.m00 > 0) {
            tmpl_radius = std::sqrt(m.m00 / CV_PI);
        }
    }

    while (true) {
        double max_val;
        cv::Point max_loc;
        cv::minMaxLoc(result, nullptr, &max_val, nullptr, &max_loc);

        if (max_val < match_threshold) break;

        cv::Point2d center(max_loc.x + tmpl.cols / 2.0,
                           max_loc.y + tmpl.rows / 2.0);

        int cx = cvRound(center.x);
        int cy = cvRound(center.y);
        int r = cvRound(tmpl_radius * 0.8);

        cv::Point2d refined = center;
        if (cx - r >= 0 && cy - r >= 0 &&
            cx + r < gray.cols && cy + r < gray.rows) {
            cv::Rect roi_rect(cx - r, cy - r, 2 * r + 1, 2 * r + 1);
            cv::Mat roi = gray(roi_rect);

            cv::Mat roi_blur;
            cv::GaussianBlur(roi, roi_blur, cv::Size(5, 5), 0);
            cv::Mat roi_binary_inv, roi_binary_norm;
            cv::threshold(roi_blur, roi_binary_inv, 0, 255, cv::THRESH_BINARY_INV + cv::THRESH_OTSU);
            cv::threshold(roi_blur, roi_binary_norm, 0, 255, cv::THRESH_BINARY + cv::THRESH_OTSU);
            int inv_cnt = cv::countNonZero(roi_binary_inv);
            int norm_cnt = cv::countNonZero(roi_binary_norm);
            cv::Mat roi_binary = (norm_cnt <= inv_cnt) ? roi_binary_norm : roi_binary_inv;

            cv::Mat mk = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3));
            cv::morphologyEx(roi_binary, roi_binary, cv::MORPH_CLOSE, mk, cv::Point(-1, -1), 1);
            cv::morphologyEx(roi_binary, roi_binary, cv::MORPH_OPEN, mk, cv::Point(-1, -1), 1);

            std::vector<std::vector<cv::Point>> roi_contours;
            cv::findContours(roi_binary, roi_contours, cv::RETR_LIST, cv::CHAIN_APPROX_NONE);

            cv::Point2d roi_center(r, r);
            double best_score = -1;
            int best_ci = -1;
            for (size_t ci = 0; ci < roi_contours.size(); ++ci) {
                double ca = cv::contourArea(roi_contours[ci]);
                if (ca < 50) continue;
                cv::Moments cm = cv::moments(roi_contours[ci]);
                if (cm.m00 <= 0) continue;
                cv::Point2d mc(cm.m10 / cm.m00, cm.m01 / cm.m00);
                double dist = std::sqrt(std::pow(mc.x - roi_center.x, 2) + std::pow(mc.y - roi_center.y, 2));
                double max_dist = std::sqrt(roi_center.x * roi_center.x + roi_center.y * roi_center.y);
                double perimeter = cv::arcLength(roi_contours[ci], true);
                if (perimeter <= 0) continue;
                double circ = 4.0 * CV_PI * ca / (perimeter * perimeter);
                double score = circ * 0.6 + (1.0 - dist / max_dist) * 0.4;
                if (score > best_score) { best_score = score; best_ci = static_cast<int>(ci); }
            }

            if (best_ci >= 0) {
                cv::Point2d local_rough(roi_center.x, roi_center.y);
                cv::Point2d local_refined = refineCircleCenter(roi_blur, local_rough, tmpl_radius, roi_contours[best_ci]);
                cv::Point2d global_refined(local_refined.x + cx - r, local_refined.y + cy - r);
                double shift = std::sqrt(std::pow(global_refined.x - center.x, 2) +
                                         std::pow(global_refined.y - center.y, 2));
                if (shift < tmpl_radius * 0.5) {
                    refined = global_refined;
                }
            }
        }

        CircleInfo ci;
        ci.center = refined;
        ci.area = CV_PI * tmpl_radius * tmpl_radius;
        ci.circularity = 1.0;
        ci.radius = tmpl_radius;
        ci.index = -1;
        circles.push_back(ci);

        int suppress_x0 = std::max(0, max_loc.x - static_cast<int>(min_dist));
        int suppress_y0 = std::max(0, max_loc.y - static_cast<int>(min_dist));
        int suppress_x1 = std::min(result.cols - 1, max_loc.x + static_cast<int>(min_dist));
        int suppress_y1 = std::min(result.rows - 1, max_loc.y + static_cast<int>(min_dist));

        for (int y = suppress_y0; y <= suppress_y1; ++y) {
            for (int x = suppress_x0; x <= suppress_x1; ++x) {
                result.at<float>(y, x) = 0.0f;
            }
        }
    }

    if (circles.empty()) return circles;

    std::sort(circles.begin(), circles.end(),
              [](const CircleInfo& a, const CircleInfo& b) {
                  return a.center.y < b.center.y;
              });

    std::vector<std::vector<CircleInfo>> rows;
    std::vector<CircleInfo> current_row;
    current_row.push_back(circles[0]);

    double row_gap = tmpl.rows * 1.5;
    if (circles.size() >= 4) {
        std::vector<double> y_diffs;
        for (size_t i = 1; i < circles.size(); ++i) {
            y_diffs.push_back(circles[i].center.y - circles[i - 1].center.y);
        }
        std::sort(y_diffs.begin(), y_diffs.end());

        double max_gap = 0;
        double gap_mid = 0;
        for (size_t i = 1; i < y_diffs.size(); ++i) {
            double gap = y_diffs[i] - y_diffs[i - 1];
            if (gap > max_gap) {
                max_gap = gap;
                gap_mid = (y_diffs[i] + y_diffs[i - 1]) * 0.5;
            }
        }

        if (max_gap > 0) {
            row_gap = gap_mid;
        } else {
            row_gap = y_diffs[y_diffs.size() / 2] * 2.0;
        }
        if (row_gap < 1.0) row_gap = 1.0;
    } else if (circles.size() >= 2) {
        std::vector<double> y_diffs;
        for (size_t i = 1; i < circles.size(); ++i) {
            y_diffs.push_back(circles[i].center.y - circles[i - 1].center.y);
        }
        std::sort(y_diffs.begin(), y_diffs.end());
        row_gap = y_diffs[y_diffs.size() / 2] * 2.0;
        if (row_gap < 1.0) row_gap = 1.0;
    }

    for (size_t i = 1; i < circles.size(); ++i) {
        if (std::abs(circles[i].center.y - current_row.back().center.y) > row_gap) {
            rows.push_back(current_row);
            current_row.clear();
        }
        current_row.push_back(circles[i]);
    }
    rows.push_back(current_row);

    for (auto& row : rows) {
        std::sort(row.begin(), row.end(),
                  [](const CircleInfo& a, const CircleInfo& b) {
                      return a.center.x < b.center.x;
                  });
    }

    std::vector<CircleInfo> sorted;
    int idx = 0;
    for (auto& row : rows) {
        for (auto& ci : row) {
            ci.index = idx++;
            sorted.push_back(ci);
        }
    }

    return sorted;
}

void drawCircles(cv::Mat& image, const std::vector<CircleInfo>& circles) {
    for (const auto& ci : circles) {
        cv::Point center(cvRound(ci.center.x), cvRound(ci.center.y));
        int radius = cvRound(ci.radius);

        cv::circle(image, center, radius, cv::Scalar(0, 255, 0), 2);

        int cs = 15;
        cv::line(image, cv::Point(center.x - cs, center.y),
                 cv::Point(center.x + cs, center.y), cv::Scalar(0, 0, 255), 2);
        cv::line(image, cv::Point(center.x, center.y - cs),
                 cv::Point(center.x, center.y + cs), cv::Scalar(0, 0, 255), 2);

        char label[64];
        snprintf(label, sizeof(label), "#%d", ci.index);
        cv::putText(image, label, cv::Point(center.x + 10, center.y - 10),
                    cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 255), 2);

        char coord[64];
        snprintf(coord, sizeof(coord), "(%.4f,%.4f)", ci.center.x, ci.center.y);
        cv::putText(image, coord, cv::Point(center.x + 10, center.y + 15),
                    cv::FONT_HERSHEY_SIMPLEX, 0.4, cv::Scalar(0, 0, 255), 1);
    }
}

AffineResult computeAffine(const std::vector<cv::Point2d>& pixel_pts,
                           const std::vector<cv::Point2d>& robot_pts,
                           const CalibConfig& cfg) {
    AffineResult result;

    if (pixel_pts.size() < 3 || robot_pts.size() < 3) {
        std::cerr << "[标定] 至少需要3对点才能计算仿射变换" << std::endl;
        return result;
    }

    size_t n = std::min(pixel_pts.size(), robot_pts.size());
    std::vector<cv::Point2d> px(pixel_pts.begin(), pixel_pts.begin() + n);
    std::vector<cv::Point2d> rb(robot_pts.begin(), robot_pts.begin() + n);

    cv::Mat inliers;
    cv::Mat affine = cv::estimateAffine2D(px, rb, inliers);

    if (affine.empty()) {
        std::cerr << "[标定] 仿射变换计算失败" << std::endl;
        return result;
    }

    double total_error = 0;
    double max_error = 0;
    for (size_t i = 0; i < n; ++i) {
        cv::Mat pt = (cv::Mat_<double>(3, 1) << px[i].x, px[i].y, 1.0);
        cv::Mat transformed = affine * pt;
        double dx = transformed.at<double>(0, 0) - rb[i].x;
        double dy = transformed.at<double>(1, 0) - rb[i].y;
        double err = std::sqrt(dx * dx + dy * dy);
        total_error += err;
        if (err > max_error) max_error = err;

        if (err > cfg.reproj_error_threshold) {
            std::cerr << "[标定] 点 #" << i << " 重投影误差过大: " << err << std::endl;
        }
    }

    double mean_error = total_error / n;

    if (mean_error > cfg.reproj_error_threshold) {
        std::cerr << "[标定] 平均重投影误差超过阈值: " << cfg.reproj_error_threshold << std::endl;
    }

    result.affine_robot = affine;
    result.mean_error = mean_error;
    result.max_error = max_error;

    // 从机器人坐标推导相机物理网格坐标，计算相机仿射矩阵
    auto cam_pts = generateCameraPhysicalCoords(rb);
    if (cam_pts.size() == n) {
        cv::Mat cam_inliers;
        cv::Mat cam_affine = cv::estimateAffine2D(px, cam_pts, cam_inliers);
        if (!cam_affine.empty()) {
            result.affine_camera = cam_affine;

            // 全局正偏移：确保视野内所有像素的相机物理坐标均非负
            // 1. 估算像素覆盖范围（标定点 + margin）
            const double kMargin = 200.0;
            double max_px_x = 0.0, max_px_y = 0.0;
            for (size_t i = 0; i < n; ++i) {
                if (px[i].x > max_px_x) max_px_x = px[i].x;
                if (px[i].y > max_px_y) max_px_y = px[i].y;
            }
            max_px_x += kMargin;
            max_px_y += kMargin;

            // 2. 评估视野角点的 cam 坐标，找出最小值
            std::vector<cv::Point2d> corners;
            for (double x : {0.0, max_px_x}) {
                for (double y : {0.0, max_px_y}) {
                    cv::Mat pt = (cv::Mat_<double>(3, 1) << x, y, 1.0);
                    cv::Mat t = result.affine_camera * pt;
                    corners.emplace_back(t.at<double>(0, 0), t.at<double>(1, 0));
                }
            }

            double min_cam_x = corners[0].x, min_cam_y = corners[0].y;
            for (const auto& c : corners) {
                if (c.x < min_cam_x) min_cam_x = c.x;
                if (c.y < min_cam_y) min_cam_y = c.y;
            }

            // 3. 偏移 = -min（使最小坐标归零）+ 小 padding
            const double kPad = 5.0;
            double offset_x = (min_cam_x < 0.0) ? -min_cam_x + kPad : 0.0;
            double offset_y = (min_cam_y < 0.0) ? -min_cam_y + kPad : 0.0;

            result.affine_camera.at<double>(0, 2) += offset_x;
            result.affine_camera.at<double>(1, 2) += offset_y;
        } else {
            std::cerr << "[标定] 相机仿射矩阵计算失败" << std::endl;
        }
    } else {
        std::cerr << "[标定] 相机物理坐标生成失败" << std::endl;
    }

    result.valid = true;
    return result;
}

std::vector<cv::Point2d> generateCameraPhysicalCoords(const std::vector<cv::Point2d>& robot_pts) {
    // 通过对 robot_pts 排序/分组来确定真实的网格行列结构，
    // 按输入顺序返回对应的相机物理坐标（轴对齐，原点在左上角网格点）。
    // 相机物理坐标系：原点 = (0,0)，X 向右，Y 向下。
    int n = static_cast<int>(robot_pts.size());
    if (n < 2) return {};

    // 1. 按 Y 降序、X 升序排序（适合典型机器人坐标系：Y 向上，X 向右）
    std::vector<int> sorted_idx(n);
    for (int i = 0; i < n; ++i) sorted_idx[i] = i;
    std::sort(sorted_idx.begin(), sorted_idx.end(), [&](int a, int b) {
        if (std::abs(robot_pts[a].y - robot_pts[b].y) > 1e-3)
            return robot_pts[a].y > robot_pts[b].y;  // Y 降序：行从图像上方到下方
        return robot_pts[a].x < robot_pts[b].x;       // X 升序：从左到右
    });

    // 2. 自适应行分组：相邻 Y 差大于阈值则视为不同行
    //    通过分析 Y 差异分布（双峰：行内微小差异 vs 行间大间距）自动确定阈值。
    std::vector<double> y_diffs;
    for (int i = 1; i < n; ++i) {
        double d = std::abs(robot_pts[sorted_idx[i]].y - robot_pts[sorted_idx[i - 1]].y);
        y_diffs.push_back(d);
    }
    double y_thresh = 10.0;  // 默认阈值
    if (!y_diffs.empty()) {
        double sum = 0, max_d = 0;
        for (double d : y_diffs) { sum += d; if (d > max_d) max_d = d; }
        double mean = sum / y_diffs.size();

        if (max_d > mean * 2.0) {
            // 存在远超均值的大间距 → 双峰分布（行内 + 行间），取均值的一半
            y_thresh = mean * 0.5;
        } else {
            // 所有差异相近 → 单行或无明确分界，阈值设为最大差异 +1（全部归一行）
            y_thresh = max_d + 1.0;
        }
    }

    // 分组
    std::vector<std::vector<int>> row_groups;  // 每个元素是 sorted_idx 中的 index
    for (int i = 0; i < n; ) {
        std::vector<int> row;
        row.push_back(sorted_idx[i]);
        int j = i + 1;
        while (j < n && std::abs(robot_pts[sorted_idx[j]].y - robot_pts[sorted_idx[i]].y) < y_thresh) {
            row.push_back(sorted_idx[j]);
            j++;
        }
        // 行内按 X 升序排序
        std::sort(row.begin(), row.end(), [&](int a, int b) {
            return robot_pts[a].x < robot_pts[b].x;
        });
        row_groups.push_back(std::move(row));
        i = j;
    }

    int grid_rows = static_cast<int>(row_groups.size());
    int grid_cols = 0;
    for (const auto& r : row_groups)
        grid_cols = std::max(grid_cols, static_cast<int>(r.size()));

    // 3. 从排序后的网格计算列间距 dx 和行间距 dy
    double sum_dx = 0, sum_dy = 0;
    int count_dx = 0, count_dy = 0;

    for (int r = 0; r < grid_rows; ++r) {
        for (int c = 0; c < static_cast<int>(row_groups[r].size()) - 1; ++c) {
            sum_dx += cv::norm(robot_pts[row_groups[r][c + 1]] - robot_pts[row_groups[r][c]]);
            count_dx++;
        }
    }
    for (int r = 0; r < grid_rows - 1; ++r) {
        int ncols = std::min(static_cast<int>(row_groups[r].size()),
                             static_cast<int>(row_groups[r + 1].size()));
        for (int c = 0; c < ncols; ++c) {
            sum_dy += cv::norm(robot_pts[row_groups[r + 1][c]] - robot_pts[row_groups[r][c]]);
            count_dy++;
        }
    }

    double dx = count_dx > 0 ? sum_dx / count_dx : 30.0;
    double dy = count_dy > 0 ? sum_dy / count_dy : 30.0;

    // 4. 建立 原始索引 → (row, col) 映射，然后按原始顺序生成 cam_pts
    std::vector<std::pair<int, int>> grid_pos(n, {-1, -1});
    for (int r = 0; r < grid_rows; ++r) {
        for (int c = 0; c < static_cast<int>(row_groups[r].size()); ++c) {
            int orig_idx = row_groups[r][c];
            grid_pos[orig_idx] = {r, c};
        }
    }

    std::vector<cv::Point2d> coords(n);
    for (int i = 0; i < n; ++i) {
        int r = grid_pos[i].first;
        int c = grid_pos[i].second;
        coords[i] = cv::Point2d(c * dx, r * dy);
    }

    return coords;
}

cv::Point2d pixelToRobot(const cv::Point2d& pixel, const cv::Mat& affine_matrix) {
    if (affine_matrix.empty())
        return cv::Point2d(0, 0);
    cv::Mat pt = (cv::Mat_<double>(3, 1) << pixel.x, pixel.y, 1.0);
    cv::Mat transformed = affine_matrix * pt;
    return cv::Point2d(transformed.at<double>(0, 0), transformed.at<double>(1, 0));
}

cv::Mat loadAffineMatrix(const std::string& path) {
    cv::FileStorage fs(path, cv::FileStorage::READ);
    if (!fs.isOpened()) {
        std::cerr << "[标定] 无法打开仿射矩阵文件: " << path << std::endl;
        return cv::Mat();
    }
    cv::Mat affine;
    fs["affine_matrix"] >> affine;
    fs.release();

    if (affine.empty()) {
        std::cerr << "[标定] 文件中未找到仿射矩阵: " << path << std::endl;
    } else if (affine.rows != 2 || affine.cols != 3) {
        std::cerr << "[标定] 仿射矩阵维度异常 (" << affine.rows << "x" << affine.cols
                  << ")，期望 2x3" << std::endl;
        return cv::Mat();
    }
    return affine;
}

void transformObjectList(ObjectInfoList& list, const cv::Mat& affine_matrix) {
    if (affine_matrix.empty()) return;
    for (auto& obj : list.getObjects()) {
        cv::Point2d phys = pixelToRobot(cv::Point2d(obj.getX(), obj.getY()), affine_matrix);
        obj.setX(phys.x);
        obj.setY(phys.y);
    }
}

bool solveTCPOffset(const std::vector<FlangePose>& input_poses,
                    const CalibConfig& cfg,
                    TCPCalibResult& result) {
    if ((int)input_poses.size() < cfg.min_poses) {
        std::cerr << "[TCP标定] 位姿数据不足,至少需要 " << cfg.min_poses << " 组" << std::endl;
        return false;
    }

    std::vector<FlangePose> poses = input_poses;
    std::vector<int> original_indices;
    for (size_t i = 0; i < poses.size(); ++i) original_indices.push_back((int)i);

    const int max_iterations = 3;
    const double outlier_factor = 2.5;

    for (int iter = 0; iter < max_iterations; ++iter) {
        double min_theta = poses[0].theta;
        double max_theta = poses[0].theta;
        for (const auto& p : poses) {
            if (p.theta < min_theta) min_theta = p.theta;
            if (p.theta > max_theta) max_theta = p.theta;
        }
        double angle_range = max_theta - min_theta;

        int n = (int)poses.size() - 1;
        cv::Mat A(2 * n, 2, CV_64F);
        cv::Mat b(2 * n, 1, CV_64F);

        double theta0 = poses[0].theta * CV_PI / 180.0;
        double cos0 = std::cos(theta0);
        double sin0 = std::sin(theta0);

        for (int i = 0; i < n; ++i) {
            double theta_i = poses[i + 1].theta * CV_PI / 180.0;
            double cos_i = std::cos(theta_i);
            double sin_i = std::sin(theta_i);

            double dc = cos_i - cos0;
            double ds = sin_i - sin0;

            A.at<double>(2 * i, 0) = dc;
            A.at<double>(2 * i, 1) = -ds;
            A.at<double>(2 * i + 1, 0) = ds;
            A.at<double>(2 * i + 1, 1) = dc;

            b.at<double>(2 * i, 0) = poses[0].x - poses[i + 1].x;
            b.at<double>(2 * i + 1, 0) = poses[0].y - poses[i + 1].y;
        }

        cv::Mat x;
        bool ok = cv::solve(A, b, x, cv::DECOMP_SVD);
        if (!ok) {
            std::cerr << "[TCP标定] 最小二乘求解失败" << std::endl;
            return false;
        }

        double dx = x.at<double>(0, 0);
        double dy = x.at<double>(1, 0);

        std::vector<cv::Point2d> tcp_points;
        for (const auto& p : poses) {
            double theta_rad = p.theta * CV_PI / 180.0;
            double tcp_x = p.x + dx * std::cos(theta_rad) - dy * std::sin(theta_rad);
            double tcp_y = p.y + dx * std::sin(theta_rad) + dy * std::cos(theta_rad);
            tcp_points.push_back(cv::Point2d(tcp_x, tcp_y));
        }

        cv::Point2d tcp_mean(0, 0);
        for (const auto& pt : tcp_points) {
            tcp_mean += pt;
        }
        tcp_mean.x /= tcp_points.size();
        tcp_mean.y /= tcp_points.size();

        std::vector<double> residuals(tcp_points.size());
        double total_error = 0;
        double max_error = 0;
        for (size_t i = 0; i < tcp_points.size(); ++i) {
            residuals[i] = std::sqrt(std::pow(tcp_points[i].x - tcp_mean.x, 2) +
                                     std::pow(tcp_points[i].y - tcp_mean.y, 2));
            total_error += residuals[i];
            if (residuals[i] > max_error) max_error = residuals[i];
        }
        double mean_error = total_error / tcp_points.size();

        double outlier_threshold = mean_error * outlier_factor;
        std::vector<FlangePose> filtered_poses;
        std::vector<int> filtered_indices;
        bool removed = false;
        for (size_t i = 0; i < residuals.size(); ++i) {
            if (residuals[i] > outlier_threshold && (int)poses.size() - (int)filtered_poses.size() > cfg.min_poses) {
                removed = true;
            } else {
                filtered_poses.push_back(poses[i]);
                filtered_indices.push_back(original_indices[i]);
            }
        }

        if (!removed || (int)filtered_poses.size() < cfg.min_poses) {
            if (angle_range < cfg.min_angle_range) {
                std::cerr << "[TCP标定] 警告: J4角度范围(" << angle_range
                          << "度)小于阈值(" << cfg.min_angle_range
                          << "度),求解可能不稳定" << std::endl;
            }

            result.dx = dx;
            result.dy = dy;
            result.dtheta = 0.0;
            result.tcp_world_x = tcp_mean.x;
            result.tcp_world_y = tcp_mean.y;
            result.position_mean_error = mean_error;
            result.position_max_error = max_error;
            result.num_poses = (int)poses.size();
            result.angle_range = angle_range;

            return true;
        }

        poses = filtered_poses;
        original_indices = filtered_indices;
    }

    std::cerr << "[TCP标定] 离群点剔除迭代次数超限" << std::endl;
    return false;
}

double calibrateRotationOffsetMethodA(double theta_flange, double theta_ref) {
    return theta_ref - theta_flange;
}

double calibrateRotationOffsetMethodB(double theta_flange,
                                      cv::Point2d refA,
                                      cv::Point2d refB) {
    double theta_ref = std::atan2(refB.y - refA.y, refB.x - refA.x) * 180.0 / CV_PI;
    return theta_ref - theta_flange;
}

double calibrateRotationOffsetMethodC(double dx, double dy, bool leftSide) {
    double a = std::atan2(dy, dx) * 180.0 / CV_PI;
    if (leftSide) {
        return a + 90.0;
    } else {
        return a - 90.0;
    }
}

bool saveTCPCalibResult(const TCPCalibResult& result, const std::string& output_path) {
    cv::FileStorage fs(output_path, cv::FileStorage::WRITE);
    if (!fs.isOpened()) {
        std::cerr << "[TCP标定] 无法保存标定结果: " << output_path << std::endl;
        return false;
    }

    fs << "tcp_dx" << result.dx;
    fs << "tcp_dy" << result.dy;
    fs << "tcp_dtheta" << result.dtheta;
    fs << "tcp_world_x" << result.tcp_world_x;
    fs << "tcp_world_y" << result.tcp_world_y;
    fs << "position_mean_error" << result.position_mean_error;
    fs << "position_max_error" << result.position_max_error;
    fs << "num_poses" << result.num_poses;
    fs << "angle_range" << result.angle_range;
    fs.release();

    return true;
}

bool saveAffineResult(const AffineResult& result, const std::string& output_path) {
    if (!result.valid) {
        std::cerr << "[标定] 仿射结果无效,无法保存" << std::endl;
        return false;
    }

    cv::FileStorage fs(output_path, cv::FileStorage::WRITE);
    if (!fs.isOpened()) {
        std::cerr << "[标定] 无法保存标定结果: " << output_path << std::endl;
        return false;
    }
    fs << "affine_matrix" << result.affine_robot;
    fs << "affine_robot" << result.affine_robot;
    fs << "affine_camera" << result.affine_camera;
    fs << "reprojection_error" << result.mean_error;
    fs << "max_reprojection_error" << result.max_error;
    fs.release();

    return true;
}

bool detectCheckerboard(const cv::Mat& image,
                        const cv::Size& board_size,
                        std::vector<cv::Point2f>& corners) {
    cv::Mat gray;
    if (image.channels() == 3) {
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = image.clone();
    }

    bool found = cv::findChessboardCorners(gray, board_size, corners,
        cv::CALIB_CB_ADAPTIVE_THRESH + cv::CALIB_CB_NORMALIZE_IMAGE + cv::CALIB_CB_FAST_CHECK);

    if (found) {
        cv::cornerSubPix(gray, corners, cv::Size(11, 11), cv::Size(-1, -1),
            cv::TermCriteria(cv::TermCriteria::EPS + cv::TermCriteria::MAX_ITER, 30, 0.1));
    }

    return found;
}

IntrinsicCalibResult calibrateIntrinsic(
    const std::vector<std::string>& image_paths,
    const cv::Size& board_size,
    double square_size) {

    IntrinsicCalibResult result;

    std::vector<std::vector<cv::Point2f>> all_corners;
    std::vector<std::vector<cv::Point3f>> all_object_points;
    int img_width = 0, img_height = 0;

    std::vector<cv::Point3f> obj_pts;
    for (int r = 0; r < board_size.height; ++r) {
        for (int c = 0; c < board_size.width; ++c) {
            obj_pts.push_back(cv::Point3f(c * square_size, r * square_size, 0.0));
        }
    }

    for (const auto& path : image_paths) {
        cv::Mat img = cv::imread(path, cv::IMREAD_COLOR);
        if (img.empty()) {
            std::cerr << "[内参标定] 无法读取图片: " << path << std::endl;
            continue;
        }

        std::vector<cv::Point2f> corners;
        if (!detectCheckerboard(img, board_size, corners)) {
            std::cerr << "[内参标定] 未检测到棋盘格: " << path << std::endl;
            continue;
        }

        all_corners.push_back(corners);
        all_object_points.push_back(obj_pts);

        if (img_width == 0) {
            img_width = img.cols;
            img_height = img.rows;
        }
    }

    if (all_corners.size() < 3) {
        std::cerr << "[内参标定] 有效图片不足3张，无法标定" << std::endl;
        return result;
    }

    cv::Mat camera_matrix = cv::Mat::eye(3, 3, CV_64F);
    cv::Mat dist_coeffs = cv::Mat::zeros(5, 1, CV_64F);

    std::vector<cv::Mat> rvecs, tvecs;
    double rms = cv::calibrateCamera(all_object_points, all_corners,
                                     cv::Size(img_width, img_height),
                                     camera_matrix, dist_coeffs, rvecs, tvecs);

    result.camera_matrix = camera_matrix;
    result.dist_coeffs = dist_coeffs;
    result.rvecs = rvecs;
    result.tvecs = tvecs;
    result.rms_error = rms;
    result.image_count = static_cast<int>(all_corners.size());
    result.image_width = img_width;
    result.image_height = img_height;
    result.valid = true;

    std::cout << "[内参标定] 标定完成, RMS=" << rms
              << ", 使用图片=" << all_corners.size() << "张" << std::endl;

    return result;
}

bool saveIntrinsicCalibResult(const IntrinsicCalibResult& result,
                              const std::string& output_path) {
    if (!result.valid) {
        std::cerr << "[内参标定] 结果无效,无法保存" << std::endl;
        return false;
    }

    cv::FileStorage fs(output_path, cv::FileStorage::WRITE);
    if (!fs.isOpened()) {
        std::cerr << "[内参标定] 无法保存标定结果: " << output_path << std::endl;
        return false;
    }

    fs << "image_width" << result.image_width;
    fs << "image_height" << result.image_height;
    fs << "camera_matrix" << result.camera_matrix;
    fs << "distortion_coefficients" << result.dist_coeffs;
    fs << "rms_error" << result.rms_error;
    fs << "image_count" << result.image_count;
    fs.release();

    return true;
}

bool loadIntrinsicCalibResult(const std::string& input_path,
                              IntrinsicCalibResult& result) {
    cv::FileStorage fs(input_path, cv::FileStorage::READ);
    if (!fs.isOpened()) {
        std::cerr << "[内参标定] 无法打开标定文件: " << input_path << std::endl;
        return false;
    }

    fs["image_width"] >> result.image_width;
    fs["image_height"] >> result.image_height;
    fs["camera_matrix"] >> result.camera_matrix;
    fs["distortion_coefficients"] >> result.dist_coeffs;

    if (!fs["rms_error"].empty()) result.rms_error = (double)fs["rms_error"];
    if (!fs["image_count"].empty()) result.image_count = (int)fs["image_count"];

    fs.release();

    if (result.camera_matrix.empty() || result.dist_coeffs.empty()) {
        std::cerr << "[内参标定] 标定文件缺少内参或畸变系数" << std::endl;
        return false;
    }

    result.valid = true;
    return true;
}

ConveyorCalibResult calibrateConveyor(
    const cv::Mat& image,
    const cv::Size& board_size,
    double square_size,
    int origin_mode,
    const cv::Mat& camera_matrix,
    const cv::Mat& dist_coeffs) {

    ConveyorCalibResult result;

    std::vector<cv::Point2f> corners;
    if (!detectCheckerboard(image, board_size, corners)) {
        std::cerr << "[传送带标定] 未检测到棋盘格" << std::endl;
        return result;
    }

    std::vector<cv::Point2f> image_points = corners;

    if (!camera_matrix.empty() && !dist_coeffs.empty()) {
        cv::undistortPoints(image_points, image_points, camera_matrix, dist_coeffs,
                           camera_matrix);
    }

    int rows = board_size.height;
    int cols = board_size.width;

    cv::Point2f p00 = corners[0];
    cv::Point2f p0c = corners[cols - 1];
    cv::Point2f pr0 = corners[(rows - 1) * cols];
    cv::Point2f prc = corners[rows * cols - 1];

    cv::Point2f center = (p00 + p0c + pr0 + prc) * 0.25;

    cv::Point2f oc_pts[4] = {p00, p0c, pr0, prc};
    int oc_visual[4];
    for (int i = 0; i < 4; ++i) {
        bool top = oc_pts[i].y < center.y;
        bool left = oc_pts[i].x < center.x;
        if (top && left) oc_visual[i] = 0;
        else if (top && !left) oc_visual[i] = 1;
        else if (!top && left) oc_visual[i] = 2;
        else oc_visual[i] = 3;
    }

    int selected_oc = 0;
    for (int i = 0; i < 4; ++i) {
        if (oc_visual[i] == origin_mode) {
            selected_oc = i;
            break;
        }
    }

    bool row_reverse = (selected_oc == 2 || selected_oc == 3);
    bool col_reverse = (selected_oc == 1 || selected_oc == 3);

    std::vector<cv::Point2f> ordered_image_points(image_points.size());
    std::vector<cv::Point2f> world_points(image_points.size());

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            int src_r = row_reverse ? (rows - 1 - r) : r;
            int src_c = col_reverse ? (cols - 1 - c) : c;
            int src_idx = src_r * cols + src_c;

            int dst_idx = r * cols + c;
            if (dst_idx < static_cast<int>(ordered_image_points.size()) && src_idx < static_cast<int>(image_points.size())) {
                ordered_image_points[dst_idx] = image_points[src_idx];
            }
            if (dst_idx < static_cast<int>(world_points.size())) {
                world_points[dst_idx] = cv::Point2f(c * square_size, r * square_size);
            }
        }
    }

    cv::Mat H = cv::findHomography(ordered_image_points, world_points, cv::LMEDS);
    if (H.empty()) {
        std::cerr << "[传送带标定] 单应性矩阵计算失败" << std::endl;
        return result;
    }

    double total_error = 0;
    int n = static_cast<int>(ordered_image_points.size());
    for (int i = 0; i < n; ++i) {
        cv::Mat pt = (cv::Mat_<double>(3, 1) << ordered_image_points[i].x, ordered_image_points[i].y, 1.0);
        cv::Mat mapped = H * pt;
        double wx = mapped.at<double>(0, 0) / mapped.at<double>(2, 0);
        double wy = mapped.at<double>(1, 0) / mapped.at<double>(2, 0);
        double dx = wx - world_points[i].x;
        double dy = wy - world_points[i].y;
        total_error += std::sqrt(dx * dx + dy * dy);
    }
    double rms = std::sqrt(total_error * total_error / n);

    cv::Mat H_inv = H.inv();

    cv::Mat origin_pt = (cv::Mat_<double>(3, 1) << ordered_image_points[0].x, ordered_image_points[0].y, 1.0);
    cv::Mat origin_world = H * origin_pt;
    double ox = origin_world.at<double>(0, 0) / origin_world.at<double>(2, 0);
    double oy = origin_world.at<double>(1, 0) / origin_world.at<double>(2, 0);

    double direction = 0.0;
    if (cols >= 2) {
        cv::Mat end_pt = (cv::Mat_<double>(3, 1) << ordered_image_points[1].x,
                          ordered_image_points[1].y, 1.0);
        cv::Mat end_world = H * end_pt;
        double ex = end_world.at<double>(0, 0) / end_world.at<double>(2, 0);
        double ey = end_world.at<double>(1, 0) / end_world.at<double>(2, 0);
        direction = std::atan2(ey - oy, ex - ox) * 180.0 / CV_PI;
    }

    result.H_pixel_to_world = H;
    result.H_world_to_pixel = H_inv;
    result.conveyor_origin = (cv::Mat_<double>(2, 1) << ox, oy);
    result.conveyor_direction = direction;
    result.rms_error = rms;
    result.valid = true;
    result.origin_mode = origin_mode;

    std::cout << "[传送带标定] 标定完成, RMS=" << rms
              << ", 方向=" << direction << "度"
              << ", 原点模式=" << origin_mode << std::endl;

    return result;
}

cv::Point2d pixelToConveyorWorld(const cv::Point2d& pixel,
                                  const ConveyorCalibResult& result) {
    if (!result.valid || result.H_pixel_to_world.empty()) {
        return cv::Point2d(0, 0);
    }
    cv::Mat pt = (cv::Mat_<double>(3, 1) << pixel.x, pixel.y, 1.0);
    cv::Mat mapped = result.H_pixel_to_world * pt;
    return cv::Point2d(mapped.at<double>(0, 0) / mapped.at<double>(2, 0),
                       mapped.at<double>(1, 0) / mapped.at<double>(2, 0));
}

cv::Point2d conveyorWorldToPixel(const cv::Point2d& world,
                                  const ConveyorCalibResult& result) {
    if (!result.valid || result.H_world_to_pixel.empty()) {
        return cv::Point2d(0, 0);
    }
    cv::Mat pt = (cv::Mat_<double>(3, 1) << world.x, world.y, 1.0);
    cv::Mat mapped = result.H_world_to_pixel * pt;
    return cv::Point2d(mapped.at<double>(0, 0) / mapped.at<double>(2, 0),
                       mapped.at<double>(1, 0) / mapped.at<double>(2, 0));
}

bool saveConveyorCalibResult(const ConveyorCalibResult& result,
                              const std::string& output_path) {
    if (!result.valid) {
        std::cerr << "[传送带标定] 结果无效,无法保存" << std::endl;
        return false;
    }

    cv::FileStorage fs(output_path, cv::FileStorage::WRITE);
    if (!fs.isOpened()) {
        std::cerr << "[传送带标定] 无法保存标定结果: " << output_path << std::endl;
        return false;
    }

    fs << "H_pixel_to_world" << result.H_pixel_to_world;
    fs << "H_world_to_pixel" << result.H_world_to_pixel;
    fs << "conveyor_origin" << result.conveyor_origin;
    fs << "conveyor_direction" << result.conveyor_direction;
    fs << "rms_error" << result.rms_error;
    fs.release();

    return true;
}

bool loadConveyorCalibResult(const std::string& input_path,
                              ConveyorCalibResult& result) {
    cv::FileStorage fs(input_path, cv::FileStorage::READ);
    if (!fs.isOpened()) {
        std::cerr << "[传送带标定] 无法打开标定文件: " << input_path << std::endl;
        return false;
    }

    fs["H_pixel_to_world"] >> result.H_pixel_to_world;
    fs["H_world_to_pixel"] >> result.H_world_to_pixel;
    fs["conveyor_origin"] >> result.conveyor_origin;

    if (!fs["conveyor_direction"].empty()) result.conveyor_direction = (double)fs["conveyor_direction"];
    if (!fs["rms_error"].empty()) result.rms_error = (double)fs["rms_error"];

    fs.release();

    if (result.H_pixel_to_world.empty()) {
        std::cerr << "[传送带标定] 标定文件缺少单应性矩阵" << std::endl;
        return false;
    }

    result.valid = true;
    return true;
}

#include <cstdio>
#include <algorithm>
#include <base/Angle.hpp>
#include "base/MathUtil.hpp"
#include "SonarHolder.hpp"

namespace sonar_target_tracking {

SonarHolder::SonarHolder()
    : beam_width_(0.0)
    , bin_count_(0)
    , beam_count_(0)
    , cart_size_(0, 0)
    , cart_origin_(0.0, 0.0)
    , total_bins_(0)
    , interpolation_type_(WEIGHTED)
{
}

SonarHolder::SonarHolder(
    std::vector<float> bins,
    float start_beam,
    float beam_width,
    uint32_t bin_count,
    uint32_t beam_count,
    int interpolation_type)
    : cart_size_(0, 0)
    , cart_origin_(0.0, 0.0)
    , total_bins_(0)
    , interpolation_type_(interpolation_type)
{
    Reset(bins, start_beam, beam_width, bin_count, beam_count);
}

SonarHolder::SonarHolder(
    std::vector<float> bins,
    std::vector<float> bearings,
    float beam_width,
    uint32_t bin_count,
    uint32_t beam_count,
    int interpolation_type)
    : cart_size_(0, 0)
    , cart_origin_(0.0, 0.0)
    , total_bins_(0)
    , interpolation_type_(interpolation_type)
{
    Reset(bins, bearings, beam_width, bin_count, beam_count);
}

SonarHolder::~SonarHolder() {
}

void SonarHolder::Reset(
    std::vector<float> bins,
    std::vector<float> bearings,
    float beam_width,
    uint32_t bin_count,
    uint32_t beam_count)
{
    bool is_initialize = (bin_count == bin_count_ && beam_count == beam_count_);

    bins_ = bins;
    bearings_ = bearings;
    bin_count_ = bin_count;
    beam_count_ = beam_count;
    beam_width_ = beam_width;

    if (!is_initialize) Initialize();

    InitializeCartesianImage();
}

void SonarHolder::Reset(
    std::vector<float> bins,
    float start_beam,
    float beam_width,
    uint32_t bin_count,
    uint32_t beam_count)
{
    Reset(bins, BuildBeamBearings(start_beam, beam_width, beam_count), beam_width, bin_count, beam_count);
}

std::vector<float> SonarHolder::BuildBeamBearings(float start_beam, float beam_width, uint32_t beam_count) {
    std::vector<float> bearings;
    bearings.resize(beam_count, 0);
    float interval = beam_width / beam_count;
    float angle = start_beam;
    for (uint32_t beam = 0; beam <= beam_count; beam++, angle += interval) bearings[beam] = angle;
    return bearings;
}

void SonarHolder::Initialize() {
    total_bins_ = bin_count_ * beam_count_;
    cart_size_ = cv::Size(cos(beam_width_ - M_PI_2) * bin_count_ * 2.0, bin_count_);
    cart_origin_ = cv::Point2f(cart_size_.width / 2, cart_size_.height - 1);
    InitializeCartesianPoints();
    InitializePolarMapping();
}

void SonarHolder::InitializeCartesianPoints() {
    cart_points_.assign(total_bins_, cv::Point2f(-1, -1));
    for (uint32_t bin = 0; bin < bin_count_; bin++) {
        for (uint32_t beam = 0; beam < beam_count_; beam++) {
            float radius = (bin == 0) ? 0.0001 : (float)bin;
            cart_points_[beam * bin_count_ + bin] = base::MathUtil::to_cartesianf(bearings_[beam], radius, -M_PI_2) + cart_origin_;
        }
    }
}

void SonarHolder::InitializePolarMapping() {
    cart_center_points_.assign(total_bins_, cv::Point2f(-1, -1));
    cart_to_polar_.assign(cart_size_.width * cart_size_.height, -1);
    radius_.assign(cart_size_.width * cart_size_.height, 0);
    angles_.assign(cart_size_.width * cart_size_.height, 0);

    for (uint32_t i = 0; i < total_bins_; i++) {
        SetCartesianToPolarSector(i);
    }
}

void SonarHolder::InitializeCartesianImage() {
    if (interpolation_type_ == LINEAR) {
        LinearPolarToCartesianImage(cart_image_);
    }
    else if (interpolation_type_ == WEIGHTED)  {
        WeightedPolarToCartesianImage(cart_image_);
    }
    else {
        throw std::invalid_argument("the interpolation type is invalid");
    }
}

void SonarHolder::LinearPolarToCartesianImage(cv::OutputArray _dst) {
    _dst.create(cart_size_, CV_32FC1);
    cv::Mat dst = _dst.getMat();
    dst.setTo(0);
    float *dst_ptr = reinterpret_cast<float*>(dst.data);
    for (size_t cart_idx = 0; cart_idx < cart_to_polar_.size(); cart_idx++) {
        if (cart_to_polar_[cart_idx] != -1) {
            *(dst_ptr + cart_idx) = bins_[cart_to_polar_[cart_idx]];
        }
    }
}

void SonarHolder::WeightedPolarToCartesianImage(cv::OutputArray _dst) {
    _dst.create(cart_size_, CV_32FC1);
    cv::Mat dst = _dst.getMat();
    dst.setTo(0);

    float *dst_ptr = reinterpret_cast<float*>(dst.data);

    for (size_t cart_idx = 0; cart_idx < cart_to_polar_.size(); cart_idx++) {

        if (cart_to_polar_[cart_idx] != -1) {

            int polar_idx = cart_to_polar_[cart_idx];

            int beam = polar_idx / bin_count_;
            int bin = polar_idx % bin_count_;

            if (beam < beam_count_-1 && bin < bin_count_-1) {
                float s0 = bins_[(beam+0)*bin_count_+bin+0];
                float s1 = bins_[(beam+0)*bin_count_+bin+1];
                float s2 = bins_[(beam+1)*bin_count_+bin+0];
                float s3 = bins_[(beam+1)*bin_count_+bin+1];

                float r0 = bin+0;
                float r1 = bin+1;
                float t0 = bearings_[beam+0];
                float t1 = bearings_[beam+1];

                float r = radius_[cart_idx];
                float t = angles_[cart_idx];

                float v0 = s0 + (s1 - s0) * (r - r0);
                float v1 = s2 + (s3 - s2) * (r - r0);
                float v = v0 + (v1 - v0) * (t - t0) / (t1 - t0);

                *(dst_ptr + cart_idx) = v;
            }
        }
    }
}

void SonarHolder::SetCartesianToPolarSector(uint32_t polar_idx) {
    uint32_t beam = polar_idx / bin_count_;
    uint32_t bin = polar_idx % bin_count_;
    if (beam >= beam_count_ - 1 || bin >= bin_count_ - 1) return;

    cv::Mat_<cv::Point2f> points(1, 4, cv::Point2f(0, 0));
    points(0) = cart_points_[(beam + 0) * bin_count_ + (bin + 0)];
    points(1) = cart_points_[(beam + 1) * bin_count_ + (bin + 1)];
    points(2) = cart_points_[(beam + 0) * bin_count_ + (bin + 1)];
    points(3) = cart_points_[(beam + 1) * bin_count_ + (bin + 0)];

    cv::Rect rc = cv::boundingRect(points);

    float r0 = bin;
    float r1 = bin+1;
    float t0 = bearings_[beam];
    float t1 = bearings_[beam+1];

    cart_center_points_[polar_idx] = base::MathUtil::to_cartesianf(t0 + (t1 - t0) / 2, r0 + (r1 - r0) / 2, -M_PI_2) + cart_origin_;

    for (uint32_t y = rc.tl().y; y <= rc.br().y && y < cart_size_.height; y++) {
        for (uint32_t x = rc.tl().x; x <= rc.br().x && x < cart_size_.width; x++) {
            size_t cart_idx = y * cart_size_.width + x;

            if (cart_to_polar_[cart_idx] == -1) {
                float dx = cart_origin_.x - x;
                float dy = cart_origin_.y - y;
                float r = sqrt(dx * dx + dy * dy);
                float t = atan2(dy, dx) - M_PI_2;

                radius_[cart_idx] = r;
                angles_[cart_idx] = t;

                if (r <= r1 && r >= r0 && t >= t0 && t <= t1) {
                    cart_to_polar_[cart_idx] = polar_idx;
                }
            }
        }
    }
}

int SonarHolder::GetMinAngleDistance(std::vector<float> angles, std::vector<int> indices, float alpha, int& angle_index) const {
    int index = -1;
    float min_dist = FLT_MAX;

    for (size_t i = 0; i < angles.size(); i++) {
        if (indices[i] != -1) {
            float dist = fabs(alpha - angles[i]);

            if (dist > M_PI) dist = fabs(dist - 2 * M_PI);

            if (dist < min_dist) {
                index = indices[i];
                min_dist = dist;
                angle_index = i;
            }
        }
    }

    return index;
}

void SonarHolder::GetNeighborhoodAngles(int origin_index, int index, std::vector<int>& neighbors_indices, std::vector<float>& angles, int neighbor_size) const {
    size_t total_neighbors = neighbor_size * neighbor_size;

    uint32_t beam = index / bin_count_;
    uint32_t bin = index % bin_count_;

    cv::Point2f point = cv::Point(-1, -1);
    cv::Point2f center_point = cart_center_points_[origin_index];

    neighbors_indices.assign(total_neighbors, -1);
    angles.assign(total_neighbors, 0.0);

    int j = 0;
    int neighbor_size_2 = neighbor_size / 2;
    for (int i = 0; i < total_neighbors; i++) {
        int x = (i % neighbor_size) - neighbor_size_2;
        int y = (i / neighbor_size) - neighbor_size_2;
        int bi = (beam+y < 0 || beam+y >= beam_count_) ? -1 : beam+y;
        int bj = (bin+x < 0 || bin+x >= bin_count_) ? -1 : bin+x;
        int idx = bi * bin_count_ + bj;

        if (bi != -1 && bj != -1 && idx != index) {
            point = cart_center_points_[idx];
            float dx = center_point.x - point.x;
            float dy = center_point.y - point.y;
            angles[i] = atan2(dy, dx);
            neighbors_indices[i] = idx;
        }
    }
}

void SonarHolder::GetNeighborhood(int polar_index, std::vector<int>& neighbors_indices, int neighbor_size) const {
    size_t total_neighbors = neighbor_size * neighbor_size;

    uint32_t beam = polar_index / bin_count_;
    uint32_t bin = polar_index % bin_count_;

    cv::Point2f point = cv::Point(-1, -1);
    neighbors_indices.assign(total_neighbors, -1);

    int j = 0;
    int neighbor_size_2 = neighbor_size / 2;

    for (int i = 0; i < total_neighbors; i++) {
        int x = (i % neighbor_size) - neighbor_size_2;
        int y = (i / neighbor_size) - neighbor_size_2;
        int bi = (beam+y < 0 || beam+y >= beam_count_) ? -1 : beam+y;
        int bj = (bin+x < 0 || bin+x >= bin_count_) ? -1 : bin+x;
        int idx = bi * bin_count_ + bj;

        if (bi != -1 && bj != -1 && idx != polar_index) {
            neighbors_indices[i] = idx;
        }
    }
}

std::vector<cv::Point2f> SonarHolder::GetSectorPoints(int polar_index) const {
    int beam = index_to_beam(polar_index);
    int bin = index_to_bin(polar_index);

    std::vector<cv::Point2f> points(4);

    points[0] = cart_points_[(beam + 0) * bin_count_ + (bin + 0)];
    points[1] = cart_points_[(beam + 1) * bin_count_ + (bin + 1)];
    points[2] = cart_points_[(beam + 0) * bin_count_ + (bin + 1)];
    points[3] = cart_points_[(beam + 1) * bin_count_ + (bin + 0)];

    return points;
}

void SonarHolder::GetPolarLimits(int polar_index, float& start_bin, float& final_bin, float& start_beam, float& final_beam) const {
    int bin = index_to_bin(polar_index);
    int beam = index_to_beam(polar_index);
    start_bin = float(bin+0);
    final_bin = float(bin+1);
    start_beam = bearings_[beam+0];
    final_beam = bearings_[beam+1];
}

} /* namespace sonar_target_tracking */

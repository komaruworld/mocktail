#ifndef MOCKTAIL_VR_POSE_MATH_H_
#define MOCKTAIL_VR_POSE_MATH_H_

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mocktail::vr::pose {

// Coordinate spaces and units used across the whole camera path, documented so
// every stage is auditable:
//
//   OpenXR head/views  metres, right-handed, +Y up, -Z forward, LOCAL space
//     -> published pose  metres, quaternion (x, y, z, w), unchanged units
//     -> guest state     metres; the exact-build native helper converts metres
//                        to studs itself (factor 10/3). This layer must never
//                        pre-apply that scale.
//     -> camera          engine composes
//                        out.rotation = base.rotation * head.rotation
//                        out.position = base.position
//                                       + base.rotation * head.position * HeadScale
//                        where HeadScale is an engine value distinct from the
//                        metres-to-studs conversion factor.
//     -> render          one pass per eye using the per-eye offset/FOV
//     -> XR submission   metres again, the very pose used to render the pair.

struct Vec3 {
  float x = 0.f, y = 0.f, z = 0.f;
};

struct Quat {
  float x = 0.f, y = 0.f, z = 0.f, w = 1.f;
};

struct Pose {
  Vec3 position{};
  Quat orientation{};
};

inline constexpr float kPi = 3.14159265358979323846f;

inline Quat QuatMultiply(const Quat& a, const Quat& b) {
  return Quat{
      a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
      a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
      a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
      a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
  };
}

inline Quat QuatConjugate(const Quat& q) { return Quat{-q.x, -q.y, -q.z, q.w}; }

inline float QuatNorm(const Quat& q) {
  return std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
}

// Returns false for a non-finite or near-zero quaternion; the output is left
// untouched in that case so callers never publish a fabricated rotation.
inline bool QuatNormalize(const Quat& in, Quat* out) {
  if (out == nullptr) {
    return false;
  }
  if (!std::isfinite(in.x) || !std::isfinite(in.y) || !std::isfinite(in.z) ||
      !std::isfinite(in.w)) {
    return false;
  }
  const float norm = QuatNorm(in);
  if (!(norm > 1e-6f) || !std::isfinite(norm)) {
    return false;
  }
  const float inverse = 1.f / norm;
  *out = Quat{in.x * inverse, in.y * inverse, in.z * inverse, in.w * inverse};
  return true;
}

// q and -q denote the same rotation. Used to prove the path is sign-invariant.
inline bool QuatSameRotation(const Quat& a, const Quat& b, float tolerance) {
  const float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
  return std::abs(std::abs(dot) - 1.f) <= tolerance;
}

inline Vec3 QuatRotate(const Quat& q, const Vec3& v) {
  // t = 2 * cross(q.xyz, v); v' = v + q.w * t + cross(q.xyz, t)
  const Vec3 u{q.x, q.y, q.z};
  const Vec3 cross1{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z,
                    u.x * v.y - u.y * v.x};
  const Vec3 t{2.f * cross1.x, 2.f * cross1.y, 2.f * cross1.z};
  const Vec3 cross2{u.y * t.z - u.z * t.y, u.z * t.x - u.x * t.z,
                    u.x * t.y - u.y * t.x};
  return Vec3{v.x + q.w * t.x + cross2.x, v.y + q.w * t.y + cross2.y,
              v.z + q.w * t.z + cross2.z};
}

// result = a * b in SE(3): apply b first, then a.
inline Pose PoseMultiply(const Pose& a, const Pose& b) {
  Pose result;
  result.orientation = QuatMultiply(a.orientation, b.orientation);
  const Vec3 rotated = QuatRotate(a.orientation, b.position);
  result.position = Vec3{a.position.x + rotated.x, a.position.y + rotated.y,
                         a.position.z + rotated.z};
  return result;
}

inline Pose PoseInverse(const Pose& p) {
  Pose result;
  result.orientation = QuatConjugate(p.orientation);
  const Vec3 neg{-p.position.x, -p.position.y, -p.position.z};
  result.position = QuatRotate(result.orientation, neg);
  return result;
}

// Rotation for a head turned by yaw about +Y then pitched about +X, matching
// the scripted diagnostic motion. Quaternion order is (x, y, z, w).
inline Quat QuatFromYawPitch(float yaw, float pitch) {
  const float cy = std::cos(yaw * 0.5f), sy = std::sin(yaw * 0.5f);
  const float cp = std::cos(pitch * 0.5f), sp = std::sin(pitch * 0.5f);
  return Quat{cy * sp, sy * cp, -sy * sp, cy * cp};
}

inline Quat QuatFromAxisAngle(const Vec3& axis, float angle) {
  const float half = angle * 0.5f;
  const float s = std::sin(half);
  return Quat{axis.x * s, axis.y * s, axis.z * s, std::cos(half)};
}

// Per-eye FOV as the runtime reports it: angleLeft, angleRight, angleUp,
// angleDown in radians, each signed relative to the eye's forward axis.
struct EyeFov {
  float angle_left = 0.f;
  float angle_right = 0.f;
  float angle_up = 0.f;
  float angle_down = 0.f;
};

// The guest state stores projection tangents, not angles. The 2998 layout is
// up, down, left, right in that order; down/left are negated because the
// guest's tangent space measures them in the opposite direction. Getting this
// order or a sign wrong flips or shears the image, so it is pinned by test.
struct ProjectionTangents {
  float up = 0.f;
  float down = 0.f;
  float left = 0.f;
  float right = 0.f;
};

inline ProjectionTangents FovToTangents(const EyeFov& fov) {
  return ProjectionTangents{std::tan(fov.angle_up), std::tan(-fov.angle_down),
                            std::tan(-fov.angle_left), std::tan(fov.angle_right)};
}

inline constexpr float kMaxEyeFovAngle = 1.56f;

// Enclose the display frustum in a head-parallel camera for the guest renderer.
// The compositor receives this rendered FOV and pose, not the canted display's.
inline bool ParallelEyeFov(const Quat& eye_orientation, const EyeFov& display,
                           EyeFov* rendered) {
  Quat orientation;
  if (!rendered || !QuatNormalize(eye_orientation, &orientation) ||
      !(display.angle_left < display.angle_right) ||
      !(display.angle_down < display.angle_up)) {
    return false;
  }
  const float angles[] = {display.angle_left, display.angle_right,
                          display.angle_up, display.angle_down};
  for (float angle : angles) {
    if (!std::isfinite(angle) || std::abs(angle) >= kMaxEyeFovAngle)
      return false;
  }
  if (orientation.x == 0.f && orientation.y == 0.f && orientation.z == 0.f) {
    *rendered = display;
    return true;
  }
  const float xs[] = {std::tan(display.angle_left),
                      std::tan(display.angle_right)};
  const float ys[] = {std::tan(display.angle_down), std::tan(display.angle_up)};
  float left = 0.f, right = 0.f, down = 0.f, up = 0.f;
  bool first = true;
  for (float x : xs) {
    for (float y : ys) {
      const Vec3 ray = QuatRotate(orientation, {x, y, -1.f});
      if (!std::isfinite(ray.z) || ray.z >= -1e-6f) return false;
      const float horizontal = ray.x / -ray.z;
      const float vertical = ray.y / -ray.z;
      if (!std::isfinite(horizontal) || !std::isfinite(vertical)) return false;
      if (first) {
        left = right = horizontal;
        down = up = vertical;
        first = false;
      } else {
        left = std::min(left, horizontal);
        right = std::max(right, horizontal);
        down = std::min(down, vertical);
        up = std::max(up, vertical);
      }
    }
  }
  const EyeFov result{std::atan(left), std::atan(right), std::atan(up),
                      std::atan(down)};
  const float bounds[] = {result.angle_left, result.angle_right,
                          result.angle_up, result.angle_down};
  for (float angle : bounds) {
    if (std::abs(angle) >= kMaxEyeFovAngle) return false;
  }
  *rendered = result;
  return true;
}

// Metres-to-studs factor the exact-build native helper applies itself. Exposed
// only so tests can prove this layer does NOT pre-apply it.
inline constexpr float kMetresToStuds = 10.f / 3.f;

// Expected stud value for a metre offset, for numerical unit checks.
inline float MetresToStuds(float metres) { return metres * kMetresToStuds; }

}  // namespace mocktail::vr::pose

#endif  // MOCKTAIL_VR_POSE_MATH_H_

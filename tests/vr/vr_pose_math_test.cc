#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "mocktail/vr/vr_pose_math.h"

namespace mocktail::vr {
namespace {

using namespace pose;

constexpr float kTol = 1e-5f;

Vec3 Forward() { return Vec3{0.f, 0.f, -1.f}; }  // OpenXR: -Z is forward
Vec3 Up() { return Vec3{0.f, 1.f, 0.f}; }
Vec3 Right() { return Vec3{1.f, 0.f, 0.f}; }

void ExpectVec(const Vec3& actual, const Vec3& expected, float tolerance) {
  EXPECT_NEAR(actual.x, expected.x, tolerance) << "x";
  EXPECT_NEAR(actual.y, expected.y, tolerance) << "y";
  EXPECT_NEAR(actual.z, expected.z, tolerance) << "z";
}

// ---- rotation axes verified independently, not only in combination ----

TEST(VrPoseMath, YawAloneRotatesForwardWithoutAxisInversion) {
  const float yaw = 0.6f;
  const Quat q = QuatFromYawPitch(yaw, 0.f);
  // Rotation about +Y by yaw takes forward (0,0,-1) to (-sin, 0, -cos).
  const Vec3 rotated = QuatRotate(q, Forward());
  ExpectVec(rotated, Vec3{-std::sin(yaw), 0.f, -std::cos(yaw)}, kTol);
  // Up is untouched by yaw: an inverted axis would move it.
  ExpectVec(QuatRotate(q, Up()), Up(), kTol);
}

TEST(VrPoseMath, PitchAloneRotatesForwardWithoutAxisInversion) {
  const float pitch = 0.4f;
  const Quat q = QuatFromYawPitch(0.f, pitch);
  // Rotation about +X by pitch takes (0,0,-1) to (0, sin, -cos): looking up
  // (positive pitch) raises the forward vector's Y, matching head tilt up.
  const Vec3 rotated = QuatRotate(q, Forward());
  ExpectVec(rotated, Vec3{0.f, std::sin(pitch), -std::cos(pitch)}, kTol);
  ExpectVec(QuatRotate(q, Right()), Right(), kTol);
}

TEST(VrPoseMath, RollAloneRotatesUpAroundForwardAxis) {
  const float roll = 0.35f;
  const Quat q = QuatFromAxisAngle(Vec3{0.f, 0.f, 1.f}, roll);
  // Right-handed rotation about +Z takes up (0,1,0) to (-sin, cos, 0): the
  // head tilting right rotates the world's up toward the viewer's left.
  const Vec3 rotated_up = QuatRotate(q, Up());
  ExpectVec(rotated_up, Vec3{-std::sin(roll), std::cos(roll), 0.f}, kTol);
  // The roll axis itself is invariant.
  ExpectVec(QuatRotate(q, Vec3{0.f, 0.f, 1.f}), Vec3{0.f, 0.f, 1.f}, kTol);
}

TEST(VrPoseMath, YawPitchCombinationMatchesIndependentComposition) {
  const float yaw = 0.5f, pitch = 0.3f;
  const Quat combined = QuatFromYawPitch(yaw, pitch);
  // Documented order: R = Ry(yaw) * Rx(pitch).
  const Quat yaw_only = QuatFromYawPitch(yaw, 0.f);
  const Quat pitch_only = QuatFromYawPitch(0.f, pitch);
  const Quat expected = QuatMultiply(yaw_only, pitch_only);
  EXPECT_TRUE(QuatSameRotation(combined, expected, kTol));
  // A forward vector must agree between both constructions.
  const Vec3 a = QuatRotate(combined, Forward());
  const Vec3 b = QuatRotate(expected, Forward());
  ExpectVec(a, b, kTol);
  // And against explicit sequential rotation: pitch first, then yaw.
  const Vec3 c = QuatRotate(yaw_only, QuatRotate(pitch_only, Forward()));
  ExpectVec(a, c, kTol);
}

TEST(VrPoseMath, PoseMultiplyAndInverseRoundTrip) {
  Pose a;
  a.position = Vec3{0.3f, -1.2f, 0.7f};
  a.orientation = QuatFromYawPitch(0.4f, -0.25f);
  const Pose identity = PoseMultiply(a, PoseInverse(a));
  ExpectVec(identity.position, Vec3{0.f, 0.f, 0.f}, kTol);
  EXPECT_TRUE(QuatSameRotation(identity.orientation, Quat{0.f, 0.f, 0.f, 1.f},
                               kTol));
}

TEST(VrPoseMath, EyeOffsetIsHeadInverseTimesRuntimeEye) {
  // The backend computes relative = inverse(head) * runtime_eye and reprojects
  // final_eye = head * relative, which must reproduce the runtime eye exactly.
  Pose head;
  head.position = Vec3{0.2f, 1.5f, -0.4f};
  head.orientation = QuatFromYawPitch(0.7f, 0.15f);
  Pose runtime_eye = head;
  runtime_eye.position = PoseMultiply(head, Pose{Vec3{-0.032f, 0.f, 0.f},
                                                 Quat{}}).position;
  const Pose relative = PoseMultiply(PoseInverse(head), runtime_eye);
  const Pose final_eye = PoseMultiply(head, relative);
  ExpectVec(final_eye.position, runtime_eye.position, kTol);
}

// ---- quaternion sign/normalization contract ----

TEST(VrPoseMath, QuaternionSignDoesNotChangeRotation) {
  const Quat q = QuatFromYawPitch(0.9f, -0.4f);
  const Quat negated{-q.x, -q.y, -q.z, -q.w};
  EXPECT_TRUE(QuatSameRotation(q, negated, kTol));
  ExpectVec(QuatRotate(q, Forward()), QuatRotate(negated, Forward()), kTol);
  const Pose p{Vec3{0.1f, 0.2f, 0.3f}, q};
  const Pose pn{Vec3{0.1f, 0.2f, 0.3f}, negated};
  const Pose other{Vec3{-0.5f, 1.f, 0.25f}, QuatFromAxisAngle(Up(), 0.3f)};
  const Pose a = PoseMultiply(p, other);
  const Pose b = PoseMultiply(pn, other);
  ExpectVec(a.position, b.position, kTol);
  EXPECT_TRUE(QuatSameRotation(a.orientation, b.orientation, kTol));
}

TEST(VrPoseMath, NormalizeRejectsNonfiniteAndNearZeroWithoutWritingOutput) {
  Quat out{9.f, 9.f, 9.f, 9.f};
  Quat input{0.f, 0.f, 0.f, 2.f};
  ASSERT_TRUE(QuatNormalize(input, &out));
  ExpectVec(Vec3{out.x, out.y, out.z}, Vec3{0.f, 0.f, 0.f}, kTol);
  EXPECT_NEAR(out.w, 1.f, kTol);

  const float nan = std::numeric_limits<float>::quiet_NaN();
  input = Quat{nan, 0.f, 0.f, 1.f};
  out = Quat{9.f, 9.f, 9.f, 9.f};
  EXPECT_FALSE(QuatNormalize(input, &out));
  EXPECT_FLOAT_EQ(out.x, 9.f);  // output untouched on rejection

  input = Quat{0.f, 0.f, 0.f, 0.f};
  EXPECT_FALSE(QuatNormalize(input, &out));
  EXPECT_FALSE(QuatNormalize(input, nullptr));
}

// ---- FOV order/sign contract (asymmetric, pinned to the 2998 guest layout) --

TEST(VrPoseMath, FovToTangentsPinsUpDownLeftRightOrderAndSigns) {
  EyeFov fov;
  fov.angle_left = -0.7f;
  fov.angle_right = 0.8f;
  fov.angle_up = 0.9f;
  fov.angle_down = -0.6f;
  const ProjectionTangents tangents = FovToTangents(fov);
  // The guest state block stores up, down, left, right in this order; down and
  // left are negated relative to the OpenXR angle signs. The existing bridge
  // test pins the same expectation through ApplyXrPose; this pins the math.
  EXPECT_FLOAT_EQ(tangents.up, std::tan(0.9f));
  EXPECT_FLOAT_EQ(tangents.down, std::tan(0.6f));
  EXPECT_FLOAT_EQ(tangents.left, std::tan(0.7f));
  EXPECT_FLOAT_EQ(tangents.right, std::tan(0.8f));
  // Asymmetric FOV must not collapse to a symmetric projection.
  EXPECT_NE(tangents.left, tangents.right);
  EXPECT_NE(tangents.up, tangents.down);
}

TEST(VrPoseMath, ParallelProjectionKeepsUnrotatedFovExact) {
  const EyeFov display{-.7f, .8f, .9f, -.6f};
  for (float sign : {1.f, -1.f}) {
    EyeFov rendered;
    ASSERT_TRUE(ParallelEyeFov({0, 0, 0, sign}, display, &rendered));
    EXPECT_EQ(rendered.angle_left, display.angle_left);
    EXPECT_EQ(rendered.angle_right, display.angle_right);
    EXPECT_EQ(rendered.angle_up, display.angle_up);
    EXPECT_EQ(rendered.angle_down, display.angle_down);
  }
}

TEST(VrPoseMath, ParallelProjectionContainsEveryCantedDisplayRay) {
  const EyeFov display{-.7f, .8f, .9f, -.6f};
  for (const Vec3 axis : {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}}) {
    for (float angle : {-.2f, .2f}) {
      const Quat rotation = QuatFromAxisAngle(axis, angle);
      EyeFov rendered;
      ASSERT_TRUE(ParallelEyeFov(rotation, display, &rendered));
      for (int x = 0; x <= 4; ++x) {
        for (int y = 0; y <= 4; ++y) {
          const Vec3 ray =
              QuatRotate(rotation, {std::tan(display.angle_left) +
                                        x * .25f *
                                            (std::tan(display.angle_right) -
                                             std::tan(display.angle_left)),
                                    std::tan(display.angle_down) +
                                        y * .25f *
                                            (std::tan(display.angle_up) -
                                             std::tan(display.angle_down)),
                                    -1});
          const float horizontal = std::atan2(ray.x, -ray.z);
          const float vertical = std::atan2(ray.y, -ray.z);
          EXPECT_GE(horizontal, rendered.angle_left - 1e-6f);
          EXPECT_LE(horizontal, rendered.angle_right + 1e-6f);
          EXPECT_GE(vertical, rendered.angle_down - 1e-6f);
          EXPECT_LE(vertical, rendered.angle_up + 1e-6f);
        }
      }
      const Pose head{{1, 2, 3}, QuatFromYawPitch(.4f, -.1f)};
      const Pose parallel_eye{{-.03f, .01f, .02f}, {}};
      const Pose submitted = PoseMultiply(head, parallel_eye);
      EXPECT_TRUE(
          QuatSameRotation(submitted.orientation, head.orientation, 1e-6f));
    }
  }
}

TEST(VrPoseMath, UnrepresentableProjectionDoesNotModifyOutput) {
  const EyeFov display{-.7f, .8f, .9f, -.6f};
  EyeFov rendered{1, 2, 3, 4};
  EXPECT_FALSE(ParallelEyeFov({0, 0, 0, 0}, display, &rendered));
  EXPECT_FALSE(ParallelEyeFov({}, {.8f, -.7f, .9f, -.6f}, &rendered));
  EXPECT_FALSE(
      ParallelEyeFov(QuatFromAxisAngle({0, 1, 0}, 1.5f), display, &rendered));
  EXPECT_FALSE(ParallelEyeFov({}, display, nullptr));
  EXPECT_EQ(rendered.angle_left, 1);
  EXPECT_EQ(rendered.angle_down, 4);
}

// ---- units: the 0.10 m translation check through the native conversion ----

TEST(VrPoseMath, TenthMetreConversionIsDocumentedAndNotPreApplied) {
  // The exact-build native helper converts metres to studs itself with factor
  // 10/3; 0.10 m must arrive in the guest state as metres (0.10) and become
  // 1/3 stud natively. This layer only documents the expected value.
  EXPECT_FLOAT_EQ(MetresToStuds(0.10f), 0.10f * kMetresToStuds);
  EXPECT_NEAR(MetresToStuds(0.10f), 1.0f / 3.0f, 1e-6f);
  // Engine HeadScale multiplies head.position again inside the camera
  // composition (out.position = base + base.rotation * head.position *
  // HeadScale); it is an engine property distinct from the 10/3 conversion and
  // was verified numerically with the native-camera runtime observer.
  EXPECT_NE(kMetresToStuds, 1.f);
}

}  // namespace
}  // namespace mocktail::vr

/**
 * @file tests/unit/gabs/test_cursor.cpp
 * @brief Gabs Remote: tests for the cursor drawn on the tablet (grCursor and /gabs/cursor).
 */

// test includes
#include "../../tests_common.h"

// standard includes
#include <chrono>
#include <memory>
#include <optional>
#include <vector>

// local includes
#include <src/gabs/cursor.h>

namespace gc = gabs::cursor;
using namespace std::chrono_literals;

namespace {
  struct fixture_t {
    gc::shape_e shape {gc::shape_e::arrow};
    int samples {0};
    std::vector<bool> video;
    std::unique_ptr<gc::tracker_t> tracker;

    fixture_t() {
      tracker = std::make_unique<gc::tracker_t>(
        [this]() {
          ++samples;
          return shape;
        },
        [this](bool on) {
          video.push_back(on);
        },
        0ms,
        10s
      );
    }
  };

  const auto t0 = std::chrono::steady_clock::time_point {} + 1h;
}  // namespace

TEST(GabsCursorTest, ParsesCursorChoice) {
  EXPECT_TRUE(gc::parse_cursor_choice("local"));
  EXPECT_FALSE(gc::parse_cursor_choice(""));
  EXPECT_FALSE(gc::parse_cursor_choice("Local"));
  EXPECT_FALSE(gc::parse_cursor_choice("video"));
}

TEST(GabsCursorTest, NamesShapes) {
  EXPECT_EQ(gc::shape_name(gc::shape_e::hidden), "hidden");
  EXPECT_EQ(gc::shape_name(gc::shape_e::arrow), "arrow");
  EXPECT_EQ(gc::shape_name(gc::shape_e::ibeam), "ibeam");
  EXPECT_EQ(gc::shape_name(gc::shape_e::hand), "hand");
  EXPECT_EQ(gc::shape_name(gc::shape_e::app_starting), "app_starting");
  EXPECT_EQ(gc::shape_name(gc::shape_e::size_nwse), "size_nwse");
  EXPECT_EQ(gc::shape_name(gc::shape_e::other), "other");
}

TEST(GabsCursorTest, FormatsReplyAndParsesKnownVersion) {
  EXPECT_EQ(gc::format_reply({7, gc::shape_e::ibeam}), "7 ibeam");
  EXPECT_EQ(gc::parse_known("12"), 12u);
  EXPECT_EQ(gc::parse_known(""), 0u);
  EXPECT_EQ(gc::parse_known("abc"), 0u);
  EXPECT_EQ(gc::parse_known("3x"), 0u);
}

TEST(GabsCursorTest, LocalSessionHidesTheCursorInTheVideoAndRestoreShowsIt) {
  fixture_t f;
  f.tracker->apply(true);
  EXPECT_TRUE(f.tracker->local());
  f.tracker->restore();
  EXPECT_FALSE(f.tracker->local());
  EXPECT_EQ(f.video, (std::vector<bool> {false, true}));
}

TEST(GabsCursorTest, LaunchWithoutGrCursorKeepsTheCursorInTheVideo) {
  fixture_t f;
  f.tracker->apply(true);
  f.tracker->apply(false);
  EXPECT_FALSE(f.tracker->local());
  EXPECT_EQ(f.video, (std::vector<bool> {false, true}));
}

TEST(GabsCursorTest, SamplesOnlyWhileLocal) {
  fixture_t f;
  f.tracker->tick(t0);
  EXPECT_EQ(f.samples, 0);
  f.tracker->apply(true);
  f.tracker->tick(t0);
  EXPECT_EQ(f.samples, 1);
}

TEST(GabsCursorTest, ShapeChangeBumpsTheVersion) {
  fixture_t f;
  f.tracker->apply(true);
  auto first = f.tracker->state();
  f.tracker->tick(t0);
  EXPECT_EQ(f.tracker->state().version, first.version);  // still the arrow
  f.shape = gc::shape_e::ibeam;
  f.tracker->tick(t0);
  EXPECT_EQ(f.tracker->state().version, first.version + 1);
  EXPECT_EQ(f.tracker->state().shape, gc::shape_e::ibeam);
}

TEST(GabsCursorTest, PollWithOldVersionRepliesAtOnce) {
  fixture_t f;
  std::optional<gc::state_t> got;
  f.tracker->poll(0, [&](const gc::state_t &s) {
    got = s;
  });
  ASSERT_TRUE(got);
  EXPECT_EQ(got->version, f.tracker->state().version);
  EXPECT_EQ(f.tracker->waiting(), 0u);
}

TEST(GabsCursorTest, PollWaitsForAShapeChange) {
  fixture_t f;
  f.tracker->apply(true);
  std::optional<gc::state_t> got;
  f.tracker->poll(f.tracker->state().version, [&](const gc::state_t &s) {
    got = s;
  });
  EXPECT_FALSE(got);
  EXPECT_EQ(f.tracker->waiting(), 1u);
  f.tracker->tick(t0);
  EXPECT_FALSE(got);  // same shape
  f.shape = gc::shape_e::hand;
  f.tracker->tick(t0);
  ASSERT_TRUE(got);
  EXPECT_EQ(got->shape, gc::shape_e::hand);
  EXPECT_EQ(f.tracker->waiting(), 0u);
}

TEST(GabsCursorTest, PollTimesOutWithTheCurrentState) {
  fixture_t f;
  f.tracker->apply(true);
  f.tracker->tick(t0);  // the waiter below starts after this moment
  std::optional<gc::state_t> got;
  f.tracker->poll(f.tracker->state().version, [&](const gc::state_t &s) {
    got = s;
  });
  f.tracker->tick(std::chrono::steady_clock::now() + 5s);
  EXPECT_FALSE(got);
  f.tracker->tick(std::chrono::steady_clock::now() + 11s);
  ASSERT_TRUE(got);
  EXPECT_EQ(got->shape, gc::shape_e::arrow);
}

TEST(GabsCursorTest, RestoreAnswersWaitingRequests) {
  fixture_t f;
  f.tracker->apply(true);
  std::optional<gc::state_t> got;
  f.tracker->poll(f.tracker->state().version, [&](const gc::state_t &s) {
    got = s;
  });
  f.tracker->restore();
  EXPECT_TRUE(got);
  EXPECT_EQ(f.tracker->waiting(), 0u);
}

TEST(GabsCursorTest, NullCallbacksAreSafe) {
  gc::tracker_t tracker {nullptr, nullptr};
  tracker.apply(true);
  tracker.tick(t0);
  EXPECT_EQ(tracker.state().shape, gc::shape_e::arrow);
  tracker.restore();
}

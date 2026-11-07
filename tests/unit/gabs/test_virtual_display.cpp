/**
 * @file tests/unit/gabs/test_virtual_display.cpp
 * @brief Gabs Remote: tests for the per-client virtual display (grDisplay).
 */

// test includes
#include "../../tests_common.h"

// standard includes
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>

// local includes
#include <src/config.h>
#include <src/gabs/virtual_display.h>
#include <thread>

namespace vd = gabs::virtual_display;

namespace {
  struct fake_counts_t {
    int add = 0;
    int remove = 0;
    int remove_all = 0;
    std::atomic<int> keep_alive = 0;
    std::chrono::milliseconds add_delay {0};
    std::atomic<int> keep_alive_during_add = 0;
    bool adding = false;
  };

  class fake_driver_t: public vd::driver_t {
  public:
    fake_driver_t(std::shared_ptr<fake_counts_t> counts, bool fail_add = false):
        counts {std::move(counts)},
        fail_add {fail_add} {
    }

    bool available() override {
      return true;
    }

    std::optional<std::string> add(const std::string &, const vd::mode_t &) override {
      ++counts->add;
      if (counts->add_delay.count() > 0) {
        counts->adding = true;
        const auto before {counts->keep_alive.load()};
        std::this_thread::sleep_for(counts->add_delay);
        counts->keep_alive_during_add = counts->keep_alive.load() - before;
        counts->adding = false;
      }
      if (fail_add) {
        return std::nullopt;
      }
      return std::string {R"(\\.\DISPLAY22)"};
    }

    bool remove(const std::string &) override {
      ++counts->remove;
      return true;
    }

    void remove_all() override {
      ++counts->remove_all;
    }

    void keep_alive() override {
      ++counts->keep_alive;
    }

  private:
    std::shared_ptr<fake_counts_t> counts;
    bool fail_add;
  };

  constexpr auto DEVICE_ID {"{gabs-virtual-device-id}"};

  std::atomic<bool> fake_device_present {true};

  std::optional<std::string> fake_device_id_for(const std::string &display_name) {
    if (fake_device_present && display_name == R"(\\.\DISPLAY22)") {
      return std::string {DEVICE_ID};
    }
    return std::nullopt;
  }

  std::optional<std::string> no_device_id_for(const std::string &) {
    return std::nullopt;
  }

  /**
   * @brief Installs a manager with a fake driver for the test and restores a driverless one after.
   */
  class VirtualDisplayFixture: public ::testing::Test {
  protected:
    std::shared_ptr<fake_counts_t> counts {std::make_shared<fake_counts_t>()};

    void install_fake(bool fail_add = false, bool resolve_device = true) {
      vd::install(std::make_unique<vd::manager_t>(
        std::make_unique<fake_driver_t>(counts, fail_add),
        resolve_device ? fake_device_id_for : no_device_id_for
      ));
    }

    void SetUp() override {
      fake_device_present = true;
    }

    void TearDown() override {
      vd::install(nullptr);
    }
  };

  config::video_t base_video() {
    config::video_t video {};
    video.output_name = "configured-output";
    video.dd.configuration_option = config::video_t::dd_t::config_option_e::verify_only;
    video.dd.resolution_option = config::video_t::dd_t::resolution_option_e::disabled;
    video.dd.refresh_rate_option = config::video_t::dd_t::refresh_rate_option_e::disabled;
    return video;
  }
}  // namespace

TEST(VirtualDisplayLaunch, ParsesDisplayChoice) {
  EXPECT_EQ(vd::parse_display_choice("virtual"), vd::display_choice_e::virtual_display);
  EXPECT_EQ(vd::parse_display_choice("mirror"), vd::display_choice_e::mirror);
  EXPECT_EQ(vd::parse_display_choice(""), vd::display_choice_e::upstream);
  EXPECT_EQ(vd::parse_display_choice("VIRTUAL"), vd::display_choice_e::upstream);
}

TEST(VirtualDisplayLaunch, ClientUidPrefersCertificate) {
  EXPECT_EQ(vd::client_uid_from("-----BEGIN CERTIFICATE-----abc", "0123456789ABCDEF"), "-----BEGIN CERTIFICATE-----abc");
  EXPECT_EQ(vd::client_uid_from("", "0123456789ABCDEF"), "0123456789ABCDEF");
}

TEST(VirtualDisplayLaunch, RefreshHzAcceptsHzAndMillihertz) {
  EXPECT_EQ(vd::refresh_hz_from_fps(120), 120);
  EXPECT_EQ(vd::refresh_hz_from_fps(119880), 119);
  EXPECT_EQ(vd::refresh_hz_from_fps(0), 60);
}

TEST(VirtualDisplayLaunch, MonitorGuidIsStableNameBasedV5) {
  const auto a {vd::monitor_guid_for("tablet")};
  const auto b {vd::monitor_guid_for("tablet")};
  const auto c {vd::monitor_guid_for("outro")};
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
  EXPECT_EQ(a[6] >> 4, 5);  // version 5
  EXPECT_EQ(a[8] & 0xC0, 0x80);  // RFC 4122 variant
}

TEST_F(VirtualDisplayFixture, EnsureCreatesOnce) {
  install_fake();
  const auto id {vd::instance().ensure("tab", {2800, 1752, 120})};
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(*id, DEVICE_ID);
  EXPECT_EQ(counts->add, 1);
  EXPECT_EQ(vd::instance().active_device_id(), std::optional<std::string> {DEVICE_ID});
}

TEST_F(VirtualDisplayFixture, EnsureReusesSameClient) {
  install_fake();
  vd::instance().ensure("tab", {2800, 1752, 120});
  const auto id {vd::instance().ensure("tab", {2800, 1752, 120})};
  EXPECT_EQ(id, std::optional<std::string> {DEVICE_ID});
  EXPECT_EQ(counts->add, 1);
  EXPECT_EQ(counts->remove, 0);
}

TEST_F(VirtualDisplayFixture, EnsureRecreatesOnModeChange) {
  install_fake();
  vd::instance().ensure("tab", {2800, 1752, 120});
  vd::instance().ensure("tab", {1400, 876, 60});
  EXPECT_EQ(counts->remove, 1);
  EXPECT_EQ(counts->add, 2);
}

TEST_F(VirtualDisplayFixture, StartupCleanupRemovesAll) {
  install_fake();
  vd::instance().ensure("tab", {2800, 1752, 120});
  vd::instance().startup_cleanup();
  EXPECT_EQ(counts->remove_all, 1);
  EXPECT_FALSE(vd::instance().active_device_id().has_value());
}

TEST_F(VirtualDisplayFixture, ReleaseAllClearsActive) {
  install_fake();
  vd::instance().ensure("tab", {2800, 1752, 120});
  EXPECT_EQ(vd::effective_output_name("cfg"), DEVICE_ID);
  vd::instance().release_all();
  EXPECT_EQ(counts->remove, 1);
  EXPECT_EQ(vd::effective_output_name("cfg"), "cfg");
}

TEST_F(VirtualDisplayFixture, FallsBackToMirrorWhenDriverFails) {
  install_fake(/* fail_add = */ true);
  EXPECT_FALSE(vd::video_config_for_launch(base_video(), vd::display_choice_e::virtual_display, "tab", {2800, 1752, 120}).has_value());
  EXPECT_FALSE(vd::instance().active_device_id().has_value());
}

TEST_F(VirtualDisplayFixture, FallsBackToMirrorWhenDeviceIdIsUnknown) {
  install_fake(/* fail_add = */ false, /* resolve_device = */ false);
  EXPECT_FALSE(vd::video_config_for_launch(base_video(), vd::display_choice_e::virtual_display, "tab", {2800, 1752, 120}).has_value());
  EXPECT_FALSE(vd::instance().active_device_id().has_value());
  EXPECT_EQ(counts->remove, 1);  // the display that could not be resolved is not left behind
}

TEST_F(VirtualDisplayFixture, VirtualOverridesOutputAndDd) {
  install_fake();
  const auto video {vd::video_config_for_launch(base_video(), vd::display_choice_e::virtual_display, "tab", {2800, 1752, 120})};
  ASSERT_TRUE(video.has_value());
  EXPECT_EQ(video->output_name, DEVICE_ID);
  EXPECT_EQ(video->dd.configuration_option, config::video_t::dd_t::config_option_e::ensure_only_display);
  EXPECT_EQ(video->dd.resolution_option, config::video_t::dd_t::resolution_option_e::automatic);
  EXPECT_EQ(video->dd.refresh_rate_option, config::video_t::dd_t::refresh_rate_option_e::automatic);
}

TEST_F(VirtualDisplayFixture, MirrorDoesNotConfigure) {
  install_fake();
  EXPECT_FALSE(vd::video_config_for_launch(base_video(), vd::display_choice_e::mirror, "tab", {2800, 1752, 120}).has_value());
  EXPECT_EQ(counts->add, 0);
}

TEST_F(VirtualDisplayFixture, UpstreamKeepsBase) {
  install_fake();
  const auto video {vd::video_config_for_launch(base_video(), vd::display_choice_e::upstream, "tab", {2800, 1752, 120})};
  ASSERT_TRUE(video.has_value());
  EXPECT_EQ(video->output_name, "configured-output");
  EXPECT_EQ(video->dd.configuration_option, config::video_t::dd_t::config_option_e::verify_only);
  EXPECT_EQ(counts->add, 0);
}

TEST_F(VirtualDisplayFixture, DriverlessManagerIsSafe) {
  vd::install(nullptr);
  EXPECT_FALSE(vd::instance().ensure("tab", {2800, 1752, 120}).has_value());
  vd::instance().release_all();
  vd::instance().startup_cleanup();
  EXPECT_EQ(vd::effective_output_name("cfg"), "cfg");
}

TEST_F(VirtualDisplayFixture, KeepAliveRunsWhileDriverAddIsSlow) {
  // SudoVDA removes displays after 3 s without a ping; creating one can take seconds.
  counts->add_delay = std::chrono::milliseconds {2500};
  install_fake();
  ASSERT_TRUE(vd::instance().ensure("tab", {2800, 1752, 120}).has_value());
  EXPECT_GE(counts->keep_alive_during_add.load(), 1);
}

TEST_F(VirtualDisplayFixture, EnsureRecreatesWhenReusedDisplayVanished) {
  install_fake();
  vd::instance().ensure("tab", {2800, 1752, 120});
  fake_device_present = false;  // removed behind our back (watchdog, driver restart)
  EXPECT_FALSE(vd::instance().ensure("tab", {2800, 1752, 120}).has_value());
  EXPECT_EQ(counts->add, 2);
  fake_device_present = true;
  EXPECT_EQ(vd::instance().ensure("tab", {2800, 1752, 120}), std::optional<std::string> {DEVICE_ID});
}

TEST_F(VirtualDisplayFixture, SessionEndRevertsWhenVirtualActive) {
  install_fake();
  EXPECT_FALSE(vd::should_revert_on_session_end(false));
  EXPECT_TRUE(vd::should_revert_on_session_end(true));
  vd::instance().ensure("tab", {2800, 1752, 120});
  EXPECT_TRUE(vd::should_revert_on_session_end(false));
}

TEST_F(VirtualDisplayFixture, PrepareLaunchAlwaysCancelsPendingRevert) {
  install_fake();
  int cancels = 0, reverts = 0;
  vd::prepare_launch(vd::display_choice_e::virtual_display, [&]() { ++cancels; }, [&]() { ++reverts; });
  vd::prepare_launch(vd::display_choice_e::mirror, [&]() { ++cancels; }, [&]() { ++reverts; });
  vd::prepare_launch(vd::display_choice_e::upstream, [&]() { ++cancels; }, [&]() { ++reverts; });
  EXPECT_EQ(cancels, 3);
  EXPECT_EQ(reverts, 0);  // nothing virtual was active
}

TEST_F(VirtualDisplayFixture, PrepareMirrorAfterVirtualRevertsAndReleases) {
  install_fake();
  vd::instance().ensure("tab", {2800, 1752, 120});
  int reverts = 0;
  vd::prepare_launch(vd::display_choice_e::mirror, []() {}, [&]() { ++reverts; });
  EXPECT_EQ(reverts, 1);
  EXPECT_FALSE(vd::instance().active_device_id().has_value());
  EXPECT_EQ(vd::effective_output_name("cfg"), "cfg");
}

TEST_F(VirtualDisplayFixture, PrepareUpstreamAfterVirtualRevertsAndReleases) {
  install_fake();
  vd::instance().ensure("tab", {2800, 1752, 120});
  int reverts = 0;
  vd::prepare_launch(vd::display_choice_e::upstream, []() {}, [&]() { ++reverts; });
  EXPECT_EQ(reverts, 1);
  EXPECT_FALSE(vd::instance().active_device_id().has_value());
}

TEST_F(VirtualDisplayFixture, PrepareVirtualKeepsActiveDisplay) {
  install_fake();
  vd::instance().ensure("tab", {2800, 1752, 120});
  int reverts = 0;
  vd::prepare_launch(vd::display_choice_e::virtual_display, []() {}, [&]() { ++reverts; });
  EXPECT_EQ(reverts, 0);
  EXPECT_TRUE(vd::instance().active_device_id().has_value());
}

TEST(VirtualDisplayScale, ParsesScale) {
  EXPECT_EQ(vd::parse_scale("150"), std::optional<int> {150});
  EXPECT_EQ(vd::parse_scale("100"), std::optional<int> {100});
  EXPECT_EQ(vd::parse_scale("125"), std::optional<int> {125});
  EXPECT_EQ(vd::parse_scale(""), std::nullopt);
  EXPECT_EQ(vd::parse_scale("0"), std::nullopt);
  EXPECT_EQ(vd::parse_scale("130"), std::nullopt);  // Windows only has fixed steps
  EXPECT_EQ(vd::parse_scale("abc"), std::nullopt);
}

TEST(VirtualDisplayScale, StepIsRelativeToRecommended) {
  // Windows sets DPI as a step relative to the recommended one; min_rel = -(index of recommended).
  // Recommended 175% (index 3): min_rel = -3.
  EXPECT_EQ(vd::dpi_relative_step(150, -3), std::optional<int> {-1});
  EXPECT_EQ(vd::dpi_relative_step(175, -3), std::optional<int> {0});
  EXPECT_EQ(vd::dpi_relative_step(100, -3), std::optional<int> {-3});
  EXPECT_EQ(vd::dpi_relative_step(200, -3), std::optional<int> {1});
  // Recommended 100%: min_rel = 0.
  EXPECT_EQ(vd::dpi_relative_step(125, 0), std::optional<int> {1});
  EXPECT_EQ(vd::dpi_relative_step(130, 0), std::nullopt);
}

TEST(VirtualDisplayLaunch, DescribesLaunchForTheLog) {
  EXPECT_EQ(vd::describe_launch("2800x1752x120", "virtual", "150"), "mode=2800x1752x120 grDisplay=virtual grScale=150");
}

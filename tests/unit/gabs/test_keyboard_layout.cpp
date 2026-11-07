/**
 * @file tests/unit/gabs/test_keyboard_layout.cpp
 * @brief Gabs Remote: tests for the Windows keyboard layout switched during a session (grKeyboard).
 */

// test includes
#include "../../tests_common.h"

// standard includes
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// local includes
#include <src/gabs/keyboard_layout.h>

namespace kl = gabs::keyboard_layout;

namespace {
  struct fake_state_t {
    std::optional<std::uintptr_t> current {0x04090409};  // English (US)
    bool fail_activate = false;
    std::vector<std::string> activated_layouts;
    std::vector<std::uintptr_t> restored_handles;
  };

  class fake_backend_t: public kl::backend_t {
  public:
    explicit fake_backend_t(std::shared_ptr<fake_state_t> state):
        state {std::move(state)} {
    }

    std::optional<std::uintptr_t> current() override {
      return state->current;
    }

    bool activate_layout(const std::string &klid) override {
      if (state->fail_activate) {
        return false;
      }
      state->activated_layouts.push_back(klid);
      state->current = 0xF0010416;
      return true;
    }

    bool activate_handle(std::uintptr_t handle) override {
      state->restored_handles.push_back(handle);
      state->current = handle;
      return true;
    }

  private:
    std::shared_ptr<fake_state_t> state;
  };

  struct switcher_fixture_t {
    std::shared_ptr<fake_state_t> state {std::make_shared<fake_state_t>()};
    kl::switcher_t switcher {std::make_unique<fake_backend_t>(state)};
  };
}  // namespace

TEST(KeyboardLayoutLaunch, ParsesLayoutId) {
  EXPECT_EQ(kl::parse_layout("00010416"), std::optional<std::string> {"00010416"});
  EXPECT_EQ(kl::parse_layout("0001041a"), std::optional<std::string> {"0001041A"});
  EXPECT_EQ(kl::parse_layout(""), std::nullopt);
  EXPECT_EQ(kl::parse_layout("0"), std::nullopt);
  EXPECT_EQ(kl::parse_layout("1041"), std::nullopt);
  EXPECT_EQ(kl::parse_layout("000104160"), std::nullopt);
  EXPECT_EQ(kl::parse_layout("0001041G"), std::nullopt);
  EXPECT_EQ(kl::parse_layout("pt-BR"), std::nullopt);
}

TEST(KeyboardLayoutSwitcher, ApplySavesCurrentAndActivates) {
  switcher_fixture_t f;
  f.switcher.apply(std::string {"00010416"});
  EXPECT_EQ(f.state->activated_layouts, std::vector<std::string> {"00010416"});
  EXPECT_EQ(f.switcher.saved(), std::optional<std::uintptr_t> {0x04090409});
}

TEST(KeyboardLayoutSwitcher, RestoreGoesBackToSavedLayoutOnce) {
  switcher_fixture_t f;
  f.switcher.apply(std::string {"00010416"});
  f.switcher.restore();
  f.switcher.restore();
  EXPECT_EQ(f.state->restored_handles, std::vector<std::uintptr_t> {0x04090409});
  EXPECT_EQ(f.switcher.saved(), std::nullopt);
}

TEST(KeyboardLayoutSwitcher, ResumeKeepsTheLayoutSavedBeforeTheSession) {
  switcher_fixture_t f;
  f.switcher.apply(std::string {"00010416"});
  f.switcher.apply(std::string {"00010416"});  // /resume: current is already pt-BR
  EXPECT_EQ(f.switcher.saved(), std::optional<std::uintptr_t> {0x04090409});
  f.switcher.restore();
  EXPECT_EQ(f.state->current, std::optional<std::uintptr_t> {0x04090409});
}

TEST(KeyboardLayoutSwitcher, LaunchWithoutLayoutRestoresPrevious) {
  switcher_fixture_t f;
  f.switcher.apply(std::string {"00010416"});
  f.switcher.apply(std::nullopt);
  EXPECT_EQ(f.state->restored_handles, std::vector<std::uintptr_t> {0x04090409});
  EXPECT_EQ(f.switcher.saved(), std::nullopt);
}

TEST(KeyboardLayoutSwitcher, WithoutLayoutAndNothingSavedDoesNothing) {
  switcher_fixture_t f;
  f.switcher.apply(std::nullopt);
  f.switcher.restore();
  EXPECT_TRUE(f.state->activated_layouts.empty());
  EXPECT_TRUE(f.state->restored_handles.empty());
}

TEST(KeyboardLayoutSwitcher, DoesNotSwitchWhenCurrentLayoutIsUnknown) {
  switcher_fixture_t f;
  f.state->current = std::nullopt;  // no foreground window: nothing to go back to
  f.switcher.apply(std::string {"00010416"});
  EXPECT_TRUE(f.state->activated_layouts.empty());
  EXPECT_EQ(f.switcher.saved(), std::nullopt);
}

TEST(KeyboardLayoutSwitcher, FailedActivationForgetsSavedLayout) {
  switcher_fixture_t f;
  f.state->fail_activate = true;
  f.switcher.apply(std::string {"00010416"});
  EXPECT_EQ(f.switcher.saved(), std::nullopt);
  f.switcher.restore();
  EXPECT_TRUE(f.state->restored_handles.empty());
}

TEST(KeyboardLayoutSwitcher, WithoutBackendIsSafe) {
  kl::switcher_t switcher {nullptr};
  switcher.apply(std::string {"00010416"});
  switcher.restore();
  EXPECT_EQ(switcher.saved(), std::nullopt);
}

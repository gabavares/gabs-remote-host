/**
 * @file tests/unit/gabs/test_mic.cpp
 * @brief Gabs Remote: tests for the tablet microphone (packet, jitter buffer, decoder).
 */

// test includes
#include "../../tests_common.h"

// standard includes
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

// lib includes
#include <opus/opus.h>

// local includes
#include <src/gabs/mic.h>

namespace gm = gabs::mic;
using namespace std::chrono_literals;
using kind_e = gm::frame_t::kind_e;

namespace {
  crypto::aes_t test_key() {
    crypto::aes_t k(16);
    for (int i = 0; i < 16; ++i) {
      k[i] = static_cast<std::uint8_t>(i);
    }
    return k;
  }

  std::string test_vector() {
    auto path = std::filesystem::path(SUNSHINE_SOURCE_DIR) / "tests" / "unit" / "gabs" / "testdata" / "mic-packet.hex";
    std::ifstream in(path);
    std::string hex;
    in >> hex;
    std::string bytes;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
      bytes.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return bytes;
  }

  std::string as_string(const std::vector<std::uint8_t> &v) {
    return {v.begin(), v.end()};
  }

  gm::packet_t pkt(std::uint32_t seq) {
    return {seq, {static_cast<std::uint8_t>(seq)}};
  }

  struct out_t {
    kind_e kind;
    int payload;  ///< First byte of the Opus data (the seq in these tests), -1 when none.
  };

  out_t take(const gm::frame_t &f) {
    return {f.kind, f.opus.empty() ? -1 : f.opus[0]};
  }

  /**
   * Sends each seq at seq * 20 ms (except the ones in `missing`) and pops once every 20 ms,
   * right after the push of that tick, for `ticks` ticks.
   */
  std::vector<out_t> run(gm::jitter_t &j, std::uint32_t first, std::uint32_t last, const std::vector<std::uint32_t> &missing, int ticks) {
    std::vector<out_t> out;
    auto t0 = std::chrono::steady_clock::time_point {} + 1h;
    for (int tick = 0; tick < ticks; ++tick) {
      auto seq = first + static_cast<std::uint32_t>(tick);
      auto now = t0 + tick * 20ms;
      if (seq <= last && std::find(missing.begin(), missing.end(), seq) == missing.end()) {
        j.push(pkt(seq), now);
      }
      if (auto f = j.pop(now)) {
        out.push_back(take(*f));
      }
    }
    return out;
  }

  std::vector<int> normals(const std::vector<out_t> &v) {
    std::vector<int> r;
    for (auto &o : v) {
      if (o.kind == kind_e::normal) {
        r.push_back(o.payload);
      }
    }
    return r;
  }
}  // namespace

TEST(MicPacket, OpensTestVector) {
  auto bytes = test_vector();
  ASSERT_EQ(bytes.size(), 28u);
  auto p = gm::open_packet(bytes, test_key());
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->seq, 7u);
  EXPECT_EQ(as_string(p->opus), "gabs");
  EXPECT_EQ(as_string(gm::seal_packet(7, "gabs", test_key())), bytes);
}

TEST(MicPacket, RejectsWrongKeyMagicTagAndSize) {
  auto good = test_vector();
  auto other = test_key();
  other[0] ^= 1;
  EXPECT_FALSE(gm::open_packet(good, other).has_value());

  auto magic = good;
  magic[3] = '2';
  EXPECT_FALSE(gm::open_packet(magic, test_key()).has_value());

  auto tag = good;
  tag[8] ^= 0x01;
  EXPECT_FALSE(gm::open_packet(tag, test_key()).has_value());

  EXPECT_FALSE(gm::open_packet(good.substr(0, 24), test_key()).has_value());
  EXPECT_FALSE(gm::open_packet("", test_key()).has_value());

  auto big = as_string(gm::seal_packet(1, std::string(1001, 'x'), test_key()));
  EXPECT_FALSE(gm::open_packet(big, test_key()).has_value());
  auto max = as_string(gm::seal_packet(1, std::string(1000, 'x'), test_key()));
  EXPECT_TRUE(gm::open_packet(max, test_key()).has_value());
}

TEST(MicPacket, SeqIsBoundToIv) {
  auto p = test_vector();
  p[4] = 8;  // seq 7 -> 8 in the header
  EXPECT_FALSE(gm::open_packet(p, test_key()).has_value());
}

TEST(MicReceiver, InOrder) {
  gm::jitter_t j;
  auto out = run(j, 0, 9, {}, 13);
  EXPECT_EQ(normals(out), (std::vector<int> {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));
  // Nothing but normal frames until the last packet was played.
  for (std::size_t i = 0; i < 10; ++i) {
    EXPECT_EQ(out[i].kind, kind_e::normal);
  }
}

TEST(MicReceiver, ReordersWithinTarget) {
  gm::jitter_t j;
  auto t0 = std::chrono::steady_clock::time_point {} + 1h;
  j.push(pkt(1), t0);
  j.push(pkt(3), t0 + 2ms);
  j.push(pkt(2), t0 + 5ms);
  std::vector<out_t> out;
  for (int i = 1; i <= 3; ++i) {
    auto f = j.pop(t0 + i * 20ms);
    ASSERT_TRUE(f.has_value());
    out.push_back(take(*f));
  }
  EXPECT_EQ(normals(out), (std::vector<int> {1, 2, 3}));
}

TEST(MicReceiver, LossUsesFecThenConceal) {
  {
    gm::jitter_t j;
    auto out = run(j, 1, 12, {5}, 14);
    // 1..4 normal, 5 recovered from 6 by FEC, then 6.. normal.
    ASSERT_GE(out.size(), 6u);
    EXPECT_EQ(out[3].kind, kind_e::normal);
    EXPECT_EQ(out[3].payload, 4);
    EXPECT_EQ(out[4].kind, kind_e::fec);
    EXPECT_EQ(out[4].payload, 6);
    EXPECT_EQ(out[5].kind, kind_e::normal);
    EXPECT_EQ(out[5].payload, 6);
  }
  {
    gm::jitter_t j;
    auto out = run(j, 1, 14, {5, 6, 7}, 16);
    ASSERT_GE(out.size(), 8u);
    EXPECT_EQ(out[3].payload, 4);
    EXPECT_EQ(out[4].kind, kind_e::conceal);
    EXPECT_EQ(out[5].kind, kind_e::conceal);
    // By the time 7 is due, 8 has arrived: its FEC data rebuilds 7.
    EXPECT_EQ(out[6].kind, kind_e::fec);
    EXPECT_EQ(out[6].payload, 8);
    EXPECT_EQ(out[7].kind, kind_e::normal);
    EXPECT_EQ(out[7].payload, 8);
  }
}

TEST(MicReceiver, DropsDuplicateAndLate) {
  gm::jitter_t j;
  auto t0 = std::chrono::steady_clock::time_point {} + 1h;
  std::vector<out_t> out;
  for (int tick = 0; tick < 12; ++tick) {
    auto now = t0 + tick * 20ms;
    if (tick >= 1 && tick <= 8) {
      j.push(pkt(tick), now);
    }
    if (tick == 5) {
      j.push(pkt(4), now + 1ms);  // duplicate
    }
    if (tick == 8) {
      j.push(pkt(2), now + 1ms);  // already played
    }
    if (auto f = j.pop(now + 2ms)) {
      out.push_back(take(*f));
    }
  }
  EXPECT_EQ(normals(out), (std::vector<int> {1, 2, 3, 4, 5, 6, 7, 8}));
  for (std::size_t i = 0; i < 8; ++i) {
    EXPECT_EQ(out[i].kind, kind_e::normal);
  }
}

TEST(MicReceiver, CapsDelay) {
  gm::jitter_t j;
  auto t0 = std::chrono::steady_clock::time_point {} + 1h;
  for (std::uint32_t s = 0; s < 20; ++s) {
    j.push(pkt(s), t0);
  }
  EXPECT_LE(j.delay(), 120ms);
  auto f = j.pop(t0);
  ASSERT_TRUE(f.has_value());
  EXPECT_EQ(f->kind, kind_e::normal);
  EXPECT_EQ(f->opus[0], 14);  // 14..19 = 120 ms kept, the 14 oldest dropped
}

TEST(MicReceiver, SilenceWhenPacketsStop) {
  gm::jitter_t j;
  auto t0 = std::chrono::steady_clock::time_point {} + 1h;
  std::vector<out_t> out;
  int tick = 0;
  for (; tick < 10; ++tick) {
    j.push(pkt(tick), t0 + tick * 20ms);
    if (auto f = j.pop(t0 + tick * 20ms)) {
      out.push_back(take(*f));
    }
  }
  // 9 was pushed at tick 9; stop sending for 300 ms.
  std::optional<gm::frame_t> last;
  for (; tick < 25; ++tick) {
    last = j.pop(t0 + tick * 20ms);
  }
  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(last->kind, kind_e::silence);

  // Packets come back with a much higher seq: playback resumes from them.
  std::vector<out_t> after;
  for (int k = 0; k < 6; ++k, ++tick) {
    j.push(pkt(100 + k), t0 + tick * 20ms);
    if (auto f = j.pop(t0 + tick * 20ms)) {
      after.push_back(take(*f));
    }
  }
  auto n = normals(after);
  ASSERT_FALSE(n.empty());
  EXPECT_EQ(n.front(), 100);
}

TEST(MicDecoder, SilenceIsZeros) {
  gm::decoder_t d;
  auto pcm = d.decode({kind_e::silence, {}});
  for (auto s : pcm) {
    ASSERT_EQ(s, 0);
  }
}

TEST(MicDecoder, DecodesEncodedTone) {
  int err = 0;
  auto *enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err);
  ASSERT_EQ(err, OPUS_OK);
  opus_encoder_ctl(enc, OPUS_SET_BITRATE(32000));

  gm::decoder_t d;
  std::array<std::int16_t, 960> out {};
  for (int frame = 0; frame < 25; ++frame) {
    std::array<std::int16_t, 960> pcm {};
    for (int i = 0; i < 960; ++i) {
      auto n = frame * 960 + i;
      pcm[i] = static_cast<std::int16_t>(10000 * std::sin(2 * 3.14159265358979 * 440 * n / 48000.0));
    }
    std::vector<std::uint8_t> buf(1000);
    auto len = opus_encode(enc, pcm.data(), 960, buf.data(), static_cast<opus_int32>(buf.size()));
    ASSERT_GT(len, 0);
    buf.resize(len);
    out = d.decode({kind_e::normal, buf});
  }
  opus_encoder_destroy(enc);

  double energy = 0;
  int crossings = 0;
  for (int i = 0; i < 960; ++i) {
    energy += static_cast<double>(out[i]) * out[i];
    if (i > 0 && ((out[i - 1] < 0) != (out[i] < 0))) {
      ++crossings;
    }
  }
  auto rms = std::sqrt(energy / 960);
  EXPECT_GT(rms, 2000);
  // 440 Hz over 20 ms = 8.8 periods = about 17.6 zero crossings.
  EXPECT_GE(crossings, 14);
  EXPECT_LE(crossings, 22);
}

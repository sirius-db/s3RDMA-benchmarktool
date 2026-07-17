/*
 * Copyright 2025, Sirius Contributors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "verify/manifest.hpp"
#include "verify/sampler.hpp"

#ifndef S3RDMA_HAVE_LIBCRYPTO
#define S3RDMA_HAVE_LIBCRYPTO 1
#endif

#if S3RDMA_HAVE_LIBCRYPTO
#include "sha256.hpp"
#endif

namespace {

using s3rdma::verify::ClosureStatus;
using s3rdma::verify::Manifest;
using s3rdma::verify::ManifestEntry;
using s3rdma::verify::ParseStatus;
using s3rdma::verify::RequestedSpan;
using s3rdma::verify::Sampler;

constexpr const char* kDigestA =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr const char* kDigestB =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr const char* kDigestC =
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
constexpr const char* kDigestD =
    "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";

class TestState {
 public:
  void check(bool condition, const char* ac, const char* message, const char* file,
             int line) {
    ++total_;
    if (condition) {
      std::cout << "PASS " << ac << ": " << message << '\n';
      return;
    }
    ++failed_;
    std::cout << "FAIL " << ac << ": " << message << " (" << file << ':' << line
              << ")\n";
  }

  int finish() const {
    if (failed_ == 0) {
      std::cout << "SELFTEST PASS " << total_ << '/' << total_ << '\n';
      return 0;
    }
    std::cout << "SELFTEST FAIL " << failed_ << '/' << total_ << '\n';
    return 1;
  }

 private:
  int total_ = 0;
  int failed_ = 0;
};

#define CHECK(state, condition, ac, message) \
  (state).check((condition), (ac), (message), __FILE__, __LINE__)

Manifest parse_fixture(const std::string& name, ParseStatus& status) {
  const std::string path = "tests/fixtures/" + name;
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot open fixture: " + path);
  return s3rdma::verify::parse_manifest(input, status);
}

void test_manifest_parser(TestState& state) {
  ParseStatus status;
  Manifest manifest = parse_fixture("good.tsv", status);
  CHECK(state, status.ok, "AC1", "valid manifest reports success");
  CHECK(state, status.error.empty(), "AC1", "valid manifest has no error text");
  CHECK(state, status.line == 0, "AC1", "valid manifest has no offending line");
  CHECK(state, manifest.by_key.size() == 2, "AC1", "valid manifest has two keys");

  const auto full = manifest.by_key.find("object-full.bin");
  CHECK(state, full != manifest.by_key.end(), "AC1", "full-object key is present");
  CHECK(state, full != manifest.by_key.end() && full->second.key == "object-full.bin",
        "AC1", "full-object key is byte-exact");
  CHECK(state, full != manifest.by_key.end() && full->second.size == 4096, "AC1",
        "full-object size is exact");
  CHECK(state, full != manifest.by_key.end() && full->second.full_sha256 == kDigestA,
        "AC1", "full-object digest is exact");
  CHECK(state, full != manifest.by_key.end() && full->second.prefix_sha256.empty(),
        "AC1", "full-object-only entry has no prefixes");

  const auto prefixed = manifest.by_key.find("object-prefix.bin");
  CHECK(state, prefixed != manifest.by_key.end(), "AC1", "prefix key is present");
  CHECK(state,
        prefixed != manifest.by_key.end() && prefixed->second.key == "object-prefix.bin",
        "AC1", "prefix key is byte-exact");
  CHECK(state, prefixed != manifest.by_key.end() && prefixed->second.size == 2097152,
        "AC1", "prefix entry size is exact");
  CHECK(state,
        prefixed != manifest.by_key.end() && prefixed->second.full_sha256 == kDigestB,
        "AC1", "prefix entry full digest is exact");
  CHECK(state,
        prefixed != manifest.by_key.end() && prefixed->second.prefix_sha256.size() == 2,
        "AC1", "prefix entry has two prefix digests");
  CHECK(state,
        prefixed != manifest.by_key.end() &&
            prefixed->second.prefix_sha256.find(4096) !=
                prefixed->second.prefix_sha256.end() &&
            prefixed->second.prefix_sha256.at(4096) == kDigestD,
        "AC1", "4096-byte prefix digest is exact");
  CHECK(state,
        prefixed != manifest.by_key.end() &&
            prefixed->second.prefix_sha256.find(1048576) !=
                prefixed->second.prefix_sha256.end() &&
            prefixed->second.prefix_sha256.at(1048576) == kDigestC,
        "AC1", "1048576-byte prefix digest is exact");

  status = {};
  parse_fixture("duplicate_key.tsv", status);
  CHECK(state, !status.ok, "AC2", "identical duplicate key is rejected");
  CHECK(state, status.line == 2 && !status.error.empty(), "AC2",
        "duplicate reports the second physical line");

  status = {};
  parse_fixture("conflicting_key.tsv", status);
  CHECK(state, !status.ok, "AC3", "conflicting key record is rejected");
  CHECK(state, status.line == 2 && !status.error.empty(), "AC3",
        "conflict reports its physical line");

  const std::array<const char*, 8> bad_digest_files = {
      "bad_digest_63.tsv", "bad_digest_65.tsv", "bad_digest_uppercase.tsv",
      "bad_digest_nonhex.tsv", "bad_prefix_digest_63.tsv",
      "bad_prefix_digest_65.tsv", "bad_prefix_digest_uppercase.tsv",
      "bad_prefix_digest_nonhex.tsv"};
  const std::array<const char*, 8> bad_digest_messages = {
      "63-character digest is rejected", "65-character digest is rejected",
      "uppercase digest is rejected", "non-hex digest is rejected",
      "63-character prefix digest is rejected",
      "65-character prefix digest is rejected",
      "uppercase prefix digest is rejected", "non-hex prefix digest is rejected"};
  for (size_t i = 0; i < bad_digest_files.size(); ++i) {
    status = {};
    parse_fixture(bad_digest_files[i], status);
    CHECK(state, !status.ok && status.line == 1 && !status.error.empty(), "AC4",
          bad_digest_messages[i]);
  }

  const std::array<const char*, 4> bad_prefix_files = {
      "bad_prefix_empty_len.tsv", "bad_prefix_zero_len.tsv",
      "bad_prefix_nonnumeric_len.tsv", "bad_prefix_duplicate_len.tsv"};
  const std::array<const char*, 4> bad_prefix_messages = {
      "empty prefix length is rejected", "zero prefix length is rejected",
      "non-numeric prefix length is rejected", "duplicate prefix length is rejected"};
  for (size_t i = 0; i < bad_prefix_files.size(); ++i) {
    status = {};
    parse_fixture(bad_prefix_files[i], status);
    CHECK(state, !status.ok && status.line == 1 && !status.error.empty(), "AC5",
          bad_prefix_messages[i]);
  }

  const std::array<const char*, 4> bad_size_key_files = {
      "bad_size_zero.tsv", "bad_size_nonnumeric.tsv", "bad_size_trailing.tsv",
      "bad_empty_key.tsv"};
  const std::array<const char*, 4> bad_size_key_messages = {
      "zero size is rejected", "non-numeric size is rejected",
      "partially numeric size is rejected", "empty key is rejected"};
  for (size_t i = 0; i < bad_size_key_files.size(); ++i) {
    status = {};
    parse_fixture(bad_size_key_files[i], status);
    CHECK(state, !status.ok && status.line == 1 && !status.error.empty(), "AC6",
          bad_size_key_messages[i]);
  }

  status = {};
  Manifest comments = parse_fixture("comments_blank_valid.tsv", status);
  CHECK(state, status.ok && status.error.empty(), "AC7",
        "comments and blank lines are skipped");
  const auto comment_entry = comments.by_key.find("commented-object.bin");
  CHECK(state,
        comments.by_key.size() == 1 && comment_entry != comments.by_key.end() &&
            comment_entry->second.size == 8192 &&
            comment_entry->second.full_sha256 == kDigestA,
        "AC7", "record following comments is parsed exactly");
  status = {};
  parse_fixture("comments_physical_line_error.tsv", status);
  CHECK(state, !status.ok && !status.error.empty(), "AC7",
        "later duplicate after skipped lines is rejected");
  CHECK(state, status.line == 6, "AC7", "error counts skipped physical lines");
}

Manifest closure_manifest() {
  Manifest manifest;

  ManifestEntry full;
  full.key = "object-full.bin";
  full.size = 4096;
  full.full_sha256 = kDigestA;
  manifest.by_key.emplace(full.key, full);

  ManifestEntry prefixed;
  prefixed.key = "object-prefix.bin";
  prefixed.size = 2097152;
  prefixed.full_sha256 = kDigestB;
  prefixed.prefix_sha256.emplace(4096, kDigestD);
  prefixed.prefix_sha256.emplace(1048576, kDigestC);
  manifest.by_key.emplace(prefixed.key, prefixed);

  return manifest;
}

void test_closure(TestState& state) {
  const Manifest manifest = closure_manifest();
  const std::map<std::string, uint64_t> heads = {
      {"object-full.bin", 4096}, {"object-prefix.bin", 2097152}};
  const std::vector<RequestedSpan> covered = {
      {"object-full.bin", 0, 0}, {"object-prefix.bin", 0, 4096}};

  ClosureStatus status = s3rdma::verify::validate_closure(manifest, covered, heads);
  CHECK(state, status.ok, "AC8", "full and prefix spans pass closure");
  status = s3rdma::verify::validate_closure(manifest, {}, heads);
  CHECK(state, !status.ok, "AC8", "empty requested-span set cannot pass vacuously");

  std::map<std::string, uint64_t> missing_manifest_heads = heads;
  missing_manifest_heads.emplace("missing-object.bin", 512);
  status = s3rdma::verify::validate_closure(
      manifest, {{"missing-object.bin", 0, 0}}, missing_manifest_heads);
  CHECK(state, !status.ok, "AC9", "requested key absent from manifest fails closure");
  CHECK(state, status.error.find("missing-object.bin") != std::string::npos, "AC9",
        "missing-manifest error names the requested key");

  std::map<std::string, uint64_t> mismatched_heads = heads;
  mismatched_heads["object-full.bin"] = 4097;
  status = s3rdma::verify::validate_closure(
      manifest, {{"object-full.bin", 0, 0}}, mismatched_heads);
  CHECK(state, !status.ok && !status.error.empty(), "AC10",
        "manifest and HEAD size mismatch fails closure");

  status = s3rdma::verify::validate_closure(
      manifest, {{"object-prefix.bin", 0, 8192}}, heads);
  CHECK(state, !status.ok && !status.error.empty(), "AC11",
        "missing exact prefix digest fails closure");

  const std::map<std::string, uint64_t> missing_head = {
      {"object-prefix.bin", 2097152}};
  status = s3rdma::verify::validate_closure(
      manifest, {{"object-full.bin", 0, 0}}, missing_head);
  CHECK(state, !status.ok && !status.error.empty(), "AC12",
        "manifest key absent from HEAD results fails closure");
}

void test_sampler(TestState& state) {
  Sampler disabled(17, 0, 0, 1);
  bool never_samples = true;
  for (uint64_t occurrence = 0; occurrence < 1000; ++occurrence) {
    never_samples = never_samples && !disabled.should_sample(3, occurrence);
  }
  CHECK(state, never_samples, "AC13", "every=0 never samples");

  Sampler always(17, 1, 0, 1);
  bool always_samples = true;
  for (uint64_t occurrence = 0; occurrence < 1000; ++occurrence) {
    always_samples = always_samples && always.should_sample(3, occurrence);
  }
  CHECK(state, always_samples, "AC13", "every=1 always samples");

  constexpr uint32_t every = 4;
  Sampler windowed(0x12345678, every, 2, 5);
  std::vector<bool> decisions;
  decisions.reserve(44);
  for (uint64_t occurrence = 0; occurrence < 44; ++occurrence) {
    decisions.push_back(windowed.should_sample(7, occurrence));
  }
  bool every_sliding_window_has_one = true;
  for (size_t start = 0; start + every <= decisions.size(); ++start) {
    const auto count = std::count(decisions.begin() + start,
                                  decisions.begin() + start + every, true);
    every_sliding_window_has_one = every_sliding_window_has_one && count == 1;
  }
  CHECK(state, every_sliding_window_has_one, "AC14",
        "every sliding N-occurrence window contains exactly one sample");
  CHECK(state, std::count(decisions.begin(), decisions.end(), true) == 11, "AC14",
        "44 occurrences at every=4 contain eleven samples");

  Sampler per_key(0xabcdef, 2, 0, 1);
  std::array<uint64_t, 4> occurrences{};
  std::array<uint64_t, 4> samples{};
  std::array<uint64_t, 4> naive_samples{};
  for (uint64_t iter = 0; iter < 40; ++iter) {
    const uint32_t key = static_cast<uint32_t>(iter % 4);
    if (per_key.should_sample(key, occurrences[key])) ++samples[key];
    ++occurrences[key];
    if (iter % 2 == 0) ++naive_samples[key];
  }
  bool all_keys_sampled_exactly_half = true;
  for (size_t key = 0; key < samples.size(); ++key) {
    all_keys_sampled_exactly_half =
        all_keys_sampled_exactly_half && occurrences[key] == 10 && samples[key] == 5;
  }
  CHECK(state, all_keys_sampled_exactly_half, "AC15",
        "per-key counters sample every round-robin key exactly half the time");
  CHECK(state,
        naive_samples[0] == 10 && naive_samples[1] == 0 && naive_samples[2] == 10 &&
            naive_samples[3] == 0,
        "AC15", "naive global iter modulo starves two of four keys");

  Sampler first(0x99887766, 7, 2, 4);
  Sampler second(0x99887766, 7, 2, 4);
  bool deterministic = true;
  for (uint32_t key = 0; key < 6; ++key) {
    for (uint64_t occurrence = 0; occurrence < 70; ++occurrence) {
      deterministic =
          deterministic &&
          first.should_sample(key, occurrence) == second.should_sample(key, occurrence);
    }
  }
  CHECK(state, deterministic, "AC16", "identical sampler inputs are deterministic");

  std::array<int, 4> first_samples = {-1, -1, -1, -1};
  for (uint32_t thread = 0; thread < first_samples.size(); ++thread) {
    Sampler sampler(0x11223344, 4, thread, 4);
    for (uint64_t occurrence = 0; occurrence < 4; ++occurrence) {
      if (sampler.should_sample(9, occurrence)) {
        first_samples[thread] = static_cast<int>(occurrence);
        break;
      }
    }
  }
  CHECK(state,
        std::all_of(first_samples.begin(), first_samples.end(),
                    [](int occurrence) { return occurrence >= 0; }),
        "AC16", "each thread has a first sample in one N-occurrence window");
  CHECK(state,
        std::all_of(first_samples.begin(), first_samples.end(),
                    [](int occurrence) { return occurrence >= 0; }) &&
            std::set<int>(first_samples.begin(), first_samples.end()).size() > 1,
        "AC16", "thread sampling phases are not all equal");
}

#if S3RDMA_HAVE_LIBCRYPTO
void test_sha256(TestState& state, const std::string& pattern_path) {
  const std::string empty;
  CHECK(state,
        s3rdma::sha256_hex(empty.data(), empty.size()) ==
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "AC17", "empty-string SHA256 matches the standard vector");

  const std::string abc = "abc";
  CHECK(state,
        s3rdma::sha256_hex(abc.data(), abc.size()) ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "AC17", "abc SHA256 matches the standard vector");

  std::vector<unsigned char> pattern(1024 * 1024);
  for (size_t i = 0; i < pattern.size(); ++i) {
    pattern[i] = static_cast<unsigned char>((i * 131 + (i >> 3) + 17) & 0xff);
  }
  const std::string digest = s3rdma::sha256_hex(pattern.data(), pattern.size());
  CHECK(state,
        digest.size() == 64 &&
            std::all_of(digest.begin(), digest.end(), [](char c) {
              return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            }),
        "AC17", "1 MiB pattern digest is lowercase SHA256 hex");

  std::ofstream output(pattern_path, std::ios::binary);
  if (output) {
    output.write(reinterpret_cast<const char*>(pattern.data()),
                 static_cast<std::streamsize>(pattern.size()));
  }
  const bool pattern_written = output.good();
  output.close();
  CHECK(state, !pattern_path.empty() && pattern_written, "AC17",
        "1 MiB pattern is written for system cross-check");
  std::cout << "PATTERN_SHA256 " << digest << '\n';
}
#endif

struct Options {
  std::string pattern_path;
  bool force_failure = false;
  bool valid = true;
};

Options parse_options(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--harness-fail-probe") {
      options.force_failure = true;
    } else if (arg == "--pattern-file" && i + 1 < argc) {
      options.pattern_path = argv[++i];
    } else {
      options.valid = false;
    }
  }
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  const Options options = parse_options(argc, argv);
  TestState state;

  if (options.force_failure) {
    CHECK(state, false, "AC18", "intentional failure-propagation probe");
    return state.finish();
  }
  if (!options.valid) {
    CHECK(state, false, "AC18", "invalid selftest command line");
    return state.finish();
  }

  try {
    test_manifest_parser(state);
    test_closure(state);
    test_sampler(state);
#if S3RDMA_HAVE_LIBCRYPTO
    test_sha256(state, options.pattern_path);
#else
    std::cout << "SKIP sha256-vectors\n";
    std::cout << "SKIP AC17: libcrypto unavailable\n";
#endif
    CHECK(state, true, "AC18", "harness reached the final contract check");
  } catch (const std::exception& error) {
    const std::string message = std::string("unexpected test infrastructure error: ") +
                                error.what();
    CHECK(state, false, "AC18", message.c_str());
  } catch (...) {
    CHECK(state, false, "AC18", "unexpected non-standard exception");
  }

  return state.finish();
}

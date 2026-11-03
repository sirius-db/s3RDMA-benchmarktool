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

// Rig-free self-test for common/sigv4.hpp and common/s3_protocol.hpp.
// Build and run: tests/run_sigv4_selftest.sh
// Vectors: AWS "Examples: Signature Calculations in AWS Signature Version 4"
// (GET Object, GET Bucket Lifecycle, GET Bucket (List Objects)); the RDMA reply
// semantics follow NVIDIA's multi-storage-client s3_cuobject provider.

#include <climits>
#include <iostream>
#include <stdexcept>
#include <string>

#include <openssl/evp.h>

#include "s3_protocol.hpp"
#include "sigv4.hpp"

namespace {

int g_failed = 0, g_total = 0;

void check(bool ok, const char* id, const char* what) {
  ++g_total;
  if (!ok) { ++g_failed; std::cout << "FAIL " << id << ": " << what << '\n'; }
}

const char* kEmptySha = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

s3rdma::SigV4Credentials aws_example_creds() {
  return {"AKIAIOSFODNN7EXAMPLE", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY", ""};
}

void test_get_object_vector() {
  s3rdma::SigV4Request req;
  req.method = "GET";
  req.host = "examplebucket.s3.amazonaws.com";
  req.path = "/test.txt";
  req.headers = {{"Range", "bytes=0-9"}};
  req.payload_hash = kEmptySha;
  auto r = s3rdma::sigv4_sign(req, aws_example_creds(), "us-east-1", "s3", "20130524T000000Z");
  check(r.signature == "f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41", "V1",
        "GET Object signature matches the AWS example");
  check(r.headers.back().first == "Authorization" &&
            r.headers.back().second ==
                "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request, "
                "SignedHeaders=host;range;x-amz-content-sha256;x-amz-date, "
                "Signature=f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41",
        "V1", "Authorization header matches the AWS example");
}

void test_lifecycle_vector() {
  s3rdma::SigV4Request req;
  req.method = "GET";
  req.host = "examplebucket.s3.amazonaws.com";
  req.path = "/";
  req.query = "lifecycle=";
  req.payload_hash = kEmptySha;
  auto r = s3rdma::sigv4_sign(req, aws_example_creds(), "us-east-1", "s3", "20130524T000000Z");
  check(r.signature == "fea454ca298b7da1c68078a5d1bdbfbbe0d65c699e0f91ac7a200a0136783543", "V2",
        "GET Bucket Lifecycle signature matches the AWS example");
}

void test_list_objects_vector() {
  s3rdma::SigV4Request req;
  req.method = "GET";
  req.host = "examplebucket.s3.amazonaws.com";
  req.path = "/";
  req.query = "max-keys=2&prefix=J";
  req.payload_hash = kEmptySha;
  auto r = s3rdma::sigv4_sign(req, aws_example_creds(), "us-east-1", "s3", "20130524T000000Z");
  check(r.signature == "34b48302e7b5fa45bde8084f4b7868a86f0a534bc59db6670ed5711ef69dc6f7", "V3",
        "GET Bucket (List Objects) signature matches the AWS example");
}

void test_encoding_and_headers() {
  using namespace s3rdma;
  check(sigv4_detail::uri_encode("/b/a key~x.y_z-/q", true) == "/b/a%20key~x.y_z-/q", "E1",
        "path encoding keeps unreserved characters and slashes");
  check(sigv4_detail::uri_encode("a/b", false) == "a%2Fb", "E1", "non-path encoding escapes slashes");
  check(sigv4_detail::uri_encode("/b/a key+\xc3\xa9~%", true) == "/b/a%20key%2B%C3%A9~%25", "E1",
        "plus, percent and UTF-8 bytes are escaped with uppercase hex");
  check(sigv4_detail::uri_encode("/a//b/", true) == "/a//b/", "E1", "repeated slashes are preserved");
  check(sigv4_detail::lower("X-Amz-Meta-\xc3\x89I") == "x-amz-meta-\xc3\x89i", "E1",
        "header-name folding is ASCII-only");
  check(sigv4_detail::canonical_value("  a   b  ") == "a b", "E2", "header values are trimmed and collapsed");
  check(sigv4_detail::canonical_value("\talpha\t\tbeta \t") == "alpha beta", "E2",
        "tabs are whitespace for canonical values");
  check(sigv4_detail::canonical_value("\t\t") .empty(), "E2", "tab-only values canonicalize to empty");
  check(sigv4_detail::canonical_value("x\t") == "x", "E2", "trailing tab is trimmed");

  const char desc[] = {'7', 'f', ':', '1', '\0'};
  check(encode_rdma_token(desc, sizeof desc, TokenEncoding::Verbatim) == "7f:1", "E3",
        "verbatim token drops the terminating NUL");
  check(encode_rdma_token(desc, sizeof desc, TokenEncoding::Base64) == "N2Y6MQA=", "E3",
        "legacy Base64 preserves all desc_len bytes");

  ProtocolOptions o;
  auto plain = control_plane_headers(o, "GET", "h:8080", "/b/k", "bytes=0-1", "tok", 0);
  check(plain.size() == 2 && plain[0].first == kHdrRdmaToken && plain[1].first == "Range", "E4",
        "unsigned requests carry only token and Range");
  o.auth = AuthMode::SigV4;
  o.credentials = aws_example_creds();
  auto signed_hdrs = control_plane_headers(o, "GET", "h:8080", "/b/k", "", "tok", 1369353600);  // 20130524T000000Z
  bool has_auth = false, has_date = false, has_sha = false;
  for (auto& h : signed_hdrs) {
    if (h.first == "Authorization") has_auth = h.second.find("SignedHeaders=host;x-amz-content-sha256;x-amz-date;x-amz-rdma-token") != std::string::npos;
    if (h.first == "x-amz-date") has_date = h.second == "20130524T000000Z";
    if (h.first == "x-amz-content-sha256") has_sha = h.second == kUnsignedPayload;
  }
  check(has_auth && has_date && has_sha, "E4", "signed requests sign the token and use UNSIGNED-PAYLOAD");
}

void test_repeated_and_managed_headers() {
  using namespace s3rdma;
  SigV4Request req;
  req.method = "GET";
  req.host = "h";
  req.path = "/";
  req.headers = {{"X-Amz-Meta-Label", "z"}, {"x-amz-meta-label", "a"}};
  auto r = sigv4_sign(req, aws_example_creds(), "us-east-1", "s3", "20130524T000000Z");
  check(r.canonical_request.find("x-amz-meta-label:z,a\n") != std::string::npos, "H1",
        "repeated names join values in wire order");
  check(r.canonical_request.find("host;x-amz-content-sha256;x-amz-date;x-amz-meta-label\n") !=
            std::string::npos,
        "H1", "repeated names appear once in SignedHeaders");
  for (const char* managed : {"Host", "x-amz-date", "X-Amz-Content-Sha256", "x-amz-security-token", "Authorization"}) {
    SigV4Request bad = req;
    bad.headers = {{managed, "v"}};
    bool threw = false;
    try { sigv4_sign(bad, aws_example_creds(), "us-east-1", "s3", "20130524T000000Z"); }
    catch (const std::invalid_argument&) { threw = true; }
    check(threw, "H2", "managed headers supplied by the caller are rejected");
  }
}

// Independent oracle: the same request signed by a separate Python
// implementation (hmac/hashlib/urllib.parse) gave this signature.
void test_combined_vector() {
  using namespace s3rdma;
  SigV4Credentials creds = aws_example_creds();
  creds.session_token = "SESSIONTOKENEXAMPLE";
  SigV4Request req;
  req.method = "HEAD";
  req.host = "gateway.example:8080";
  req.path = "/elbencho-ec-rdma/obj 1+\xc3\xa9";
  req.headers = {{"Range", "bytes=0-4194303"}, {kHdrRdmaToken, "7f0a:400000:1234:0:5:1:fe80"}};
  auto r = sigv4_sign(req, creds, "formioregion", "s3", "20261103T120000Z");
  check(r.canonical_request.rfind("HEAD\n/elbencho-ec-rdma/obj%201%2B%C3%A9\n\n", 0) == 0, "C1",
        "combined canonical request starts with method, encoded path and empty query");
  check(r.canonical_request.find("host;range;x-amz-content-sha256;x-amz-date;x-amz-rdma-token;x-amz-security-token\nUNSIGNED-PAYLOAD") != std::string::npos,
        "C1", "combined signed-header list and unsigned payload");
  check(r.signature == "4c96201c75fbd6d4e2ca0b89f130b0787820aaf5766dc6cf37f911fbb13a3eea", "C1",
        "combined HEAD signature matches the independent oracle");
  bool token_header = false;
  for (auto& h : r.headers) token_header |= (h.first == "x-amz-security-token" && h.second == creds.session_token);
  check(token_header, "C1", "session token is emitted as a wire header");
}

// Making SHA-256 unavailable through OpenSSL's default property query must
// surface as an exception, not as an empty or indeterminate digest.
void test_crypto_failure_is_loud() {
  using namespace s3rdma;
  EVP_set_default_properties(nullptr, "provider=s3rdma-no-such-provider");
  bool sha_threw = false, hmac_threw = false;
  try { sha256_hex("x", 1); } catch (const std::runtime_error&) { sha_threw = true; }
  try { sigv4_detail::hmac_sha256("k", "d"); } catch (const std::runtime_error&) { hmac_threw = true; }
  EVP_set_default_properties(nullptr, "");
  check(sha_threw, "F1", "SHA256 failure throws");
  check(hmac_threw, "F1", "HMAC-SHA256 failure throws");
  check(sha256_hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "F1",
        "digests work again after the property query is restored");
}

void test_reply_parsing() {
  using namespace s3rdma;
  using K = RdmaReply::Kind;
  check(parse_rdma_reply(false, "").kind == K::Declined, "R1", "absent reply = declined");
  check(parse_rdma_reply(true, "").kind == K::Declined, "R1", "empty reply = declined");
  check(parse_rdma_reply(true, "501").kind == K::Declined, "R1", "501 = declined");
  auto ok = parse_rdma_reply(true, "200");
  check(ok.kind == K::Code && ok.code == 200, "R2", "200 parses as a code");
  check(parse_rdma_reply(true, "200xyz").kind == K::Malformed, "R3", "trailing text is malformed");
  check(parse_rdma_reply(true, " 200").kind == K::Malformed, "R3", "leading space is malformed");
  check(parse_rdma_reply(true, "+200").kind == K::Malformed, "R3", "sign is malformed");
  check(parse_rdma_reply(true, "99").kind == K::Malformed, "R3", "out-of-range code is malformed");
  check(parse_rdma_reply(true, "600").kind == K::Malformed, "R3", "out-of-range code is malformed");
  check(parse_rdma_reply(true, "100").code == 100 && parse_rdma_reply(true, "599").code == 599, "R2",
        "boundary codes parse");
  check(parse_rdma_reply(true, "204").code == 204 && parse_rdma_reply(true, "206").code == 206, "R2",
        "partial-content codes parse");
  check(parse_rdma_reply(true, "000200").kind == K::Code && parse_rdma_reply(true, "000200").code == 200, "R4",
        "leading zeros do not change the code");
  check(parse_rdma_reply(true, "000501").kind == K::Declined, "R4", "leading-zero 501 is still declined");
  check(parse_rdma_reply(true, "000599").code == 599, "R4", "leading-zero boundary parses");
  check(parse_rdma_reply(true, "99999999999999999999").kind == K::Malformed, "R3", "huge values are malformed, not overflowed");

  check(parse_rdma_bytes_transferred(false, "", 10) == 0, "B1", "absent count = 0");
  check(parse_rdma_bytes_transferred(true, "7", 10) == 7, "B2", "count within buffer");
  check(parse_rdma_bytes_transferred(true, "10", 10) == 10, "B2", "count equal to buffer");
  check(parse_rdma_bytes_transferred(true, "11", 10) == -1, "B3", "count above buffer is invalid");
  check(parse_rdma_bytes_transferred(true, "7x", 10) == -1, "B3", "non-digit count is invalid");
  check(parse_rdma_bytes_transferred(true, "99999999999999999999", 10) == -1, "B3", "oversized count is invalid");
  check(parse_rdma_bytes_transferred(true, "", 10) == 0, "B1", "present-but-empty count = 0");
  check(parse_rdma_bytes_transferred(true, "0", 0) == 0, "B2", "explicit zero with zero capacity");
  check(parse_rdma_bytes_transferred(true, "000000000000000000007", 10) == 7, "B4", "leading zeros do not change the count");
  check(parse_rdma_bytes_transferred(true, "-1", 10) == -1, "B3", "negative count is invalid");
  check(parse_rdma_bytes_transferred(true, "+1", 10) == -1, "B3", "signed count is invalid");
  check(parse_rdma_bytes_transferred(true, " 7", 10) == -1, "B3", "whitespace count is invalid");
  check(parse_rdma_bytes_transferred(true, std::to_string(LONG_MAX), SIZE_MAX) == LONG_MAX, "B5",
        "LONG_MAX is the largest representable count");
  check(parse_rdma_bytes_transferred(true, "9223372036854775808", SIZE_MAX) == -1, "B5",
        "LONG_MAX + 1 is rejected even when the buffer allows it");
}

}  // namespace

int main() {
  test_get_object_vector();
  test_lifecycle_vector();
  test_list_objects_vector();
  test_encoding_and_headers();
  test_repeated_and_managed_headers();
  test_combined_vector();
  test_reply_parsing();
  test_crypto_failure_is_loud();
  if (g_failed == 0) std::cout << "SELFTEST PASS " << g_total << '/' << g_total << '\n';
  else std::cout << "SELFTEST FAIL " << g_failed << '/' << g_total << '\n';
  return g_failed == 0 ? 0 : 1;
}

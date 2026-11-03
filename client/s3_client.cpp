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

#include "s3_client.hpp"

#include <cstdio>

#include <httplib.h>

#include "cuobj_check.hpp"
#include "s3_protocol.hpp"
#include "s3_util.hpp"

namespace s3rdma {

namespace {

// cuObject GET callback body. Runs the control-path HTTP request: it carries
// the RDMA descriptor in the token header and asks the server to RDMA_WRITE
// into our buffer, and returns the number of bytes the server reports having
// transferred.
// Legacy Base64 reports size on HTTP 200/206. Verbatim GETs require matching
// HTTP/RDMA status and use the declared count; absent/empty counts mean zero.
ssize_t get_callback_impl(const void* handle, char* ptr, size_t size, loff_t offset,
                          const cufileRDMAInfo_t* rdma) {
  (void)ptr;
  (void)offset;
  auto* ctx = static_cast<GetContext*>(cuObjClient::getCtx(handle));
  if (!ctx || !rdma || !ctx->proto) {
    LOG_ERROR("get_callback: missing ctx/rdma info");
    return -1;
  }
  const ProtocolOptions& proto = *ctx->proto;

  std::string target;
  httplib::Headers headers;
  try {
    const std::string token = encode_rdma_token(rdma->desc_str, rdma->desc_len, proto.token_encoding);
    target = control_plane_target(proto, ctx->path);
    for (auto& h : control_plane_headers(proto, "GET", http_host_header(ctx->host, ctx->port),
                                         ctx->path, ctx->range, token))
      headers.emplace(h.first, h.second);
    if (token.empty() && proto.token_encoding == TokenEncoding::Base64 &&
        proto.auth == AuthMode::None)
      headers.emplace(kHdrRdmaToken, "");
  } catch (const std::exception& e) {
    LOG_ERROR("GET %s: cannot build request: %s", ctx->path.c_str(), e.what());
    return -1;
  }

  httplib::Client cli(ctx->host, ctx->port);
  auto res = cli.Get(target, headers);
  if (!res) {
    LOG_ERROR("HTTP GET %s failed: %s", ctx->path.c_str(),
              httplib::to_string(res.error()).c_str());
    return -1;
  }
  if (res->status != 200 && res->status != 206) {
    LOG_ERROR("HTTP GET %s -> %d", ctx->path.c_str(), res->status);
    return -1;
  }
  if (proto.token_encoding == TokenEncoding::Base64) return static_cast<ssize_t>(size);

  const bool reply_present = res->has_header(kHdrRdmaReply);
  const RdmaReply reply = parse_rdma_reply(reply_present, res->get_header_value(kHdrRdmaReply));
  if (reply.kind != RdmaReply::Kind::Code || reply.code != res->status) {
    LOG_ERROR("GET %s: endpoint did not complete the RDMA transfer (%s=%s)", ctx->path.c_str(),
              kHdrRdmaReply, reply_present ? res->get_header_value(kHdrRdmaReply).c_str() : "<absent>");
    return -1;
  }
  const bool count_present = res->has_header(kHdrRdmaBytesTransferred);
  const long declared = parse_rdma_bytes_transferred(
      count_present, res->get_header_value(kHdrRdmaBytesTransferred), size);
  if (declared < 0) {
    LOG_ERROR("GET %s: invalid %s", ctx->path.c_str(), kHdrRdmaBytesTransferred);
    return -1;
  }
  return static_cast<ssize_t>(declared);
}

// Nothing may leave the callback through the SDK's frames.
ssize_t get_callback(const void* handle, char* ptr, size_t size, loff_t offset,
                     const cufileRDMAInfo_t* rdma) noexcept {
  try {
    return get_callback_impl(handle, ptr, size, offset, rdma);
  } catch (...) {
    std::fputs("get_callback: exception\n", stderr);
    return -1;
  }
}

}  // namespace

S3Client::S3Client(std::string host, int port, ProtocolOptions proto)
    : host_(std::move(host)), port_(port), proto_(std::move(proto)) {
  ops_.get = &get_callback;
  ops_.put = nullptr;  // GET-only demo
  // NOTE: opens the cuFile RDMA path — requires GDS + Mellanox NIC.
  client_ = new cuObjClient(ops_, CUOBJ_PROTO_RDMA_DC_V1);
}

S3Client::~S3Client() { delete client_; }

ssize_t S3Client::get(void* ptr, size_t size, const std::string& bucket,
                      const std::string& object, const std::string& range) {
  CUOBJ_CHECK(client_->cuMemObjGetDescriptor(ptr, size));

  GetContext ctx;
  ctx.host = host_;
  ctx.port = port_;
  ctx.path = "/" + bucket + "/" + object;
  ctx.range = range;
  ctx.proto = &proto_;

  ssize_t n = -1;
  try {
    n = client_->cuObjGet(&ctx, ptr, size);
    if (n < 0) LOG_ERROR("cuObjGet failed (rc=%zd)", n);
  } catch (...) {
    LOG_ERROR("cuObjGet threw an exception");
  }
  client_->cuMemObjPutDescriptor(ptr);
  return n;
}

void S3Client::register_buffer(void* ptr, size_t size) {
  CUOBJ_CHECK(client_->cuMemObjGetDescriptor(ptr, size));
}

void S3Client::deregister_buffer(void* ptr) {
  client_->cuMemObjPutDescriptor(ptr);
}

ssize_t S3Client::get_registered(void* ptr, size_t size,
                                  const std::string& bucket,
                                  const std::string& object,
                                  const std::string& range) {
  GetContext ctx;
  ctx.host = host_;
  ctx.port = port_;
  ctx.path = "/" + bucket + "/" + object;
  ctx.range = range;
  ctx.proto = &proto_;

  ssize_t n = -1;
  try {
    n = client_->cuObjGet(&ctx, ptr, size);
    if (n < 0) LOG_ERROR("cuObjGet failed (rc=%zd)", n);
  } catch (...) {
    LOG_ERROR("cuObjGet threw an exception");
  }
  return n;
}

}  // namespace s3rdma

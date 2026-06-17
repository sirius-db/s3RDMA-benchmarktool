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

#include <httplib.h>

#include "base64.hpp"
#include "cuobj_check.hpp"
#include "s3_util.hpp"

namespace s3rdma {

namespace {

// cuObject GET callback. Runs the fake-S3 control-path HTTP request: it encodes
// the RDMA descriptor into a header and asks the server to RDMA_WRITE into our
// buffer. Returns the number of bytes the server reports having transferred.
ssize_t get_callback(const void* handle, char* ptr, size_t size, loff_t offset,
                      const cufileRDMAInfo_t* rdma) {
  auto* ctx = static_cast<GetContext*>(cuObjClient::getCtx(handle));
  if (!ctx || !rdma) {
    LOG_ERROR("get_callback: missing ctx/rdma info");
    return -1;
  }

  httplib::Headers headers = {
      {kHdrRdmaToken, base64_encode(rdma->desc_str, rdma->desc_len)},
  };
  if (!ctx->range.empty()) headers.emplace("Range", ctx->range);

  httplib::Client cli(ctx->host, ctx->port);
  auto res = cli.Get(ctx->path, headers);
  if (!res) {
    LOG_ERROR("HTTP GET %s failed: %s", ctx->path.c_str(),
              httplib::to_string(res.error()).c_str());
    return -1;
  }
  if (res->status != 200 && res->status != 206) {
    LOG_ERROR("HTTP GET %s -> %d", ctx->path.c_str(), res->status);
    return -1;
  }
  (void)offset;
  return static_cast<ssize_t>(size);
}

}  // namespace

S3Client::S3Client(std::string host, int port)
    : host_(std::move(host)), port_(port) {
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

// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/data_reduction_proxy/core/browser/data_reduction_proxy_data.h"

#include <memory>

#include "base/memory/ptr_util.h"
#include "base/message_loop/message_loop.h"
#include "net/base/request_priority.h"
#include "net/url_request/url_request.h"
#include "net/url_request/url_request_context.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace data_reduction_proxy {

namespace {

class DataReductionProxyDataTest : public testing::Test {
 public:
  DataReductionProxyDataTest() {}

 private:
  base::MessageLoopForIO message_loop_;
};

TEST_F(DataReductionProxyDataTest, BasicSettersAndGetters) {
  std::unique_ptr<DataReductionProxyData> data(new DataReductionProxyData());
  EXPECT_FALSE(data->used_data_reduction_proxy());
  data->set_used_data_reduction_proxy(true);
  EXPECT_TRUE(data->used_data_reduction_proxy());
  data->set_used_data_reduction_proxy(false);
  EXPECT_FALSE(data->used_data_reduction_proxy());

  EXPECT_FALSE(data->lofi_requested());
  data->set_lofi_requested(true);
  EXPECT_TRUE(data->lofi_requested());
  data->set_lofi_requested(false);
  EXPECT_FALSE(data->lofi_requested());
}

TEST_F(DataReductionProxyDataTest, AddToURLRequest) {
  std::unique_ptr<net::URLRequestContext> context(new net::URLRequestContext());
  std::unique_ptr<net::URLRequest> fake_request(context->CreateRequest(
      GURL("http://www.google.com"), net::RequestPriority::IDLE, nullptr));
  DataReductionProxyData* data =
      DataReductionProxyData::GetData(*fake_request.get());
  EXPECT_TRUE(!data);
  data =
      DataReductionProxyData::GetDataAndCreateIfNecessary(fake_request.get());
  EXPECT_FALSE(!data);
  data = DataReductionProxyData::GetData(*fake_request.get());
  EXPECT_FALSE(!data);
  DataReductionProxyData* data2 =
      DataReductionProxyData::GetDataAndCreateIfNecessary(fake_request.get());
  EXPECT_EQ(data, data2);
}

TEST_F(DataReductionProxyDataTest, DeepCopy) {
  const struct {
    bool data_reduction_used;
    bool lofi_on;
  } tests[] = {
      {
          false, true,
      },
      {
          false, false,
      },
      {
          true, false,
      },
      {
          true, true,
      },
  };

  for (size_t i = 0; i < arraysize(tests); ++i) {
    std::unique_ptr<DataReductionProxyData> data(new DataReductionProxyData());
    data->set_used_data_reduction_proxy(tests[i].data_reduction_used);
    data->set_lofi_requested(tests[i].lofi_on);
    std::unique_ptr<DataReductionProxyData> copy = data->DeepCopy();
    EXPECT_EQ(tests[i].lofi_on, copy->lofi_requested());
    EXPECT_EQ(tests[i].data_reduction_used, copy->used_data_reduction_proxy());
  }
}

}  // namespace

}  // namespace data_reduction_proxy

/*******************************************************************************
The MIT License (MIT)

Copyright (c) 2015 Dmitry "Dima" Korolev <dmitry.korolev@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*******************************************************************************/

#ifndef BRICKS_NET_API_DOCU_SERVER_03_TEST_CC
#define BRICKS_NET_API_DOCU_SERVER_03_TEST_CC

#include "../../api.h"
#include "../../../../strings/printf.h"
#include "../../../../dflags/dflags.h"
#include "../../../../3party/gtest/gtest-main-with-dflags.h"

DEFINE_int32(docu_net_server_port_03, 8082, "Okay to keep the same as in net/api/test.cc");

using namespace bricks::net::api;
using bricks::strings::Printf;
using bricks::net::HTTPHeaders;

TEST(Docu, HTTPServer03) {
const auto port = FLAGS_docu_net_server_port_03;
HTTP(port).ResetAllHandlers();
  // Constructing a more complex response.
  HTTP(port).Register("/found", [](Request r) {
    r("Yes.",
      HTTPResponseCode.Accepted,
      "text/html",
      HTTPHeaders({{"custom", "header"}}));  // TODO(dkorolev): Dot notation.
  });
EXPECT_EQ("Yes.", HTTP(GET(Printf("localhost:%d/found", port))).body);
}

#endif  // BRICKS_NET_API_DOCU_SERVER_03_TEST_CC

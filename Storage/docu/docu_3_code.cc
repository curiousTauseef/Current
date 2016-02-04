/*******************************************************************************
The MIT License (MIT)

Copyright (c) 2015 Dmitry "Dima" Korolev <dmitry.korolev@gmail.com>
          (c) 2015 Maxim Zhurovich <zhurovich@gmail.com>

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

#ifndef CURRENT_STORAGE_DOCU_DOCU_3_CODE_CC
#define CURRENT_STORAGE_DOCU_DOCU_3_CODE_CC

#define CURRENT_MOCK_TIME

#include "../../port.h"

#include "../storage.h"
#include "../api.h"
#include "../rest/hypermedia.h"
#include "../rest/advanced_hypermedia.h"
#include "../persister/sherlock.h"

#include "../../Blocks/HTTP/api.h"

#include "../../Bricks/dflags/dflags.h"

#include "../../3rdparty/gtest/gtest.h"

DEFINE_int32(client_storage_test_port, PickPortForUnitTest(), "");

namespace storage_docu {

CURRENT_ENUM(ClientID, uint64_t) { INVALID = 0ull };

CURRENT_STRUCT(BriefClient) {
  CURRENT_FIELD(key, ClientID);
  CURRENT_FIELD(name, std::string, "John Doe");
  CURRENT_CONSTRUCTOR(BriefClient)(ClientID key = ClientID::INVALID) : key(key) {}

  void InitializeOwnKey() {
    key = static_cast<ClientID>(std::hash<std::string>()(name));
  }
};

CURRENT_STRUCT(Client, BriefClient) {
  CURRENT_FIELD(white, bool, true);
  CURRENT_FIELD(straight, bool, true);
  CURRENT_FIELD(male, bool, true);

  CURRENT_CONSTRUCTOR(Client)(ClientID key = ClientID::INVALID) : SUPER(key) {}

  using T_BRIEF = BriefClient;
};

CURRENT_STORAGE_FIELD_ENTRY(OrderedDictionary, Client, PersistedClient);

CURRENT_STORAGE(StorageOfClients) {
  CURRENT_STORAGE_FIELD(client, PersistedClient);
};

}  // namespace storage_docu

TEST(StorageDocumentation, RESTifiedStorageExample) {
  using namespace storage_docu;
  using TestStorage = StorageOfClients<SherlockInMemoryStreamPersister>;

  TestStorage storage("storage_of_clients_dummy_stream_name");

  const auto rest1 = RESTfulStorage<TestStorage>(storage, FLAGS_client_storage_test_port, "/api1");
  const auto rest2 = RESTfulStorage<TestStorage, current::storage::rest::Hypermedia>(
      storage,
      FLAGS_client_storage_test_port,
      "/api2");
  const auto rest3 = RESTfulStorage<TestStorage, current::storage::rest::AdvancedHypermedia>(
      storage,
      FLAGS_client_storage_test_port,
      "/api3");

  const auto base_url = current::strings::Printf("http://localhost:%d", FLAGS_client_storage_test_port);

  // Top-level.
  {
    // Not exposed by default.
    const auto result = HTTP(GET(base_url + "/api1"));
    EXPECT_EQ(404, static_cast<int>(result.code));
  }
  {
    // Exposed by `Hypermedia`.
    {
      const auto result = HTTP(GET(base_url + "/api2"));
      EXPECT_EQ(200, static_cast<int>(result.code));
      EXPECT_EQ(base_url + "/healthz", ParseJSON<HypermediaRESTTopLevel>(result.body).url_healthz);
    }
    {
      const auto result = HTTP(GET(base_url + "/api2/healthz"));
      EXPECT_EQ(200, static_cast<int>(result.code));
      EXPECT_TRUE(ParseJSON<HypermediaRESTHealthz>(result.body).up);
    }
  }

  // GET an empty collection.
  {
    const auto result = HTTP(GET(base_url + "/api1/client"));
    EXPECT_EQ(200, static_cast<int>(result.code));
    EXPECT_EQ("", result.body);
  }
  {
    const auto result = HTTP(GET(base_url + "/api2/client"));
    EXPECT_EQ(200, static_cast<int>(result.code));
    EXPECT_EQ("{\"url\":\"http://localhost:19999/client\",\"data\":[]}\n", result.body);
  }

  // GET a non-existing resource.
  {
    const auto result = HTTP(GET(base_url + "/api1/client/42"));
    EXPECT_EQ(404, static_cast<int>(result.code));
    EXPECT_EQ("Nope.\n", result.body);
  }

  {
    const auto result = HTTP(GET(base_url + "/api2/client/42"));
    EXPECT_EQ(404, static_cast<int>(result.code));
    EXPECT_EQ("{\"error\":\"Resource not found.\"}\n", result.body);
  }

  // POST to a full resource-specifying URL, not allowed.
  {
    const auto result = HTTP(POST(base_url + "/api1/client/42", "blah"));
    EXPECT_EQ(400, static_cast<int>(result.code));
    EXPECT_EQ("Should not have resource key in the URL.\n", result.body);
  }

  {
    const auto result = HTTP(POST(base_url + "/api2/client/42", "blah"));
    EXPECT_EQ(400, static_cast<int>(result.code));
    EXPECT_EQ("{\"error\":\"Should not have resource key in the URL.\"}\n", result.body);
  }

  // POST a JSON not following the schema, not allowed.
  {
    const auto result = HTTP(POST(base_url + "/api1/client", "{\"trash\":true}"));
    EXPECT_EQ(400, static_cast<int>(result.code));
    EXPECT_EQ("Bad JSON.\n", result.body);
  }

  {
    const auto result = HTTP(POST(base_url + "/api2/client", "{\"trash\":true}"));
    EXPECT_EQ(400, static_cast<int>(result.code));
    EXPECT_EQ(
      "{"
      "\"error\":\"Invalid JSON in request body.\","
      "\"json_details\":\"Expected number for `key`, got: missing field.\""
      "}\n",
      result.body);
  }

  // POST another JSON not following the schema, still not allowed.
  {
    const auto result = HTTP(POST(base_url + "/api1/client", "{\"key\":[]}"));
    EXPECT_EQ(400, static_cast<int>(result.code));
    EXPECT_EQ("Bad JSON.\n", result.body);
  }

  {
    const auto result = HTTP(POST(base_url + "/api2/client", "{\"key\":[]}"));
    EXPECT_EQ(400, static_cast<int>(result.code));
    EXPECT_EQ(
      "{"
      "\"error\":\"Invalid JSON in request body.\","
      "\"json_details\":\"Expected number for `key`, got: []\""
      "}\n",
      result.body);
  }

  // POST a real piece.
  const auto post_response = HTTP(POST(base_url + "/api1/client", Client(ClientID(42))));
  const std::string client1_key_str = post_response.body;
  const ClientID client1_key = static_cast<ClientID>(FromString<uint64_t>(client1_key_str));
  EXPECT_EQ(201, static_cast<int>(post_response.code));

  // Now GET it via both APIs.
  {
    const auto result = HTTP(GET(base_url + "/api1/client/" + client1_key_str));
    EXPECT_EQ(200, static_cast<int>(result.code));
    EXPECT_EQ("{\"key\":" + client1_key_str + ",\"name\":\"John Doe\",\"white\":true,\"straight\":true,\"male\":true}\n", result.body);
  }

  {
    const auto result = HTTP(GET(base_url + "/api2/client/" + client1_key_str));
    EXPECT_EQ(200, static_cast<int>(result.code));
    EXPECT_EQ("{\"key\":" + client1_key_str + ",\"name\":\"John Doe\",\"white\":true,\"straight\":true,\"male\":true}\n", result.body);
  }

  // PUT an entry with the key different from URL is not allowed.
  EXPECT_EQ(400, static_cast<int>(HTTP(PUT(base_url + "/api1/client/42", Client(ClientID(64)))).code));
  EXPECT_EQ(400, static_cast<int>(HTTP(PUT(base_url + "/api2/client/42", Client(ClientID(64)))).code));

  // PUT a modified entry via both APIs.
  Client updated_client1((ClientID(client1_key)));
  updated_client1.name = "Jane Doe";
  EXPECT_EQ(200, static_cast<int>(HTTP(PUT(base_url + "/api1/client/" + client1_key_str, updated_client1)).code));
  updated_client1.male = false;
  EXPECT_EQ(200, static_cast<int>(HTTP(PUT(base_url + "/api2/client/" + client1_key_str, updated_client1)).code));

  // Check if both updates took place.
  {
    const auto result = HTTP(GET(base_url + "/api1/client/" + client1_key_str));
    EXPECT_EQ(200, static_cast<int>(result.code));
    EXPECT_EQ("{\"key\":" + client1_key_str + ",\"name\":\"Jane Doe\",\"white\":true,\"straight\":true,\"male\":false}\n", result.body);
  }

  // GET the whole collection.
  {
    const auto result = HTTP(GET(base_url + "/api1/client"));
    EXPECT_EQ(200, static_cast<int>(result.code));
    EXPECT_EQ(client1_key_str + '\n', result.body);
  }

  // PUT two more records and GET the collection again.
  EXPECT_EQ(201, static_cast<int>(HTTP(PUT(base_url + "/api1/client/101", Client(ClientID(101)))).code));
  EXPECT_EQ(201, static_cast<int>(HTTP(PUT(base_url + "/api1/client/102", Client(ClientID(102)))).code));
  {
    const auto result = HTTP(GET(base_url + "/api1/client"));
    EXPECT_EQ(200, static_cast<int>(result.code));
    EXPECT_EQ("101\n102\n" + client1_key_str + '\n', result.body);
  }
  {
    const auto result = HTTP(GET(base_url + "/api2/client"));
    EXPECT_EQ(200, static_cast<int>(result.code));
    EXPECT_EQ(
        "{"
        "\"url\":\"" + base_url + "/client\","
        "\"data\":[\"" +
        base_url + "/client/101\",\"" +
        base_url + "/client/102\",\"" +
        base_url + "/client/" + client1_key_str + "\"" +
        "]}\n",
        result.body);
  }
  {
    const auto result = HTTP(GET(base_url + "/api3/client"));
    EXPECT_EQ(200, static_cast<int>(result.code));
    // Shamelessly copy-pasted from the output. -- D.K.
    EXPECT_EQ("{\"url\":\"" + base_url + "/client?i=0&n=10\",\"url_directory\":\"" + base_url + "/client\",\"i\":0,\"n\":3,\"total\":3,\"url_next_page\":null,\"url_previous_page\":null,\"data\":[{\"url\":\"" + base_url + "/client/101\",\"url_full\":\"" + base_url + "/client/101\",\"url_brief\":\"" + base_url + "/client/101?fields=brief\",\"url_directory\":\"" + base_url + "/client\",\"data\":{\"key\":101,\"name\":\"John Doe\"}},{\"url\":\"" + base_url + "/client/102\",\"url_full\":\"" + base_url + "/client/102\",\"url_brief\":\"" + base_url + "/client/102?fields=brief\",\"url_directory\":\"" + base_url + "/client\",\"data\":{\"key\":102,\"name\":\"John Doe\"}},{\"url\":\"" + base_url + "/client/" + client1_key_str + "\",\"url_full\":\"" + base_url + "/client/" + client1_key_str + "\",\"url_brief\":\"" + base_url + "/client/" + client1_key_str + "?fields=brief\",\"url_directory\":\"" + base_url + "/client\",\"data\":{\"key\":" + client1_key_str + ",\"name\":\"Jane Doe\"}}]}",
        result.body);
  }

  // DELETE one record and GET the collection again.
  EXPECT_EQ(200, static_cast<int>(HTTP(DELETE(base_url + "/api1/client/" + client1_key_str)).code));
  {
    const auto result = HTTP(GET(base_url + "/api1/client"));
    EXPECT_EQ(200, static_cast<int>(result.code));
    EXPECT_EQ("101\n102\n", result.body);
  }
}

#endif  // CURRENT_STORAGE_DOCU_DOCU_3_CODE_CC

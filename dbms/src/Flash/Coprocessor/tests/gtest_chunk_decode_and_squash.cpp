// Copyright 2022 PingCAP, Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <DataStreams/SquashingTransform.h>
#include <DataTypes/DataTypesNumber.h>
#include <Flash/Coprocessor/CHBlockChunkCodec.h>
#include <Interpreters/Context.h>
#include <Storages/Transaction/TiDB.h>
#include <TestUtils/ColumnGenerator.h>
#include <TestUtils/FunctionTestUtils.h>
#include <TestUtils/TiFlashTestBasic.h>
#include <TestUtils/TiFlashTestEnv.h>
#include <gtest/gtest.h>

#include <Flash/Coprocessor/IChunkDecodeAndSquash.cpp>
namespace DB
{
namespace tests
{
class TestChunkDecodeAndSquash : public testing::Test
{
protected:
    void SetUp() override
    {
    }

public:
    TestChunkDecodeAndSquash()
        : context(TiFlashTestEnv::getContext())
    {}

    static Block squashBlocks(std::vector<Block> & blocks)
    {
        std::vector<Block> reference_block_vec;
        SquashingTransform squash_transform(std::numeric_limits<UInt64>::max(), 0, "");
        for (auto & block : blocks)
            squash_transform.add(std::move(block));
        Block empty;
        auto result = squash_transform.add(std::move(empty));
        return result.block;
    }

    // Return 10 Int64 column.
    static std::vector<tipb::FieldType> makeFields()
    {
        std::vector<tipb::FieldType> fields(10);
        for (int i = 0; i < 10; ++i)
        {
            fields[i].set_tp(TiDB::TypeLongLong);
            fields[i].set_flag(TiDB::ColumnFlagNotNull);
        }
        return fields;
    }

    static DAGSchema makeSchema()
    {
        auto fields = makeFields();
        DAGSchema schema;
        for (size_t i = 0; i < fields.size(); ++i)
        {
            ColumnInfo info = TiDB::fieldTypeToColumnInfo(fields[i]);
            schema.emplace_back(String("col") + std::to_string(i), std::move(info));
        }
        return schema;
    }

    // Return a block with **rows** and 10 Int64 column.
    static Block prepareBlock(size_t rows)
    {
        Block block;
        for (size_t i = 0; i < 10; ++i)
        {
            DataTypePtr int64_data_type = std::make_shared<DataTypeInt64>();
            auto int64_column = ColumnGenerator::instance().generate({rows, "Int64", RANDOM}).column;
            block.insert(ColumnWithTypeAndName{
                std::move(int64_column),
                int64_data_type,
                String("col") + std::to_string(i)});
        }
        return block;
    }

    void doTestWork(bool flush_something)
    {
        const size_t block_rows = 1024;
        const size_t block_num = 256;
        std::mt19937_64 rand_gen;
        // 1. Build Blocks.
        std::vector<Block> blocks;
        for (size_t i = 0; i < block_num; ++i)
        {
            UInt64 rows = flush_something ? static_cast<UInt64>(rand_gen()) % (block_rows * 4) : block_rows;
            blocks.emplace_back(prepareBlock(rows));
            if (flush_something)
                blocks.emplace_back(prepareBlock(0)); /// Adds this empty block, so even unluckily, total_rows % rows_limit == 0, it would flush an empty block with header
        }

        // 2. encode all blocks
        std::unique_ptr<ChunkCodecStream> codec_stream = std::make_unique<CHBlockChunkCodec>()->newCodecStream(makeFields());
        std::vector<String> encode_str_vec(block_num);
        for (const auto & block : blocks)
        {
            codec_stream->encode(block, 0, block.rows());
            encode_str_vec.push_back(codec_stream->getString());
            codec_stream->clear();
        }

        // 3. DecodeAndSquash all these blocks
        Block header = blocks.back();
        std::vector<Block> decoded_blocks;
        CHBlockChunkDecodeAndSquash decoder(header, block_rows * 4);
        for (const auto & str : encode_str_vec)
        {
            auto result = decoder.decodeAndSquash(str);
            if (result)
                decoded_blocks.push_back(std::move(result.value()));
        }
        auto last_block = decoder.flush();
        if (last_block)
            decoded_blocks.push_back(std::move(last_block.value()));
        /// flush after flush should return empty optional<block>
        ASSERT_TRUE(!decoder.flush());

        // 4. Check correctness
        Block reference_block = squashBlocks(blocks);
        Block decoded_block = squashBlocks(decoded_blocks);
        ASSERT_BLOCK_EQ(reference_block, decoded_block);
    }
    Context context;
};

TEST_F(TestChunkDecodeAndSquash, testDecodeAndSquash)
try
{
    doTestWork(true);
    doTestWork(false);
}
CATCH

} // namespace tests
} // namespace DB